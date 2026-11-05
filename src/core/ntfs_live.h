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
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

#include "core/cache_store.h"
#include "core/index_cache.h"
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
    // Changed IDs the re-query did not return although the journal had no
    // FILE_DELETE for them (they are treated as deleted). Diagnostics.
    std::uint64_t vanished = 0;
    std::vector<ChangedId> vanishedSamples;  // at most 16
    double readSeconds = 0;
    double querySeconds = 0;
    double applySeconds = 0;
    UpdateResult update;
};

struct CacheOpenResult {
    bool ok = false;
    bool missing = false;  // no cache folder or file: not an error, just build
    const wchar_t* reason = L"";
    unsigned long error = 0;
    CacheLoadStatus status = CacheLoadStatus::Ok;  // when the file was read
    std::wstring path;
    std::uint64_t bytes = 0;
    double readSeconds = 0;
    double parseSeconds = 0;  // validation and rebuilding the totals
};

struct CacheSaveResult {
    bool ok = false;
    const wchar_t* reason = L"";
    unsigned long error = 0;
    std::wstring path;
    std::uint64_t bytes = 0;
    double compactSeconds = 0;
    double writeSeconds = 0;
};

// Diagnostics: what a re-query by file ID returns for one ID right now, and
// whether the root listing shows it.
struct IdProbe {
    unsigned long error = 0;      // of FSCTL_QUERY_FILE_LAYOUT
    std::uint64_t entries = 0;    // entries returned for the record number
    bool found = false;           // an entry with the full ID (same sequence number)
    std::uint64_t returnedId = 0; // ID of the first entry, if any
    std::uint32_t attributes = 0;
    bool hasData = false;
    std::uint64_t logicalBytes = 0;
    std::vector<std::pair<std::uint64_t, std::wstring>> links;  // parent ID, name
    unsigned long listError = 0;  // of listing the root
    bool listedInRoot = false;    // the root listing shows this ID
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

    // Loads %ProgramData%\DirSize\cache\<volume GUID>.cache. Returns null
    // when there is no usable cache (see `result`); the caller then builds.
    // The caller must call CatchUp() next: it checks the journal ID and that
    // no records were trimmed since the save, and applies the changes made
    // while DirSize was not running.
    static std::unique_ptr<NtfsLiveVolume> FromCache(wchar_t drive, CacheOpenResult& result);

    // Saves the index with the USN it is applied to and the files tracked as
    // open. Refused while the index is inconsistent.
    CacheSaveResult SaveCache() const;

    CatchUpResult CatchUp();

    IdProbe ProbeId(std::uint64_t id) const;

    const LiveIndex& Live() const { return live_; }
    std::uint64_t JournalId() const { return journalId_; }
    std::int64_t AppliedUsn() const { return appliedUsn_; }
    // Diagnostics: whether a file ID appeared in any journal record read by
    // this process (or was restored from the cache as open), and whether it
    // is currently tracked as open (no CLOSE seen yet).
    bool SeenInJournal(std::uint64_t id) const { return seen_.count(id) != 0; }
    // OR of the USN reasons read for `id` by this process (0 if none).
    std::uint32_t JournalReasons(std::uint64_t id) const {
        auto it = seen_.find(id);
        return it == seen_.end() ? 0 : it->second;
    }
    bool TrackedOpen(std::uint64_t id) const { return open_.Ids().count(id) != 0; }

private:
    NtfsLiveVolume(wchar_t drive, VolumeIndex index);
    DWORD OpenHandles();

    wchar_t drive_;
    HANDLE volume_ = INVALID_HANDLE_VALUE;
    HANDLE root_ = INVALID_HANDLE_VALUE;
    LiveIndex live_;
    std::uint64_t journalId_ = 0;
    std::int64_t appliedUsn_ = 0;
    OpenFileTracker open_;
    std::unordered_map<std::uint64_t, std::uint32_t> seen_;  // diagnostics: ID -> OR of reasons
};

}  // namespace dirsize
