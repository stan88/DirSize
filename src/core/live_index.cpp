#include "core/live_index.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <unordered_set>

namespace dirsize {
namespace {

Totals FileTotals(std::uint64_t logical, std::uint64_t allocated) {
    Totals t;
    t.logicalBytes = logical;
    t.allocatedBytes = allocated;
    t.files = 1;
    return t;
}

Totals FolderTotals(const Totals& subtree) {
    Totals t = subtree;
    t.folders += 1;  // the folder itself
    return t;
}

bool IsDirectory(const CurrentEntry* e) {
    return e != nullptr && (e->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

}  // namespace

CurrentEntry ToCurrentEntry(const LayoutEntry& entry) {
    CurrentEntry e;
    e.id = entry.id;
    e.attributes = entry.attributes;
    e.logicalBytes = entry.dataLogicalBytes;
    e.allocatedBytes = entry.dataAllocatedBytes;
    e.malformed = entry.malformed;
    const bool isDir = (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    for (const LayoutLink& l : entry.links) {
        CurrentLink link;
        link.parentId = l.parentId;
        if (isDir) link.name.assign(l.name.begin(), l.name.end());
        e.links.push_back(std::move(link));
    }
    return e;
}

struct LiveIndex::DirChange {
    std::uint64_t id = 0;
    DirIndex slot = kNoDir;
    const CurrentEntry* now = nullptr;
    std::uint32_t reasons = 0;
};

LiveIndex::LiveIndex(VolumeIndex built) : index_(std::move(built)) {
    sortedDirs_ = index_.dirs.size();
    sortedFiles_ = index_.files.size();
}

DirIndex LiveIndex::FindDir(std::uint64_t id) const {
    auto it = std::lower_bound(index_.dirs.begin(), index_.dirs.begin() + static_cast<std::ptrdiff_t>(sortedDirs_), id,
                               [](const DirEntry& d, std::uint64_t v) { return d.id < v; });
    if (it != index_.dirs.begin() + static_cast<std::ptrdiff_t>(sortedDirs_) && it->id == id) {
        return static_cast<DirIndex>(it - index_.dirs.begin());
    }
    auto n = newDirs_.find(id);
    return n == newDirs_.end() ? kNoDir : n->second;
}

std::uint32_t LiveIndex::FindFile(std::uint64_t id) const {
    auto end = index_.files.begin() + static_cast<std::ptrdiff_t>(sortedFiles_);
    auto it = std::lower_bound(index_.files.begin(), end, id,
                               [](const FileEntry& f, std::uint64_t v) { return f.id < v; });
    if (it != end && it->id == id) {
        return static_cast<std::uint32_t>(it - index_.files.begin());
    }
    auto n = newFiles_.find(id);
    return n == newFiles_.end() ? kNoFile : n->second;
}

// Adds or subtracts `amount` to folder `d` and all its ancestors. A dead
// folder contributes nowhere, so nothing happens. Returns false when a total
// would go below zero (the index is then inconsistent).
bool LiveIndex::AddTotals(DirIndex d, const Totals& amount, bool subtract) {
    if (!DirLive(d)) return true;
    for (DirIndex x = d; x != kNoDir; x = index_.dirs[x].parent) {
        Totals& t = index_.dirs[x].subtree;
        if (subtract) {
            if (t.logicalBytes < amount.logicalBytes || t.allocatedBytes < amount.allocatedBytes ||
                t.files < amount.files || t.folders < amount.folders) {
                return false;
            }
            t.logicalBytes -= amount.logicalBytes;
            t.allocatedBytes -= amount.allocatedBytes;
            t.files -= amount.files;
            t.folders -= amount.folders;
        } else {
            t.Add(amount);
        }
    }
    return true;
}

void LiveIndex::LinkChild(DirIndex parent, DirIndex child) {
    DirEntry& c = index_.dirs[child];
    c.parent = parent;
    c.prevSibling = kNoDir;
    c.nextSibling = index_.dirs[parent].firstChild;
    if (c.nextSibling != kNoDir) index_.dirs[c.nextSibling].prevSibling = child;
    index_.dirs[parent].firstChild = child;
}

void LiveIndex::UnlinkChild(DirIndex child) {
    DirEntry& c = index_.dirs[child];
    if (c.prevSibling != kNoDir) {
        index_.dirs[c.prevSibling].nextSibling = c.nextSibling;
    } else if (c.parent != kNoDir) {
        index_.dirs[c.parent].firstChild = c.nextSibling;
    }
    if (c.nextSibling != kNoDir) index_.dirs[c.nextSibling].prevSibling = c.prevSibling;
    c.prevSibling = kNoDir;
    c.nextSibling = kNoDir;
}

void LiveIndex::KillSubtree(DirIndex top) {
    std::vector<DirIndex> stack{top};
    while (!stack.empty()) {
        const DirIndex d = stack.back();
        stack.pop_back();
        index_.dirs[d].flags |= kEntryDead;
        for (DirIndex c = index_.dirs[d].firstChild; c != kNoDir; c = index_.dirs[c].nextSibling) stack.push_back(c);
    }
}

// True when `candidate` is `top` or below it.
bool LiveIndex::IsInSubtree(DirIndex candidate, DirIndex top) const {
    for (DirIndex x = candidate; x != kNoDir; x = index_.dirs[x].parent) {
        if (x == top) return true;
    }
    return false;
}

void LiveIndex::SubtractFile(std::uint32_t fi, std::vector<std::uint32_t>& multiLink) {
    FileEntry& f = index_.files[fi];
    const Totals t = FileTotals(f.logicalBytes, f.allocatedBytes);
    if (!AddTotals(f.dir, t, true)) broken_ = true;
    if (f.countedLinks > 1) multiLink.push_back(fi);
}

UpdateResult LiveIndex::Fail(UpdateResult r, const wchar_t* reason) {
    broken_ = true;
    r.status = UpdateStatus::NeedsRescan;
    r.reason = reason;
    return r;
}

UpdateResult LiveIndex::Apply(const std::vector<ChangedId>& changed, const std::vector<CurrentEntry>& current,
                              const UpdateRules& rules) {
    UpdateResult result;
    if (broken_) return Fail(result, L"index already inconsistent");

    std::unordered_map<std::uint64_t, const CurrentEntry*> now;
    for (const CurrentEntry& e : current) {
        if (e.malformed) return Fail(result, L"malformed layout entry");
        now[e.id] = &e;
    }
    std::unordered_map<std::uint64_t, std::uint32_t> reasons;
    std::vector<std::uint64_t> ids;
    for (const ChangedId& c : changed) {
        reasons[c.id] |= c.reasons;
        ids.push_back(c.id);
    }
    for (const CurrentEntry& e : current) ids.push_back(e.id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    const DirIndex root = index_.root;
    const std::uint64_t rootId = index_.dirs[root].id;

    // ---- classify ------------------------------------------------------------
    struct FileChange {
        std::uint64_t id;
        std::uint32_t slot;
        const CurrentEntry* now;
    };
    std::vector<FileChange> fileChanges;
    std::vector<DirChange> dirChanges;
    for (std::uint64_t id : ids) {
        if (id == rootId) continue;  // the root never moves
        auto it = now.find(id);
        const CurrentEntry* e = it == now.end() ? nullptr : it->second;
        const std::uint32_t f = FindFile(id);
        const DirIndex d = FindDir(id);
        if ((f != kNoFile && IsDirectory(e)) || (d != kNoDir && e != nullptr && !IsDirectory(e))) {
            return Fail(result, L"file and folder kinds disagree");
        }
        auto r = reasons.find(id);
        const std::uint32_t why = r == reasons.end() ? 0 : r->second;
        if (d != kNoDir || IsDirectory(e)) {
            dirChanges.push_back({id, d, e, why});
        } else {
            fileChanges.push_back({id, f, e});
        }
    }

    // ---- pass 1: subtract the old contribution of changed files ---------------
    std::vector<std::uint32_t> multiLink;
    for (const FileChange& fc : fileChanges) {
        if (fc.slot == kNoFile || (index_.files[fc.slot].flags & kEntryDead)) continue;
        SubtractFile(fc.slot, multiLink);
        index_.files[fc.slot].flags |= kEntryDead;
    }
    if (!multiLink.empty()) {
        std::unordered_set<std::uint32_t> drop(multiLink.begin(), multiLink.end());
        const auto file = [](const ExtraLink& x) { return x.file; };
        for (const ExtraLink& x : index_.extraLinks) {
            if (drop.count(file(x))) {
                const FileEntry& f = index_.files[x.file];
                if (!AddTotals(x.dir, FileTotals(f.logicalBytes, f.allocatedBytes), true)) broken_ = true;
            }
        }
        index_.extraLinks.erase(std::remove_if(index_.extraLinks.begin(), index_.extraLinks.end(),
                                               [&](const ExtraLink& x) { return drop.count(x.file) != 0; }),
                                index_.extraLinks.end());
    }
    if (broken_) return Fail(result, L"file totals below zero");
    result.filesChanged = fileChanges.size();

    // Whether a folder with current state `e` is visible under the live folder `p`.
    auto visibleUnder = [&](const CurrentEntry& e, DirIndex p) {
        if (!DirLive(p)) return false;
        const bool parentIsRoot = p == root;
        const bool listed = parentIsRoot && rules.listedInRoot(e.id);
        std::uint32_t tag = 0;
        return ClassifyChildDir(parentIsRoot, listed, (e.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
                                [&](std::uint32_t& t) { return rules.readReparseTag(e.id, t); },
                                tag) == ChildDirVisibility::Visible;
    };

    std::vector<DirChange> creations, updates, deletions;
    for (const DirChange& c : dirChanges) {
        const bool live = DirLive(c.slot);
        if (live && c.now == nullptr) {
            deletions.push_back(c);
        } else if (live) {
            updates.push_back(c);
        } else if (c.now != nullptr) {
            creations.push_back(c);  // new, or known but dead
        }
    }

    // ---- pass 2a: creations, parents before children ----------------------------
    {
        std::unordered_set<std::uint64_t> pendingIds;
        for (const DirChange& c : creations) pendingIds.insert(c.id);
        std::vector<DirChange> pending = creations;
        bool progress = true;
        while (!pending.empty() && progress) {
            progress = false;
            std::vector<DirChange> next;
            for (const DirChange& c : pending) {
                if (c.now->links.empty()) {
                    progress = true;  // no name: not visible
                    continue;
                }
                const CurrentLink& link = c.now->links.front();
                const DirIndex p = FindDir(link.parentId);
                if (p == kNoDir && pendingIds.count(link.parentId)) {
                    next.push_back(c);  // parent created later in this pass
                    continue;
                }
                progress = true;
                pendingIds.erase(c.id);
                if (p == kNoDir || !visibleUnder(*c.now, p)) continue;
                if (c.slot != kNoDir || (c.reasons & USN_REASON_FILE_CREATE) == 0) {
                    // Its contents were never indexed (it was hidden before).
                    return Fail(result, L"folder became visible");
                }
                DirEntry d;
                d.id = c.id;
                d.nameOffset = AddName(index_, link.name, d.nameLength);
                const auto slot = static_cast<DirIndex>(index_.dirs.size());
                index_.dirs.push_back(d);
                newDirs_[c.id] = slot;
                LinkChild(p, slot);
                if (!AddTotals(p, FolderTotals({}), false)) return Fail(result, L"folder totals");
                ++result.foldersCreated;
            }
            pending.swap(next);
        }
        // Whatever is left has a parent that never appeared: not visible.
    }

    // ---- pass 2b: moves, renames, visibility changes ------------------------------
    {
        std::vector<DirChange> pending = updates;
        std::vector<DirIndex> hide;
        bool progress = true;
        while (!pending.empty() && progress) {
            progress = false;
            std::vector<DirChange> next;
            for (const DirChange& c : pending) {
                const DirIndex d = c.slot;
                if (!DirLive(d)) {
                    // Killed with a subtree earlier in this batch.
                    progress = true;
                    const DirIndex p = c.now->links.empty() ? kNoDir : FindDir(c.now->links.front().parentId);
                    if (p != kNoDir && visibleUnder(*c.now, p)) return Fail(result, L"folder became visible");
                    continue;
                }
                const DirIndex oldParent = index_.dirs[d].parent;
                const DirIndex p = c.now->links.empty() ? kNoDir : FindDir(c.now->links.front().parentId);
                if (p == kNoDir || !visibleUnder(*c.now, p)) {
                    // No longer visible. Removed after all moves, so folders
                    // moved out of it in this batch are not removed with it.
                    progress = true;
                    hide.push_back(d);
                    continue;
                }
                const std::wstring& name = c.now->links.front().name;
                if (p != oldParent) {
                    if (IsInSubtree(p, d)) {
                        next.push_back(c);  // its new parent still sits below it
                        continue;
                    }
                    const Totals moved = FolderTotals(index_.dirs[d].subtree);
                    if (!AddTotals(oldParent, moved, true)) return Fail(result, L"folder totals below zero");
                    UnlinkChild(d);
                    LinkChild(p, d);
                    AddTotals(p, moved, false);
                    ++result.foldersMoved;
                }
                progress = true;
                const DirEntry& cur = index_.dirs[d];
                if (std::wstring_view(index_.names.data() + cur.nameOffset, cur.nameLength) != name) {
                    std::uint16_t length = 0;
                    const std::uint32_t offset = AddName(index_, name, length);
                    index_.dirs[d].nameOffset = offset;
                    index_.dirs[d].nameLength = length;
                }
            }
            pending.swap(next);
        }
        if (!pending.empty()) return Fail(result, L"folder moves form a cycle");
        // The whole subtree of a folder that is no longer visible leaves the totals.
        for (DirIndex d : hide) {
            if (!DirLive(d)) continue;  // already removed with an ancestor
            if (!AddTotals(index_.dirs[d].parent, FolderTotals(index_.dirs[d].subtree), true)) {
                return Fail(result, L"folder totals below zero");
            }
            UnlinkChild(d);
            KillSubtree(d);
            ++result.foldersRemoved;
        }
    }

    // ---- pass 2c: deletions, deepest first -----------------------------------------
    {
        auto depthOf = [&](DirIndex d) {
            std::uint32_t n = 0;
            for (DirIndex x = d; x != kNoDir; x = index_.dirs[x].parent) ++n;
            return n;
        };
        std::vector<std::pair<std::uint32_t, DirIndex>> order;
        for (const DirChange& c : deletions) order.push_back({depthOf(c.slot), c.slot});
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [depth, d] : order) {
            if (!DirLive(d)) continue;  // removed with a subtree already
            const Totals& t = index_.dirs[d].subtree;
            if (t.files != 0 || t.folders != 0 || t.logicalBytes != 0 || t.allocatedBytes != 0) {
                return Fail(result, L"deleted folder is not empty");
            }
            if (!AddTotals(index_.dirs[d].parent, FolderTotals({}), true)) {
                return Fail(result, L"folder totals below zero");
            }
            UnlinkChild(d);
            index_.dirs[d].flags |= kEntryDead;
            ++result.foldersRemoved;
        }
    }

    // ---- pass 3: add the current contribution of changed files ----------------------
    std::vector<DirIndex> counted;
    for (const FileChange& fc : fileChanges) {
        if (fc.now == nullptr) continue;  // deleted
        counted.clear();
        for (const CurrentLink& link : fc.now->links) {
            const DirIndex p = FindDir(link.parentId);
            if (!DirLive(p)) continue;
            const bool parentIsRoot = p == root;
            if (!FileLinkCounted(parentIsRoot, parentIsRoot && rules.listedInRoot(fc.id))) continue;
            counted.push_back(p);
        }
        if (counted.empty()) continue;  // not visible anywhere
        std::uint32_t slot = fc.slot;
        if (slot == kNoFile) {
            slot = static_cast<std::uint32_t>(index_.files.size());
            index_.files.push_back({});
            newFiles_[fc.id] = slot;
        }
        FileEntry& f = index_.files[slot];
        f.id = fc.id;
        f.logicalBytes = fc.now->logicalBytes;
        f.allocatedBytes = fc.now->allocatedBytes;
        f.dir = counted.front();
        f.countedLinks = static_cast<std::uint16_t>(std::min<size_t>(counted.size(), 0xFFFF));
        f.flags = 0;
        const Totals t = FileTotals(f.logicalBytes, f.allocatedBytes);
        for (size_t k = 0; k < counted.size(); ++k) {
            if (k > 0) index_.extraLinks.push_back({slot, counted[k]});
            AddTotals(counted[k], t, false);
        }
    }
    return result;
}

VolumeIndex LiveIndex::Compacted() const {
    VolumeIndex out;
    out.sortedById = true;
    out.excluded = index_.excluded;
    out.usn = index_.usn;
    out.issues = index_.issues;
    out.followedReparseTags = index_.followedReparseTags;
    out.rootListingMatched = index_.rootListingMatched;
    out.followedReparseDirIds = index_.followedReparseDirIds;
    out.unreachableFileSamples = index_.unreachableFileSamples;

    // Folders: live ones, sorted by ID.
    std::vector<DirIndex> liveDirs;
    for (DirIndex i = 0; i < index_.dirs.size(); ++i) {
        if (DirLive(i)) liveDirs.push_back(i);
    }
    std::sort(liveDirs.begin(), liveDirs.end(),
              [&](DirIndex a, DirIndex b) { return index_.dirs[a].id < index_.dirs[b].id; });
    std::vector<DirIndex> remap(index_.dirs.size(), kNoDir);
    for (DirIndex k = 0; k < liveDirs.size(); ++k) remap[liveDirs[k]] = k;
    out.dirs.resize(liveDirs.size());
    for (DirIndex k = 0; k < liveDirs.size(); ++k) {
        const DirEntry& src = index_.dirs[liveDirs[k]];
        DirEntry& d = out.dirs[k];
        d.id = src.id;
        d.parent = src.parent == kNoDir ? kNoDir : remap[src.parent];
        d.nameOffset = AddName(out, std::wstring_view(index_.names.data() + src.nameOffset, src.nameLength),
                               d.nameLength);
    }
    out.root = remap[index_.root];

    // Files: counted links in live folders.
    std::vector<std::vector<DirIndex>> extra(index_.files.size());
    for (const ExtraLink& x : index_.extraLinks) extra[x.file].push_back(x.dir);
    std::vector<std::uint32_t> liveFiles;
    for (std::uint32_t i = 0; i < index_.files.size(); ++i) {
        if ((index_.files[i].flags & kEntryDead) == 0) liveFiles.push_back(i);
    }
    std::sort(liveFiles.begin(), liveFiles.end(),
              [&](std::uint32_t a, std::uint32_t b) { return index_.files[a].id < index_.files[b].id; });
    std::vector<DirIndex> dirsOf;
    for (std::uint32_t i : liveFiles) {
        const FileEntry& src = index_.files[i];
        dirsOf.clear();
        if (DirLive(src.dir)) dirsOf.push_back(remap[src.dir]);
        for (DirIndex x : extra[i]) {
            if (DirLive(x)) dirsOf.push_back(remap[x]);
        }
        if (dirsOf.empty()) continue;
        const auto slot = static_cast<std::uint32_t>(out.files.size());
        FileEntry f = src;
        f.dir = dirsOf.front();
        f.countedLinks = static_cast<std::uint16_t>(dirsOf.size());
        f.flags = 0;
        out.files.push_back(f);
        for (size_t k = 0; k < dirsOf.size(); ++k) {
            if (k > 0) out.extraLinks.push_back({slot, dirsOf[k]});
            Totals& t = out.dirs[dirsOf[k]].subtree;
            t.logicalBytes += f.logicalBytes;
            t.allocatedBytes += f.allocatedBytes;
            ++t.files;
        }
    }
    RollUp(out);
    return out;
}

}  // namespace dirsize
