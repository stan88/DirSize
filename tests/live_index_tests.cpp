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
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/cache_store.h"
#include "core/crc32.h"
#include "core/index.h"
#include "core/index_cache.h"
#include "core/layout_index_builder.h"
#include "core/live_index.h"
#include "core/usn_parser.h"

namespace {

int g_failures = 0;
int g_checks = 0;
unsigned g_seed = 0;
int g_roundTrips = 0;

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
            if (ok && batch == 150) {
                CacheMeta meta;
                meta.journalId = seed;
                meta.appliedUsn = 1000 + batch;
                meta.openFileIds = {7, 9};
                const std::vector<std::byte> bytes = SerializeIndex(got, meta);
                VolumeIndex loaded;
                CacheMeta back;
                const CacheLoadResult r2 = DeserializeIndex(bytes.data(), bytes.size(), loaded, back);
                if (r2.status != CacheLoadStatus::Ok || !SameIndex(got, loaded) || back.journalId != meta.journalId ||
                    back.appliedUsn != meta.appliedUsn || back.openFileIds != meta.openFileIds ||
                    SerializeIndex(loaded, back) != bytes) {
                    std::printf("  seed %u: cache round trip differs (%ls)\n", seed, r2.reason);
                    ok = false;
                }
                live = LiveIndex(std::move(loaded));  // keep updating the loaded index
                ++g_roundTrips;
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


// ---- cache ----------------------------------------------------------------------

void TestCrc32() {
    // Check values computed independently with Python's zlib.crc32 (zlib
    // implements the same CRC as PNG and gzip, RFC 1952).
    auto crc = [](const char* text) { return ComputeCrc32(text, std::strlen(text)); };
    CHECK(crc("") == 0x00000000u);
    CHECK(crc("a") == 0xE8B7BE43u);
    CHECK(crc("123456789") == 0xCBF43926u);
    CHECK(crc("The quick brown fox jumps over the lazy dog") == 0x414FA339u);
    unsigned char all[256];
    for (int i = 0; i < 256; ++i) all[i] = static_cast<unsigned char>(i);
    CHECK(ComputeCrc32(all, sizeof(all)) == 0x29058C73u);
    Crc32 parts;
    parts.Update(all, 100);
    parts.Update(all + 100, 156);
    CHECK(parts.Value() == 0x29058C73u);
}

// An index with nested folders, hard links and several root children.
VolumeIndex CacheSample() {
    Volume v = BaseVolume();
    LiveIndex live(v.Build());
    Mutator m(v, 7);
    for (int batch = 0; batch < 120; ++batch) {
        const VolumeIndex before = live.Compacted();
        std::vector<ChangedId> changed;
        m.BeginBatch();
        for (int k = 0; k < 3; ++k) m.Step(before, changed);
        if (live.Apply(changed, v.Query(changed), v.Rules()).status != UpdateStatus::Applied) break;
    }
    return v.Build();
}

CacheLoadStatus Load(const std::vector<std::byte>& bytes) {
    VolumeIndex index;
    CacheMeta meta;
    const CacheLoadResult r = DeserializeIndex(bytes.data(), bytes.size(), index, meta);
    if (r.status != CacheLoadStatus::Ok) CHECK(index.dirs.empty() && index.files.empty() && meta.openFileIds.empty());
    return r.status;
}

template <typename T>
void PutAt(std::vector<std::byte>& b, size_t at, T value) {
    std::memcpy(b.data() + at, &value, sizeof(T));
}

template <typename T>
T GetAt(const std::vector<std::byte>& b, size_t at) {
    T value;
    std::memcpy(&value, b.data() + at, sizeof(T));
    return value;
}

void TestCacheCorruption() {
    const VolumeIndex index = CacheSample();
    CacheMeta meta;
    meta.journalId = 42;
    meta.appliedUsn = 4096;
    meta.openFileIds = {3};
    const std::vector<std::byte> good = SerializeIndex(index, meta);
    CacheSections s;
    CHECK(LocateCacheSections(good, s));
    CHECK(s.dirCount >= 4 && s.fileCount >= 4 && s.linkCount >= 1);
    CHECK(Load(good) == CacheLoadStatus::Ok);
    std::printf("cache sample: %llu folders, %llu files, %llu extra links, %zu bytes\n", s.dirCount, s.fileCount,
                s.linkCount, good.size());

    // Every truncation and every single-byte change is rejected.
    bool allRejected = true;
    for (size_t n = 0; n < good.size(); ++n) {
        std::vector<std::byte> b(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        if (Load(b) == CacheLoadStatus::Ok) allRejected = false;
    }
    CHECK(allRejected);
    allRejected = true;
    for (size_t n = 0; n < good.size(); ++n) {
        std::vector<std::byte> b = good;
        b[n] ^= std::byte{0x01};
        if (Load(b) == CacheLoadStatus::Ok) allRejected = false;
    }
    CHECK(allRejected);
    {
        std::vector<std::byte> b = good;
        b.push_back(std::byte{0});
        CHECK(Load(b) != CacheLoadStatus::Ok);
    }

    // Changes with valid checksums: the structural checks alone must catch them.
    auto mutated = [&](auto change) {
        std::vector<std::byte> b = good;
        change(b);
        RecomputeCacheChecksums(b);
        return Load(b);
    };
    const size_t D = CacheSections::kDirBytes, F = CacheSections::kFileBytes;
    auto dirAt = [&](std::uint64_t k) { return s.dirs + k * D; };
    auto fileAt = [&](std::uint64_t k) { return s.files + k * F; };
    const auto root = GetAt<std::uint32_t>(good, CacheSections::kRootAt);
    // A non-root folder whose parent is not the root (for a cycle).
    std::uint64_t child = 0, parent = 0;
    for (std::uint64_t k = 0; k < s.dirCount; ++k) {
        const auto p = GetAt<std::uint32_t>(good, dirAt(k) + CacheSections::kDirParentAt);
        if (k != root && p != root && p != kNoDir) {
            child = k;
            parent = p;
            break;
        }
    }
    CHECK(child != 0 || parent != 0);
    const std::uint64_t other = root == 0 ? 1 : 0;  // some non-root folder

    CHECK(mutated([&](auto& b) { PutAt<std::uint32_t>(b, CacheSections::kRulesVersionAt, kCountingRulesVersion + 1); }) ==
          CacheLoadStatus::VersionMismatch);
    CHECK(mutated([&](auto& b) { PutAt<std::uint32_t>(b, CacheSections::kRootAt, static_cast<std::uint32_t>(s.dirCount)); }) ==
          CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) {  // root with a parent
              PutAt<std::uint32_t>(b, dirAt(root) + CacheSections::kDirParentAt, static_cast<std::uint32_t>(other));
          }) == CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) {  // folder that is its own parent
              PutAt<std::uint32_t>(b, dirAt(other) + CacheSections::kDirParentAt, static_cast<std::uint32_t>(other));
          }) == CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) {  // parent out of range
              PutAt<std::uint32_t>(b, dirAt(other) + CacheSections::kDirParentAt, static_cast<std::uint32_t>(s.dirCount));
          }) == CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) {  // non-root folder without a parent
              PutAt<std::uint32_t>(b, dirAt(other) + CacheSections::kDirParentAt, kNoDir);
          }) == CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) {  // cycle: parent -> child -> parent
              PutAt<std::uint32_t>(b, dirAt(parent) + CacheSections::kDirParentAt, static_cast<std::uint32_t>(child));
          }) == CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) {  // folder IDs out of order
              const auto a = GetAt<std::uint64_t>(b, dirAt(0));
              PutAt<std::uint64_t>(b, dirAt(0), GetAt<std::uint64_t>(b, dirAt(1)));
              PutAt<std::uint64_t>(b, dirAt(1), a);
          }) == CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) { PutAt<std::uint64_t>(b, dirAt(0), 0); }) == CacheLoadStatus::Invalid);  // ID 0
    CHECK(mutated([&](auto& b) {  // name outside the pool
              PutAt<std::uint32_t>(b, dirAt(other) + CacheSections::kDirNameOffsetAt, static_cast<std::uint32_t>(s.nameUnits));
              PutAt<std::uint16_t>(b, dirAt(other) + CacheSections::kDirNameLengthAt, 1);
          }) == CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) { PutAt<std::uint16_t>(b, dirAt(other) + CacheSections::kDirReservedAt, 1); }) ==
          CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) { PutAt<std::uint16_t>(b, fileAt(0) + CacheSections::kFileReservedAt, 1); }) ==
          CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) { PutAt<std::uint64_t>(b, fileAt(1), GetAt<std::uint64_t>(b, fileAt(0))); }) ==
          CacheLoadStatus::Invalid);  // duplicate file ID
    {  // a file with a folder's ID, keeping both lists strictly increasing
        bool placed = false;
        std::vector<std::byte> b = good;
        for (std::uint64_t k = 0; k < s.fileCount && !placed; ++k) {
            const std::uint64_t low = k == 0 ? 0 : GetAt<std::uint64_t>(b, fileAt(k - 1));
            const std::uint64_t high = k + 1 == s.fileCount ? UINT64_MAX : GetAt<std::uint64_t>(b, fileAt(k + 1));
            for (std::uint64_t d = 0; d < s.dirCount; ++d) {
                const auto id = GetAt<std::uint64_t>(b, dirAt(d));
                if (id > low && id < high) {
                    PutAt<std::uint64_t>(b, fileAt(k), id);
                    placed = true;
                    break;
                }
            }
        }
        CHECK(placed);
        RecomputeCacheChecksums(b);
        CHECK(Load(b) == CacheLoadStatus::Invalid);
    }
    CHECK(mutated([&](auto& b) {
              PutAt<std::uint32_t>(b, fileAt(0) + CacheSections::kFileDirAt, static_cast<std::uint32_t>(s.dirCount));
          }) == CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) { PutAt<std::uint16_t>(b, fileAt(0) + CacheSections::kFileLinksAt, 0); }) ==
          CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) {  // one more counted link than extra links
              const auto n = GetAt<std::uint16_t>(b, fileAt(0) + CacheSections::kFileLinksAt);
              PutAt<std::uint16_t>(b, fileAt(0) + CacheSections::kFileLinksAt, static_cast<std::uint16_t>(n + 1));
          }) == CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) {  // extra link to a file out of range
              PutAt<std::uint32_t>(b, s.links, static_cast<std::uint32_t>(s.fileCount));
          }) == CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) {  // extra link to a folder out of range
              PutAt<std::uint32_t>(b, s.links + 4, static_cast<std::uint32_t>(s.dirCount));
          }) == CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) {  // size totals overflow
              PutAt<std::uint64_t>(b, fileAt(0) + CacheSections::kFileLogicalAt, UINT64_MAX);
              PutAt<std::uint64_t>(b, fileAt(1) + CacheSections::kFileLogicalAt, UINT64_MAX);
          }) == CacheLoadStatus::Invalid);
    CHECK(mutated([&](auto& b) { PutAt<std::uint64_t>(b, CacheSections::kDirCountAt, s.dirCount + 1); }) ==
          CacheLoadStatus::SizeMismatch);
    CHECK(mutated([&](auto& b) { PutAt<std::uint64_t>(b, CacheSections::kDirCountAt, UINT64_MAX / 2); }) == CacheLoadStatus::SizeMismatch);
    CHECK(mutated([&](auto& b) { PutAt<std::uint32_t>(b, 0, 0); }) == CacheLoadStatus::BadMagic);
    CHECK(mutated([&](auto& b) { PutAt<std::uint32_t>(b, CacheSections::kFormatVersionAt, kCacheFormatVersion + 1); }) ==
          CacheLoadStatus::VersionMismatch);
    CHECK(mutated([&](auto& b) { PutAt<std::uint32_t>(b, CacheSections::kHeaderReservedAt, 1); }) == CacheLoadStatus::Invalid);
    // A count whose byte size wraps around to the right total (24 * 2^61 =
    // 3 * 2^64): only the per-count bound stops it.
    CHECK(mutated([&](auto& b) { PutAt<std::uint64_t>(b, CacheSections::kDirCountAt, s.dirCount + (1ull << 61)); }) ==
          CacheLoadStatus::SizeMismatch);
}

// The cache folder checks, on folders an unelevated test can make. A folder
// that passes needs an elevated process; the real run covers that.
void TestCacheFolderChecks() {
    wchar_t tempPath[MAX_PATH + 1] = {};
    CHECK(GetTempPathW(MAX_PATH + 1, tempPath) != 0);
    const std::wstring base = std::wstring(tempPath) + L"dirsize-cache-test-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring mine = base + L"\\mine";
    const std::wstring junction = base + L"\\junction";
    const std::wstring file = base + L"\\file.txt";
    CreateDirectoryW(base.c_str(), nullptr);
    CHECK(CreateDirectoryW(mine.c_str(), nullptr) != 0);
    {
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    const std::wstring command = L"cmd /c mklink /J \"" + junction + L"\" \"" + mine + L"\" >nul";
    CHECK(_wsystem(command.c_str()) == 0);

    StoreResult r = CheckFolderIsPrivate(base + L"\\does-not-exist");
    CHECK(!r.ok && r.missing);
    r = CheckFolderIsPrivate(file);
    CHECK(!r.ok && !r.missing && std::wcscmp(r.reason, L"cache folder path is not a folder") == 0);
    r = CheckFolderIsPrivate(junction);
    CHECK(!r.ok && !r.missing && std::wcscmp(r.reason, L"cache folder is a reparse point") == 0);
    // Created by this (unelevated) user: owned by the user, who is also allowed in the DACL.
    r = CheckFolderIsPrivate(mine);
    CHECK(!r.ok && !r.missing);
    // Owned by SYSTEM on a default Windows install, but its DACL allows Users:
    // only the DACL check rejects it.
    wchar_t programData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH) != 0) {
        r = CheckFolderIsPrivate(programData);
        CHECK(!r.ok && std::wcscmp(r.reason, L"cache folder DACL allows someone other than SYSTEM or Administrators") == 0);
    }

    RemoveDirectoryW(junction.c_str());  // removes the junction, not its target
    DeleteFileW(file.c_str());
    RemoveDirectoryW(mine.c_str());
    RemoveDirectoryW(base.c_str());
}

void TestVolumeGuidPath() {
    std::uint8_t guid[16];
    std::wstring text;
    CHECK(ParseVolumeGuidPath(L"\\\\?\\Volume{01234567-89AB-cdef-0123-456789abcdef}\\", guid, text));
    CHECK(text == L"01234567-89ab-cdef-0123-456789abcdef");
    const std::uint8_t expected[16] = {0x67, 0x45, 0x23, 0x01, 0xAB, 0x89, 0xEF, 0xCD,
                                       0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    CHECK(std::memcmp(guid, expected, 16) == 0);
    GUID g{};
    std::memcpy(&g, guid, 16);
    CHECK(g.Data1 == 0x01234567u && g.Data2 == 0x89AB && g.Data3 == 0xCDEF && g.Data4[0] == 0x01 && g.Data4[7] == 0xEF);
    CHECK(!ParseVolumeGuidPath(L"\\\\?\\Volume{01234567-89AB-cdef-0123-456789abcdef}", guid, text));  // no backslash
    CHECK(!ParseVolumeGuidPath(L"\\\\?\\Volume{01234567-89AB-cdef-0123-456789abcdeg}\\", guid, text));
    CHECK(!ParseVolumeGuidPath(L"\\\\?\\Volume{0123456789AB-cdef-0123-456789abcdef-}\\", guid, text));
    CHECK(!ParseVolumeGuidPath(L"C:\\", guid, text));
}

}  // namespace

int main() {
    TestUsnParser();
    TestChangeHelpers();
    TestCrc32();
    TestVolumeGuidPath();
    TestCacheFolderChecks();
    TestCacheCorruption();
    TestRandomEquivalence();
    TestFolderBecomesVisibleNeedsRescan();
    TestInconsistentBatchesNeedRescan();
    TestMoveOutThenHideParent();
    TestRecordReuse();
    TestRepeatedApplyIsIdempotent();
    CHECK(g_roundTrips == 60);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
