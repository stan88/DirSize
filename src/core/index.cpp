#include "core/index.h"

#include <algorithm>
#include <numeric>

namespace dirsize {

std::uint64_t VolumeIndex::MemoryBytes() const {
    return files.capacity() * sizeof(FileEntry) + extraLinks.capacity() * sizeof(ExtraLink) +
           dirs.capacity() * sizeof(DirEntry) + names.capacity() * sizeof(wchar_t);
}

void CountFollowedReparseTag(VolumeIndex& index, std::uint32_t tag, std::uint64_t folderId) {
    if (index.followedReparseDirIds.size() < kMaxSamples) {
        index.followedReparseDirIds.push_back(folderId);
    }
    for (ReparseTagCount& t : index.followedReparseTags) {
        if (t.tag == tag) {
            ++t.folders;
            return;
        }
    }
    index.followedReparseTags.push_back({tag, 1});
}

std::uint32_t AddName(VolumeIndex& index, std::wstring_view name, std::uint16_t& length) {
    const auto offset = static_cast<std::uint32_t>(index.names.size());
    const size_t n = std::min<size_t>(name.size(), 0xFFFF);
    index.names.insert(index.names.end(), name.begin(), name.begin() + static_cast<std::ptrdiff_t>(n));
    length = static_cast<std::uint16_t>(n);
    return offset;
}

std::vector<std::uint32_t> ComputeDepths(const std::vector<DirEntry>& dirs, DirIndex root) {
    constexpr std::uint32_t kUnknown = 0xFFFFFFFDu;
    constexpr std::uint32_t kVisiting = 0xFFFFFFFEu;
    static_assert(kVisiting != kUnreachableDepth && kUnknown != kUnreachableDepth);
    std::vector<std::uint32_t> depth(dirs.size(), kUnknown);
    if (root >= dirs.size()) {
        std::fill(depth.begin(), depth.end(), kUnreachableDepth);
        return depth;
    }
    depth[root] = 0;
    std::vector<DirIndex> chain;
    for (DirIndex start = 0; start < dirs.size(); ++start) {
        if (depth[start] != kUnknown) {
            continue;
        }
        // Walk up until a folder with a known depth, a missing parent, or a
        // cycle (a folder already on the chain, which still has kUnknown).
        chain.clear();
        DirIndex current = start;
        std::uint32_t base = kUnreachableDepth;
        for (;;) {
            if (current >= dirs.size()) {
                break;  // parent outside the table
            }
            if (depth[current] != kUnknown) {
                base = depth[current];
                break;
            }
            // Mark as visiting so a cycle ends the walk.
            depth[current] = kVisiting;
            chain.push_back(current);
            current = dirs[current].parent;
        }
        if (base == kVisiting) {
            base = kUnreachableDepth;  // hit our own chain: cycle
        }
        // Assign depths from the top of the chain down.
        for (size_t i = chain.size(); i-- > 0;) {
            if (base == kUnreachableDepth) {
                depth[chain[i]] = kUnreachableDepth;
            } else {
                base += 1;
                depth[chain[i]] = base;
            }
        }
    }
    return depth;
}

void RollUp(VolumeIndex& index) {
    std::vector<DirEntry>& dirs = index.dirs;
    const std::vector<std::uint32_t> depth = ComputeDepths(dirs, index.root);

    std::vector<DirIndex> order(dirs.size());
    std::iota(order.begin(), order.end(), DirIndex{0});
    // Deepest first; ties in index order so the result is deterministic.
    std::stable_sort(order.begin(), order.end(), [&](DirIndex a, DirIndex b) { return depth[a] > depth[b]; });

    for (DirEntry& d : dirs) {
        d.firstChild = kNoDir;
        d.nextSibling = kNoDir;
    }
    for (DirIndex i : order) {
        if (i == index.root || depth[i] == kUnreachableDepth) {
            continue;
        }
        DirEntry& d = dirs[i];
        DirEntry& parent = dirs[d.parent];
        parent.subtree.Add(d.subtree);
        parent.subtree.folders += 1;
    }
    // Child lists in index order (reverse insertion keeps ascending order).
    for (DirIndex i = static_cast<DirIndex>(dirs.size()); i-- > 0;) {
        if (i == index.root || depth[i] == kUnreachableDepth) {
            continue;
        }
        DirEntry& parent = dirs[dirs[i].parent];
        dirs[i].nextSibling = parent.firstChild;
        parent.firstChild = i;
    }
}

}  // namespace dirsize
