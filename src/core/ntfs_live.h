#pragma once

// Keeps the index of an NTFS volume current with the USN change journal
// (Phase 3). Needs an elevated process: the volume is opened with
// GENERIC_READ for both FSCTL_QUERY_FILE_LAYOUT and FSCTL_READ_USN_JOURNAL.
//
// CatchUp() reads the journal from the last applied USN to its end, collects
// the changed file IDs, re-queries their current state with
// FSCTL_QUERY_FILE_LAYOUT filtered by record number (batches of at most
// kMaxRangesPerQuery ranges) and applies it with LiveIndex::Apply. When it
// cannot continue (journal ID changed, records trimmed, too many changes, an
// inconsistent batch) it returns needsRescan and the caller rebuilds.

#include <windows.h>

#include <cstdint>
#include <memory>
#include <unordered_set>

#include "core/live_index.h"
#include "core/ntfs_index.h"
#include "core/usn_parser.h"

namespace dirsize {

constexpr size_t kMaxRangesPerQuery = 10000;
// Above this many changed IDs a full scan is faster (Phase 3 probe: 100,000
// IDs took over 20 s, a full scan of the system volume about 9 s).
constexpr size_t kMaxChangedIdsPerCatchUp = 50000;

struct CatchUpResult {
    bool needsRescan = false;
    const wchar_t* reason = L"";
    unsigned long error = 0;  // Win32 error behind the reason, if any
    std::int64_t fromUsn = 0;
    std::int64_t toUsn = 0;
    std::uint64_t records = 0;
    std::uint64_t changedIds = 0;   // from the journal records read now
    std::uint64_t openTracked = 0;  // still-open files re-queried as well
    std::uint64_t ranges = 0;
    std::uint64_t entries = 0;  // layout entries returned by the re-query
    std::uint64_t layoutCalls = 0;
    double readSeconds = 0;
    double querySeconds = 0;
    double applySeconds = 0;
    UpdateResult update;
};

class NtfsLiveVolume {
public:
    ~NtfsLiveVolume();
    NtfsLiveVolume(const NtfsLiveVolume&) = delete;
    NtfsLiveVolume& operator=(const NtfsLiveVolume&) = delete;

    // Full build with BuildNtfsIndex. Returns null when it fails (see
    // `build`). The caller should call CatchUp() right away: it replays the
    // changes made while the volume was being scanned.
    static std::unique_ptr<NtfsLiveVolume> Build(wchar_t drive, NtfsIndexResult& build);

    CatchUpResult CatchUp();

    const LiveIndex& Live() const { return live_; }
    std::uint64_t JournalId() const { return journalId_; }
    std::int64_t AppliedUsn() const { return appliedUsn_; }
    // Diagnostics: whether a file ID appeared in any journal record read so
    // far, and whether it is currently tracked as open (no CLOSE seen yet).
    bool SeenInJournal(std::uint64_t id) const { return seen_.count(id) != 0; }
    bool TrackedOpen(std::uint64_t id) const { return open_.Ids().count(id) != 0; }

private:
    NtfsLiveVolume(wchar_t drive, VolumeIndex index);

    wchar_t drive_;
    HANDLE volume_ = INVALID_HANDLE_VALUE;
    HANDLE root_ = INVALID_HANDLE_VALUE;
    LiveIndex live_;
    std::uint64_t journalId_ = 0;
    std::int64_t appliedUsn_ = 0;
    OpenFileTracker open_;
    std::unordered_set<std::uint64_t> seen_;
};

}  // namespace dirsize
