#pragma once

// Incremental updates of a folder index (Phase 3). See docs/DESIGN-phase3.md.
//
// The caller collects the file IDs that changed (from the USN journal) and
// re-queries their current state (FSCTL_QUERY_FILE_LAYOUT filtered by file
// ID). Apply() replaces the old contribution of every changed ID with its
// current one; it never derives size deltas from journal records, so the
// result does not depend on the order or repetition of changes.
//
// A batch is applied in three passes:
//   1. subtract the old contribution of every changed file;
//   2. change the folder structure (creations parent first, moves and
//      visibility changes, deletions deepest first), moving whole subtree
//      totals;
//   3. add the current contribution of every changed file.
// Anything inconsistent (a deleted folder that is not empty, a cycle, a total
// that would go below zero, a folder that becomes visible without being
// created) returns NeedsRescan; the index must then be rebuilt from a full
// scan. No repair is attempted.
//
// Not maintained incrementally: VolumeIndex::excluded, rootListingMatched,
// followedReparseTags and the diagnostic samples. They describe the last full
// scan, since hidden subtrees are not kept in the index.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/index.h"
#include "core/layout_parser.h"

namespace dirsize {

constexpr std::uint32_t kNoFile = 0xFFFFFFFFu;

// The current state of one file or folder, owning its names (layout entries
// point into the query buffer).
struct CurrentLink {
    std::uint64_t parentId = 0;
    std::wstring name;  // kept for folders only
};

struct CurrentEntry {
    std::uint64_t id = 0;
    std::uint32_t attributes = 0;
    std::vector<CurrentLink> links;
    std::uint64_t logicalBytes = 0;
    std::uint64_t allocatedBytes = 0;
    bool malformed = false;
};

CurrentEntry ToCurrentEntry(const LayoutEntry& entry);

// A changed file ID with the OR of all USN reason flags seen for it.
struct ChangedId {
    std::uint64_t id = 0;
    std::uint32_t reasons = 0;
};

// What the counting rules need from the volume while applying a batch.
struct UpdateRules {
    std::function<bool(std::uint64_t id)> listedInRoot;  // current root listing
    std::function<bool(std::uint64_t id, std::uint32_t& tag)> readReparseTag;
};

enum class UpdateStatus { Applied, NeedsRescan };

struct UpdateResult {
    UpdateStatus status = UpdateStatus::Applied;
    const wchar_t* reason = L"";  // why a rescan is needed
    std::uint64_t filesChanged = 0;
    std::uint64_t foldersCreated = 0;
    std::uint64_t foldersMoved = 0;
    std::uint64_t foldersRemoved = 0;  // deleted or no longer visible
};

class LiveIndex {
public:
    // `built` must come from a full build (LayoutIndexBuilder: sorted by ID).
    explicit LiveIndex(VolumeIndex built);

    // Applies one batch. `changed`: IDs from the journal. `current`: entries
    // returned for their MFT record numbers (any full ID). After NeedsRescan
    // the index is unusable and every later call returns NeedsRescan.
    UpdateResult Apply(const std::vector<ChangedId>& changed, const std::vector<CurrentEntry>& current,
                       const UpdateRules& rules);

    const VolumeIndex& Index() const { return index_; }
    bool Broken() const { return broken_; }

    DirIndex FindDir(std::uint64_t id) const;
    std::uint32_t FindFile(std::uint64_t id) const;
    bool DirLive(DirIndex d) const { return d != kNoDir && (index_.dirs[d].flags & kEntryDead) == 0; }

    // A fresh index with only live entries, sorted by ID, totals recomputed
    // from the files. Used for saving and as a consistency check.
    VolumeIndex Compacted() const;

private:
    struct DirChange;

    bool AddTotals(DirIndex d, const Totals& amount, bool subtract);
    void LinkChild(DirIndex parent, DirIndex child);
    void UnlinkChild(DirIndex child);
    void KillSubtree(DirIndex d);
    bool IsInSubtree(DirIndex candidate, DirIndex top) const;
    void SubtractFile(std::uint32_t f, std::vector<std::uint32_t>& multiLink);
    UpdateResult Fail(UpdateResult r, const wchar_t* reason);

    VolumeIndex index_;
    size_t sortedDirs_ = 0;
    size_t sortedFiles_ = 0;
    std::unordered_map<std::uint64_t, DirIndex> newDirs_;
    std::unordered_map<std::uint64_t, std::uint32_t> newFiles_;
    bool broken_ = false;
};

}  // namespace dirsize
