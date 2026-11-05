// Unit tests for incremental index updates (LiveIndex). No elevation needed.
//
// The main test is a randomized equivalence check: a synthetic volume (the
// "truth") is changed by random operations; after every batch the index
// updated by LiveIndex::Apply must equal a full LayoutIndexBuilder build of
// the truth, compared by ID. Fixed seeds; the seed is printed on failure.

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/index.h"
#include "core/layout_index_builder.h"
#include "core/live_index.h"
#include "core/usn_parser.h"

namespace {

int g_failures = 0;
int g_checks = 0;
unsigned g_seed = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            std::printf("FAIL %s:%d (seed %u): %s\n", __FILE__, __LINE__, g_seed, #cond);    \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

using namespace dirsize;

constexpr std::uint64_t kSeq = 1ull << 48;
constexpr std::uint64_t kRecordMask = kSeq - 1;
constexpr std::uint64_t kRootId = 5 + kSeq;
constexpr std::uint64_t kExtendId = 11 + kSeq;  // a root folder the root listing does not show
constexpr std::uint64_t kMftId = 1 + kSeq;      // a root file the root listing does not show
constexpr std::uint32_t kCloudTag = 0x9000001Au;

struct Item {
    std::uint64_t id = 0;
    bool dir = false;
    std::vector<CurrentLink> links;
    std::uint64_t size = 0;
    std::uint32_t tag = 0;  // reparse tag, 0 = not a reparse point
    bool unlisted = false;  // the root listing does not show it (like a metafile)
};

struct Volume {
    std::map<std::uint64_t, Item> items;  // by full ID
    std::uint64_t nextRecord = 100;
    std::vector<std::uint64_t> freeRecords;  // record number | sequence of the deleted ID

    bool ListedInRoot(std::uint64_t id) const {
        if (id == kExtendId || id == kMftId) return false;
        auto it = items.find(id);
        if (it == items.end() || it->second.unlisted) return false;
        for (const CurrentLink& l : it->second.links) {
            if (l.parentId == kRootId) return true;
        }
        return false;
    }
    bool Tag(std::uint64_t id, std::uint32_t& tag) const {
        auto it = items.find(id);
        if (it == items.end()) return false;
        tag = it->second.tag;
        return true;
    }
    UpdateRules Rules() const {
        return {[this](std::uint64_t id) { return ListedInRoot(id); },
                [this](std::uint64_t id, std::uint32_t& tag) { return Tag(id, tag); }};
    }
    CurrentEntry Entry(const Item& it) const {
        CurrentEntry e;
        e.id = it.id;
        e.attributes = it.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
        if (it.tag != 0) e.attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
        e.links = it.links;
        e.logicalBytes = it.dir ? 0 : it.size;
        e.allocatedBytes = it.dir ? 0 : (it.size + 4095) / 4096 * 4096;
        return e;
    }
    VolumeIndex Build() const {
        LayoutIndexBuilder b;
        std::vector<CurrentEntry> entries;
        for (const auto& [id, it] : items) entries.push_back(Entry(it));
        for (const CurrentEntry& e : entries) {
            LayoutEntry l;
            l.id = e.id;
            l.attributes = e.attributes;
            for (const CurrentLink& k : e.links) l.links.push_back({k.parentId, k.name});
            l.hasData = (e.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
            l.dataLogicalBytes = e.logicalBytes;
            l.dataAllocatedBytes = e.allocatedBytes;
            b.Add(l);
        }
        std::vector<std::uint64_t> listing;
        for (const auto& [id, it] : items) {
            if (ListedInRoot(id)) listing.push_back(id);
        }
        return b.Finish(kRootId, listing, [this](std::uint64_t id, std::uint32_t& tag) { return Tag(id, tag); });
    }
    // What a FILEID-filtered layout query returns for these IDs: the item now
    // holding each MFT record number, whatever its sequence number.
    std::vector<CurrentEntry> Query(const std::vector<ChangedId>& changed) const {
        std::set<std::uint64_t> records;
        for (const ChangedId& c : changed) records.insert(c.id & kRecordMask);
        std::vector<CurrentEntry> out;
        for (const auto& [id, it] : items) {
            if (records.count(id & kRecordMask)) out.push_back(Entry(it));
        }
        return out;
    }
    std::uint64_t NewId() {
        if (!freeRecords.empty()) {
            const std::uint64_t old = freeRecords.back();
            freeRecords.pop_back();
            return (old & kRecordMask) | ((old & ~kRecordMask) + kSeq);  // reuse with the next sequence
        }
        return (nextRecord++) | kSeq;
    }
};

Volume BaseVolume() {
    Volume v;
    v.items[kRootId] = {kRootId, true, {{kRootId, L"."}}, 0, 0};
    v.items[kExtendId] = {kExtendId, true, {{kRootId, L"$Extend"}}, 0, 0};
    v.items[kMftId] = {kMftId, false, {{kRootId, L"$MFT"}}, 1000, 0};
    return v;
}

// ---- comparison ----------------------------------------------------------------

std::vector<std::uint64_t> CountedDirIds(const VolumeIndex& x, std::uint32_t fileIndex) {
    std::vector<std::uint64_t> ids{x.dirs[x.files[fileIndex].dir].id};
    for (const ExtraLink& l : x.extraLinks) {
        if (l.file == fileIndex) ids.push_back(x.dirs[l.dir].id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool SameIndex(const VolumeIndex& a, const VolumeIndex& b) {
    bool same = a.dirs.size() == b.dirs.size() && a.files.size() == b.files.size() &&
                a.dirs[a.root].id == b.dirs[b.root].id;
    if (!same) {
        std::printf("  sizes: dirs %zu/%zu files %zu/%zu\n", a.dirs.size(), b.dirs.size(), a.files.size(),
                    b.files.size());
        return false;
    }
    for (size_t k = 0; k < a.dirs.size(); ++k) {
        const DirEntry& x = a.dirs[k];
        const DirEntry& y = b.dirs[k];
        const std::uint64_t px = x.parent == kNoDir ? 0 : a.dirs[x.parent].id;
        const std::uint64_t py = y.parent == kNoDir ? 0 : b.dirs[y.parent].id;
        if (x.id != y.id || px != py || !(x.subtree == y.subtree) || a.Name(static_cast<DirIndex>(k)) !=
                                                                         b.Name(static_cast<DirIndex>(k))) {
            std::printf("  dir %llx differs (parent %llx/%llx, files %llu/%llu, folders %llu/%llu, bytes %llu/%llu)\n",
                        x.id, px, py, x.subtree.files, y.subtree.files, x.subtree.folders, y.subtree.folders,
                        x.subtree.logicalBytes, y.subtree.logicalBytes);
            return false;
        }
    }
    for (std::uint32_t k = 0; k < a.files.size(); ++k) {
        const FileEntry& x = a.files[k];
        const FileEntry& y = b.files[k];
        if (x.id != y.id || x.logicalBytes != y.logicalBytes || x.allocatedBytes != y.allocatedBytes ||
            x.countedLinks != y.countedLinks || CountedDirIds(a, k) != CountedDirIds(b, k)) {
            std::printf("  file %llx differs\n", x.id);
            return false;
        }
    }
    return true;
}

// The totals LiveIndex maintains must equal those recomputed from scratch.
bool MaintainedTotalsMatch(const LiveIndex& live, const VolumeIndex& compacted) {
    const VolumeIndex& x = live.Index();
    for (const DirEntry& d : compacted.dirs) {
        const DirIndex i = live.FindDir(d.id);
        if (!live.DirLive(i) || !(x.dirs[i].subtree == d.subtree)) {
            std::printf("  maintained totals differ for dir %llx\n", d.id);
            return false;
        }
    }
    return true;
}

// ---- random operations ------------------------------------------------------------

class Mutator {
public:
    Mutator(Volume& v, unsigned seed) : v_(v), rng_(seed) {}

    // Applies one random operation and records the IDs it touched.
    void Step(const VolumeIndex& visible, std::vector<ChangedId>& changed) {
        std::vector<std::uint64_t> files, dirs, visibleDirs;
        for (const auto& [id, it] : v_.items) {
            if (id == kRootId || id == kMftId || id == kExtendId) continue;
            (it.dir ? dirs : files).push_back(id);
        }
        // Visible at the start of the batch and not deleted since.
        // Hidden earlier in this batch are left out: moving one back would make
        // a folder visible again within the batch, which correctly asks for a rescan.
        for (const DirEntry& d : visible.dirs) {
            if (v_.items.count(d.id) && !hiddenInBatch_.count(d.id)) visibleDirs.push_back(d.id);
        }
        std::vector<std::uint64_t> anyDir = dirs;
        anyDir.push_back(kRootId);
        anyDir.push_back(kExtendId);
        auto pick = [&](const std::vector<std::uint64_t>& from) { return from[Rand(from.size())]; };
        // Mostly visible folders, so the index grows; sometimes any folder.
        auto target = [&]() { return Rand(5) == 0 ? pick(anyDir) : pick(visibleDirs); };
        auto touch = [&](std::uint64_t id, std::uint32_t why) { changed.push_back({id, why}); };

        switch (Rand(20)) {
            case 0:
            case 1:
            case 14:
            case 15:
            case 16:
            case 17: {  // create a file
                const std::uint64_t id = v_.NewId();
                v_.items[id] = {id, false, {{target(), Name(L"f")}}, Rand(100000), 0};
                if (Rand(8) == 0) {  // in the root but not in its listing; may also get links elsewhere
                    v_.items[id].links.front().parentId = kRootId;
                    v_.items[id].unlisted = true;
                }
                touch(id, USN_REASON_FILE_CREATE);
                break;
            }
            case 2:  // resize a file
                if (!files.empty()) {
                    const std::uint64_t id = pick(files);
                    v_.items[id].size = Rand(100000);
                    touch(id, USN_REASON_DATA_EXTEND);
                }
                break;
            case 3:  // delete a file
                if (!files.empty()) {
                    const std::uint64_t id = pick(files);
                    Delete(id);
                    touch(id, USN_REASON_FILE_DELETE);
                }
                break;
            case 4:  // add a hard link, sometimes in the same folder
                if (!files.empty()) {
                    Item& f = v_.items[pick(files)];
                    const std::uint64_t parent = Rand(4) == 0 ? f.links.front().parentId : target();
                    f.links.push_back({parent, Name(L"l")});
                    touch(f.id, USN_REASON_HARD_LINK_CHANGE);
                }
                break;
            case 5:  // remove a link of a file with several
                if (!files.empty()) {
                    Item& f = v_.items[pick(files)];
                    if (f.links.size() > 1) {
                        f.links.erase(f.links.begin() + static_cast<std::ptrdiff_t>(Rand(f.links.size())));
                        touch(f.id, USN_REASON_HARD_LINK_CHANGE);
                    }
                }
                break;
            case 6:  // move a file
                if (!files.empty()) {
                    Item& f = v_.items[pick(files)];
                    f.links[Rand(f.links.size())].parentId = target();
                    touch(f.id, USN_REASON_RENAME_NEW_NAME);
                }
                break;
            case 7:
            case 8:
            case 18:
            case 19: {  // create a folder, sometimes a cloud-file reparse point
                const std::uint64_t id = v_.NewId();
                v_.items[id] = {id, true, {{target(), Name(L"d")}}, 0, Rand(5) == 0 ? kCloudTag : 0u};
                if (Rand(10) == 0) {  // a hidden root folder, like $Extend
                    v_.items[id].links.front().parentId = kRootId;
                    v_.items[id].unlisted = true;
                }
                touch(id, USN_REASON_FILE_CREATE);
                break;
            }
            case 9:  // rename a folder
                if (!dirs.empty()) {
                    const std::uint64_t id = pick(dirs);
                    v_.items[id].links.front().name = Name(L"r");
                    touch(id, USN_REASON_RENAME_NEW_NAME);
                }
                break;
            case 10:  // move a visible folder anywhere outside its own subtree (also into $Extend)
                if (visibleDirs.size() > 1) {
                    const std::uint64_t id = pick(visibleDirs);
                    if (id == kRootId) break;
                    const std::uint64_t to = target();
                    if (InSubtree(to, id)) break;
                    v_.items[id].links.front().parentId = to;
                    touch(id, USN_REASON_RENAME_NEW_NAME);
                    if (std::find(visibleDirs.begin(), visibleDirs.end(), to) == visibleDirs.end()) HideSubtree(id);
                }
                break;
            case 11:  // delete an empty folder
                if (!dirs.empty()) {
                    const std::uint64_t id = pick(dirs);
                    if (HasChildren(id)) break;
                    Delete(id);
                    touch(id, USN_REASON_FILE_DELETE);
                }
                break;
            case 12:  // turn a visible folder into a junction (its contents disappear)
                if (visibleDirs.size() > 1 && Rand(4) == 0) {
                    const std::uint64_t id = pick(visibleDirs);
                    if (id == kRootId) break;
                    v_.items[id].tag = IO_REPARSE_TAG_MOUNT_POINT;
                    touch(id, USN_REASON_REPARSE_POINT_CHANGE);
                    HideSubtree(id);
                }
                break;
            case 13:  // swap two visible folders
                if (visibleDirs.size() > 3) {
                    const std::uint64_t a = pick(visibleDirs), b = pick(visibleDirs);
                    if (a == kRootId || b == kRootId || a == b) break;
                    const std::uint64_t pa = v_.items[a].links.front().parentId;
                    const std::uint64_t pb = v_.items[b].links.front().parentId;
                    if (InSubtree(pb, a) || InSubtree(pa, b)) break;
                    v_.items[a].links.front().parentId = pb;
                    v_.items[b].links.front().parentId = pa;
                    touch(a, USN_REASON_RENAME_NEW_NAME);
                    touch(b, USN_REASON_RENAME_NEW_NAME);
                }
                break;
        }
        // An unchanged ID now and then: applying it must change nothing.
        if (Rand(6) == 0 && !files.empty()) touch(pick(files), USN_REASON_BASIC_INFO_CHANGE);
    }

    void BeginBatch() { hiddenInBatch_.clear(); }

private:
    void HideSubtree(std::uint64_t top) {
        for (const auto& [id, it] : v_.items) {
            if (it.dir && InSubtree(id, top)) hiddenInBatch_.insert(id);
        }
    }
    size_t Rand(size_t n) { return std::uniform_int_distribution<size_t>(0, n - 1)(rng_); }
    std::wstring Name(const wchar_t* prefix) { return prefix + std::to_wstring(counter_++); }
    void Delete(std::uint64_t id) {
        v_.items.erase(id);
        v_.freeRecords.push_back(id);
    }
    bool HasChildren(std::uint64_t id) const {
        for (const auto& [k, it] : v_.items) {
            for (const CurrentLink& l : it.links) {
                if (l.parentId == id && k != id) return true;
            }
        }
        return false;
    }
    // True when `x` is `top` or below it, following first links.
    bool InSubtree(std::uint64_t x, std::uint64_t top) const {
        for (int guard = 0; guard < 10000; ++guard) {
            if (x == top) return true;
            if (x == kRootId) return false;
            auto it = v_.items.find(x);
            if (it == v_.items.end() || it->second.links.empty()) return false;
            x = it->second.links.front().parentId;
        }
        return true;
    }

    Volume& v_;
    std::mt19937 rng_;
    unsigned counter_ = 0;
    std::set<std::uint64_t> hiddenInBatch_;
};

void TestRandomEquivalence() {
    UpdateResult sum;
    size_t maxDirs = 0, maxFiles = 0, maxExtra = 0;
    for (unsigned seed = 1; seed <= 60; ++seed) {
        g_seed = seed;
        Volume v = BaseVolume();
        LiveIndex live(v.Build());
        Mutator m(v, seed);
        bool ok = true;
        for (int batch = 0; batch < 300 && ok; ++batch) {
            const VolumeIndex before = live.Compacted();
            std::vector<ChangedId> changed;
            m.BeginBatch();
            const int ops = 1 + static_cast<int>(batch % 5);
            for (int k = 0; k < ops; ++k) m.Step(before, changed);
            const UpdateResult r = live.Apply(changed, v.Query(changed), v.Rules());
            if (r.status != UpdateStatus::Applied) {
                std::printf("  seed %u batch %d: rescan requested: %ls\n", seed, batch, r.reason);
                const VolumeIndex after = v.Build();
                auto has = [](const VolumeIndex& x, std::uint64_t id) {
                    for (const DirEntry& d : x.dirs) {
                        if (d.id == id) return true;
                    }
                    return false;
                };
                for (const ChangedId& c : changed) {
                    auto it = v.items.find(c.id);
                    const bool exists = it != v.items.end();
                    std::printf("    id %llx reasons %x before %d after %d exists %d dir %d parent %llx unlisted %d "
                                "tag %x\n",
                                c.id, c.reasons, has(before, c.id), has(after, c.id), exists, exists && it->second.dir,
                                exists && !it->second.links.empty() ? it->second.links.front().parentId : 0ull,
                                exists && it->second.unlisted, exists ? it->second.tag : 0u);
                }
                ok = false;
                break;
            }
            sum.filesChanged += r.filesChanged;
            sum.foldersCreated += r.foldersCreated;
            sum.foldersMoved += r.foldersMoved;
            sum.foldersRemoved += r.foldersRemoved;
            const VolumeIndex expected = v.Build();
            const VolumeIndex got = live.Compacted();
            maxDirs = std::max(maxDirs, got.dirs.size());
            maxFiles = std::max(maxFiles, got.files.size());
            maxExtra = std::max(maxExtra, got.extraLinks.size());
            if (!SameIndex(expected, got) || !MaintainedTotalsMatch(live, got)) {
                std::printf("  seed %u batch %d differs\n", seed, batch);
                ok = false;
            }
        }
        CHECK(ok);
    }
    g_seed = 0;
    std::printf("random: files changed %llu, folders created %llu, moved %llu, removed %llu; "
                "largest index %zu folders, %zu files, %zu extra links\n",
                sum.filesChanged, sum.foldersCreated, sum.foldersMoved, sum.foldersRemoved, maxDirs, maxFiles,
                maxExtra);
}

// ---- deterministic cases -----------------------------------------------------------

struct Fixture {
    Volume v = BaseVolume();
    std::uint64_t a = 0, b = 0, file = 0;
    Fixture() {
        a = v.NewId();
        b = v.NewId();
        file = v.NewId();
        v.items[a] = {a, true, {{kRootId, L"A"}}, 0, 0};
        v.items[b] = {b, true, {{a, L"B"}}, 0, 0};
        v.items[file] = {file, false, {{b, L"f"}}, 500, 0};
    }
};

void TestFolderBecomesVisibleNeedsRescan() {
    {  // moved out of $Extend
        Fixture f;
        f.v.items[f.a].links.front().parentId = kExtendId;
        LiveIndex live(f.v.Build());
        f.v.items[f.a].links.front().parentId = kRootId;
        std::vector<ChangedId> c{{f.a, USN_REASON_RENAME_NEW_NAME}};
        CHECK(live.Apply(c, f.v.Query(c), f.v.Rules()).status == UpdateStatus::NeedsRescan);
        CHECK(live.Broken());
        CHECK(live.Apply({}, {}, f.v.Rules()).status == UpdateStatus::NeedsRescan);
    }
    {  // junction removed
        Fixture f;
        f.v.items[f.a].tag = IO_REPARSE_TAG_MOUNT_POINT;
        LiveIndex live(f.v.Build());
        f.v.items[f.a].tag = 0;
        std::vector<ChangedId> c{{f.a, USN_REASON_REPARSE_POINT_CHANGE}};
        CHECK(live.Apply(c, f.v.Query(c), f.v.Rules()).status == UpdateStatus::NeedsRescan);
    }
}

void TestInconsistentBatchesNeedRescan() {
    {  // folder reported deleted while a file is still in it
        Fixture f;
        LiveIndex live(f.v.Build());
        f.v.items.erase(f.b);
        std::vector<ChangedId> c{{f.b, USN_REASON_FILE_DELETE}};
        const UpdateResult r = live.Apply(c, f.v.Query(c), f.v.Rules());
        CHECK(r.status == UpdateStatus::NeedsRescan);
    }
    {  // two folders that are each other's parent
        Fixture f;
        LiveIndex live(f.v.Build());
        f.v.items[f.a].links.front().parentId = f.b;
        std::vector<ChangedId> c{{f.a, USN_REASON_RENAME_NEW_NAME}};
        CHECK(live.Apply(c, f.v.Query(c), f.v.Rules()).status == UpdateStatus::NeedsRescan);
    }
    {  // malformed entry
        Fixture f;
        LiveIndex live(f.v.Build());
        std::vector<ChangedId> c{{f.file, USN_REASON_DATA_EXTEND}};
        std::vector<CurrentEntry> e = f.v.Query(c);
        e.front().malformed = true;
        CHECK(live.Apply(c, e, f.v.Rules()).status == UpdateStatus::NeedsRescan);
    }
}

// B moves out of A in the same batch in which A becomes a junction: B must
// stay counted, A's other contents must leave the totals.
void TestMoveOutThenHideParent() {
    Fixture f;
    const std::uint64_t c = f.v.NewId();
    f.v.items[c] = {c, true, {{f.a, L"C"}}, 0, 0};
    const std::uint64_t g = f.v.NewId();
    f.v.items[g] = {g, false, {{c, L"g"}}, 300, 0};
    LiveIndex live(f.v.Build());
    f.v.items[f.b].links.front().parentId = kRootId;
    f.v.items[f.a].tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::vector<ChangedId> ch{{f.a, USN_REASON_REPARSE_POINT_CHANGE}, {f.b, USN_REASON_RENAME_NEW_NAME}};
    CHECK(live.Apply(ch, f.v.Query(ch), f.v.Rules()).status == UpdateStatus::Applied);
    CHECK(SameIndex(f.v.Build(), live.Compacted()));
    CHECK(MaintainedTotalsMatch(live, live.Compacted()));
    const VolumeIndex x = live.Compacted();
    CHECK(x.dirs[x.root].subtree.files == 1 && x.dirs[x.root].subtree.logicalBytes == 500);
}

void TestRecordReuse() {
    Fixture f;
    LiveIndex live(f.v.Build());
    // The file is deleted and its MFT record reused by a new file in A; only
    // the old ID is in the change list.
    const std::uint64_t old = f.file;
    f.v.items.erase(old);
    const std::uint64_t reused = (old & kRecordMask) | ((old & ~kRecordMask) + kSeq);
    f.v.items[reused] = {reused, false, {{f.a, L"g"}}, 77, 0};
    std::vector<ChangedId> c{{old, USN_REASON_FILE_DELETE}};
    CHECK(live.Apply(c, f.v.Query(c), f.v.Rules()).status == UpdateStatus::Applied);
    CHECK(SameIndex(f.v.Build(), live.Compacted()));
    CHECK(live.FindFile(reused) != kNoFile);
}

void TestRepeatedApplyIsIdempotent() {
    Fixture f;
    LiveIndex live(f.v.Build());
    f.v.items[f.file].size = 9999;
    std::vector<ChangedId> c{{f.file, USN_REASON_DATA_EXTEND}, {f.a, 0}, {f.b, 0}};
    for (int i = 0; i < 3; ++i) CHECK(live.Apply(c, f.v.Query(c), f.v.Rules()).status == UpdateStatus::Applied);
    CHECK(SameIndex(f.v.Build(), live.Compacted()));
    CHECK(MaintainedTotalsMatch(live, live.Compacted()));
}

// ---- USN buffer parser ---------------------------------------------------------------

struct UsnBuffer {
    std::vector<std::byte> bytes;
    explicit UsnBuffer(std::int64_t next) { Append(&next, sizeof(next)); }
    void Append(const void* p, size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
    // Appends a version-2 record; returns its offset.
    size_t Add(std::uint64_t id, std::uint64_t parent, std::uint32_t reason, const std::wstring& name) {
        const size_t offset = bytes.size();
        const size_t fixed = offsetof(USN_RECORD_V2, FileName);
        const size_t length = (fixed + name.size() * 2 + 7) / 8 * 8;
        USN_RECORD_V2 r{};
        r.RecordLength = static_cast<DWORD>(length);
        r.MajorVersion = 2;
        r.FileReferenceNumber = id;
        r.ParentFileReferenceNumber = parent;
        r.Usn = static_cast<USN>(offset);
        r.Reason = reason;
        r.FileNameLength = static_cast<WORD>(name.size() * 2);
        r.FileNameOffset = static_cast<WORD>(fixed);
        Append(&r, fixed);
        Append(name.data(), name.size() * 2);
        bytes.resize(offset + length);
        return offset;
    }
    template <typename T>
    void Poke(size_t offset, T value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }
};

UsnParseStatus Parse(const UsnBuffer& b, std::vector<UsnChange>& out, std::int64_t& next, size_t size) {
    out.clear();
    return ParseUsnBuffer(b.bytes.data(), size, next, [&](const UsnChange& c) { out.push_back(c); });
}

void TestUsnParser() {
    UsnBuffer b(12345);
    const size_t r1 = b.Add(0x0002000000000100ull, 0x0001000000000005ull, USN_REASON_FILE_CREATE, L"a.txt");
    const size_t r2 = b.Add(0x0003000000000101ull, 0x0002000000000100ull, USN_REASON_CLOSE, L"longer name.bin");
    std::vector<UsnChange> out;
    std::int64_t next = 0;
    CHECK(Parse(b, out, next, b.bytes.size()) == UsnParseStatus::Ok);
    CHECK(next == 12345 && out.size() == 2);
    CHECK(out.size() == 2 && out[0].id == 0x0002000000000100ull && out[0].parentId == 0x0001000000000005ull &&
          out[0].reason == USN_REASON_FILE_CREATE && out[1].reason == USN_REASON_CLOSE);

    // Only the USN, no records: no more changes.
    UsnBuffer empty(777);
    CHECK(Parse(empty, out, next, empty.bytes.size()) == UsnParseStatus::Ok && out.empty() && next == 777);

    // Every truncation: either ends on a record boundary (Ok) or is malformed.
    bool truncationOk = true;
    for (size_t size = 0; size < b.bytes.size(); ++size) {
        const UsnParseStatus s = Parse(b, out, next, size);
        const bool boundary = size == 8 || size == r2;
        if (size < 8 && s != UsnParseStatus::Malformed) truncationOk = false;
        if (size >= 8 && boundary != (s == UsnParseStatus::Ok)) truncationOk = false;
    }
    CHECK(truncationOk);
    (void)r1;

    auto corrupted = [&](auto&& change) {
        UsnBuffer c = b;
        change(c);
        return Parse(c, out, next, c.bytes.size());
    };
    const size_t lengthAt = r2 + offsetof(USN_RECORD_V2, RecordLength);
    CHECK(corrupted([&](UsnBuffer& c) { c.Poke<DWORD>(lengthAt, 0); }) == UsnParseStatus::Malformed);
    CHECK(corrupted([&](UsnBuffer& c) { c.Poke<DWORD>(lengthAt, 0x10000); }) == UsnParseStatus::Malformed);
    CHECK(corrupted([&](UsnBuffer& c) { c.Poke<DWORD>(lengthAt, 61); }) == UsnParseStatus::Malformed);
    CHECK(corrupted([&](UsnBuffer& c) { c.Poke<WORD>(r2 + offsetof(USN_RECORD_V2, MajorVersion), 3); }) ==
          UsnParseStatus::Malformed);
    CHECK(corrupted([&](UsnBuffer& c) { c.Poke<WORD>(r2 + offsetof(USN_RECORD_V2, FileNameLength), 0x2000); }) ==
          UsnParseStatus::Malformed);
    CHECK(corrupted([&](UsnBuffer& c) { c.Poke<WORD>(r2 + offsetof(USN_RECORD_V2, FileNameOffset), 4); }) ==
          UsnParseStatus::Malformed);
    // Records before the malformed one were still reported.
    corrupted([&](UsnBuffer& c) { c.Poke<DWORD>(lengthAt, 0); });
    CHECK(out.size() == 1);
    CHECK(ParseUsnBuffer(nullptr, 0, next, [](const UsnChange&) {}) == UsnParseStatus::Malformed);
}

void TestChangeHelpers() {
    ChangeCollector c;
    c.Add({0x0001000000000010ull, 0, 0, USN_REASON_FILE_CREATE, 0});
    c.Add({0x0001000000000010ull, 0, 0, USN_REASON_CLOSE, 0});
    c.Add({0x0002000000000010ull, 0, 0, USN_REASON_FILE_DELETE, 0});
    const std::vector<ChangedId> ids = c.Take();
    CHECK(ids.size() == 2 && ids[0].id == 0x0001000000000010ull &&
          ids[0].reasons == (USN_REASON_FILE_CREATE | USN_REASON_CLOSE));
    CHECK(c.Size() == 0);

    // Sequence numbers stripped, duplicates removed, neighbours merged.
    const auto r = RecordRanges({0x0005000000000012ull, 0x0001000000000010ull, 0x0002000000000010ull,
                                 0x0001000000000011ull, 0x0001000000000020ull});
    CHECK(r.size() == 2 && r[0].first == 0x10 && r[0].second == 0x12 && r[1].first == 0x20 && r[1].second == 0x20);
    CHECK(RecordRanges({}).empty());

    // Open files are tracked until a close or delete record.
    OpenFileTracker open;
    open.Observe({0x10, 5, 0, USN_REASON_DATA_EXTEND, FILE_ATTRIBUTE_ARCHIVE});
    open.Observe({0x11, 5, 0, USN_REASON_FILE_CREATE, FILE_ATTRIBUTE_ARCHIVE});
    open.Observe({0x12, 5, 0, USN_REASON_FILE_CREATE, FILE_ATTRIBUTE_DIRECTORY});
    CHECK(open.Size() == 2 && open.Ids().count(0x10) && !open.Ids().count(0x12));
    open.Observe({0x10, 5, 0, USN_REASON_DATA_EXTEND | USN_REASON_CLOSE, FILE_ATTRIBUTE_ARCHIVE});
    open.Observe({0x11, 5, 0, USN_REASON_FILE_DELETE, FILE_ATTRIBUTE_ARCHIVE});
    CHECK(open.Size() == 0);
    open.Observe({0x13, 5, 0, USN_REASON_DATA_OVERWRITE, FILE_ATTRIBUTE_ARCHIVE});
    open.Forget(0x13);
    CHECK(open.Size() == 0);
}

}  // namespace

int main() {
    TestUsnParser();
    TestChangeHelpers();
    TestRandomEquivalence();
    TestFolderBecomesVisibleNeedsRescan();
    TestInconsistentBatchesNeedRescan();
    TestMoveOutThenHideParent();
    TestRecordReuse();
    TestRepeatedApplyIsIdempotent();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
