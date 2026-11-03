#include "core/layout_index_builder.h"

#include <windows.h>

#include <algorithm>
#include <numeric>

namespace dirsize {

void LayoutIndexBuilder::Add(const LayoutEntry& entry) {
    if (entry.malformed) {
        ++malformed_;
        return;
    }
    if (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) {
        RawDir d;
        d.id = entry.id;
        d.reparse = (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (!entry.links.empty()) {
            // A folder has exactly one name; hard links to folders do not exist.
            const LayoutLink& link = entry.links.front();
            d.hasParent = true;
            d.parentId = link.parentId;
            d.nameOffset = static_cast<std::uint32_t>(names_.size());
            const size_t n = std::min<size_t>(link.name.size(), 0xFFFF);
            names_.insert(names_.end(), link.name.begin(), link.name.begin() + static_cast<std::ptrdiff_t>(n));
            d.nameLength = static_cast<std::uint16_t>(n);
        }
        dirs_.push_back(d);
        return;
    }

    RawFile f;
    f.id = entry.id;
    f.logicalBytes = entry.dataLogicalBytes;
    f.allocatedBytes = entry.dataAllocatedBytes;
    f.firstLink = static_cast<std::uint32_t>(links_.size());
    f.linkCount = static_cast<std::uint16_t>(std::min<size_t>(entry.links.size(), 0xFFFF));
    for (size_t i = 0; i < f.linkCount; ++i) {
        links_.push_back(entry.links[i].parentId);
    }
    if (entry.namedStreams != 0) {
        f.hasNamedStreams = true;
        named_.push_back({static_cast<std::uint32_t>(files_.size()), entry.namedStreams, entry.namedLogicalBytes,
                          entry.namedAllocatedBytes});
    }
    files_.push_back(f);
}

VolumeIndex LayoutIndexBuilder::Finish(std::uint64_t rootId, std::vector<std::uint64_t> rootListingIds,
                                       const std::function<bool(std::uint64_t id, std::uint32_t& tag)>& readReparseTag) {
    VolumeIndex index;
    index.sortedById = true;
    index.issues.malformedEntries = malformed_;
    std::sort(rootListingIds.begin(), rootListingIds.end());
    auto listedInRoot = [&](std::uint64_t id) {
        return std::binary_search(rootListingIds.begin(), rootListingIds.end(), id);
    };

    // ---- folders: sort by ID, drop duplicate IDs -----------------------------
    std::stable_sort(dirs_.begin(), dirs_.end(), [](const RawDir& a, const RawDir& b) { return a.id < b.id; });
    {
        auto last = std::unique(dirs_.begin(), dirs_.end(),
                                [](const RawDir& a, const RawDir& b) { return a.id == b.id; });
        index.issues.duplicateIds += static_cast<std::uint64_t>(dirs_.end() - last);
        dirs_.erase(last, dirs_.end());
    }
    const auto n = static_cast<DirIndex>(dirs_.size());
    auto findDir = [&](std::uint64_t id) -> DirIndex {
        auto it = std::lower_bound(dirs_.begin(), dirs_.end(), id,
                                   [](const RawDir& d, std::uint64_t value) { return d.id < value; });
        return (it != dirs_.end() && it->id == id) ? static_cast<DirIndex>(it - dirs_.begin()) : kNoDir;
    };

    const DirIndex rawRoot = findDir(rootId);
    if (rawRoot == kNoDir) {
        return index;  // root not found: caller reports index.root == kNoDir
    }

    std::vector<DirEntry> all(n);
    for (DirIndex i = 0; i < n; ++i) {
        all[i].id = dirs_[i].id;
        if (i != rawRoot && dirs_[i].hasParent) {
            const DirIndex p = findDir(dirs_[i].parentId);
            all[i].parent = (p == i) ? kNoDir : p;
        }
    }
    const std::vector<std::uint32_t> depth = ComputeDepths(all, rawRoot);
    std::vector<DirIndex> byDepth;
    byDepth.reserve(n);
    for (DirIndex i = 0; i < n; ++i) {
        if (depth[i] == kUnreachableDepth) {
            ++index.excluded.unreachableFolders;
        } else if (i != rawRoot) {
            byDepth.push_back(i);
        }
    }
    std::stable_sort(byDepth.begin(), byDepth.end(), [&](DirIndex a, DirIndex b) { return depth[a] < depth[b]; });

    // ---- folder visibility, parents before children --------------------------
    std::vector<std::uint8_t> visible(n, 0);
    visible[rawRoot] = 1;
    for (DirIndex i : byDepth) {
        const DirIndex p = all[i].parent;
        if (!visible[p]) {
            ++index.excluded.hiddenFolders;
            continue;
        }
        if (p == rawRoot) {
            if (!listedInRoot(dirs_[i].id)) {
                ++index.excluded.hiddenFolders;  // e.g. $Extend
                continue;
            }
            ++index.rootListingMatched;
        }
        if (dirs_[i].reparse) {
            std::uint32_t tag = 0;
            if (!readReparseTag(dirs_[i].id, tag)) {
                ++index.issues.reparseTagFailures;
                continue;
            }
            if (IsReparseTagNameSurrogate(tag)) {
                ++index.excluded.surrogateReparseDirs;
                continue;
            }
            CountFollowedReparseTag(index, tag);
        }
        visible[i] = 1;
    }
    all.clear();
    all.shrink_to_fit();

    // ---- compact visible folders, keeping ID order ---------------------------
    std::vector<DirIndex> newIndex(n, kNoDir);
    DirIndex count = 0;
    for (DirIndex i = 0; i < n; ++i) {
        if (visible[i]) {
            newIndex[i] = count++;
        }
    }
    index.dirs.resize(count);
    for (DirIndex i = 0; i < n; ++i) {
        if (!visible[i]) {
            continue;
        }
        DirEntry& d = index.dirs[newIndex[i]];
        d.id = dirs_[i].id;
        if (i == rawRoot) {
            d.parent = kNoDir;
            d.nameOffset = static_cast<std::uint32_t>(index.names.size());
            d.nameLength = 0;  // the caller knows the root path
        } else {
            d.parent = newIndex[findDir(dirs_[i].parentId)];
            const std::wstring_view name(names_.data() + dirs_[i].nameOffset, dirs_[i].nameLength);
            d.nameOffset = AddName(index, name, d.nameLength);
        }
    }
    index.root = newIndex[rawRoot];
    names_.clear();
    names_.shrink_to_fit();

    // ---- files: sorted by ID, counted once per visible link ------------------
    std::vector<std::uint32_t> order(files_.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(),
                     [&](std::uint32_t a, std::uint32_t b) { return files_[a].id < files_[b].id; });
    index.files.reserve(files_.size());
    std::uint64_t previousId = 0;
    bool havePrevious = false;
    std::vector<DirIndex> counted;
    for (std::uint32_t raw : order) {
        const RawFile& f = files_[raw];
        if (havePrevious && f.id == previousId) {
            ++index.issues.duplicateIds;
            continue;
        }
        havePrevious = true;
        previousId = f.id;

        counted.clear();
        bool anyKnownParent = false;
        bool unlistedInRoot = false;
        for (std::uint32_t k = 0; k < f.linkCount; ++k) {
            const DirIndex d = findDir(links_[f.firstLink + k]);
            if (d == kNoDir || depth[d] == kUnreachableDepth) {
                continue;
            }
            anyKnownParent = true;
            if (!visible[d]) {
                continue;
            }
            if (d == rawRoot && !listedInRoot(f.id)) {
                unlistedInRoot = true;  // metafile in the root, e.g. $MFT
                continue;
            }
            counted.push_back(newIndex[d]);
        }
        if (counted.empty()) {
            if (anyKnownParent) {
                Totals& t = unlistedInRoot ? index.excluded.hiddenRootFiles : index.excluded.hidden;
                t.logicalBytes += f.logicalBytes;
                t.allocatedBytes += f.allocatedBytes;
                ++t.files;
            } else {
                ++index.excluded.unreachableFiles;
            }
            continue;
        }

        if (std::find(counted.begin(), counted.end(), index.root) != counted.end()) {
            ++index.rootListingMatched;
        }
        const auto fileIndex = static_cast<std::uint32_t>(index.files.size());
        FileEntry e;
        e.id = f.id;
        e.logicalBytes = f.logicalBytes;
        e.allocatedBytes = f.allocatedBytes;
        e.dir = counted.front();
        e.countedLinks = static_cast<std::uint16_t>(counted.size());
        index.files.push_back(e);
        for (size_t k = 0; k < counted.size(); ++k) {
            if (k > 0) {
                index.extraLinks.push_back({fileIndex, counted[k]});
            }
            Totals& t = index.dirs[counted[k]].subtree;
            t.logicalBytes += f.logicalBytes;
            t.allocatedBytes += f.allocatedBytes;
            ++t.files;
        }
        if (f.hasNamedStreams) {
            auto it = std::lower_bound(named_.begin(), named_.end(), raw,
                                       [](const RawNamed& r, std::uint32_t value) { return r.file < value; });
            if (it != named_.end() && it->file == raw) {
                index.excluded.namedStreams += it->count;
                index.excluded.namedStreamLogicalBytes += it->logicalBytes;
                index.excluded.namedStreamAllocatedBytes += it->allocatedBytes;
            }
        }
    }
    index.files.shrink_to_fit();
    index.extraLinks.shrink_to_fit();
    index.names.shrink_to_fit();

    dirs_.clear();
    dirs_.shrink_to_fit();
    files_.clear();
    files_.shrink_to_fit();
    links_.clear();
    links_.shrink_to_fit();
    named_.clear();
    named_.shrink_to_fit();

    RollUp(index);
    return index;
}

}  // namespace dirsize
