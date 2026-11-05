#pragma once

// One indexed volume inside the service (docs/DESIGN-phase4.md section 4).
//
// The worker thread owns the NtfsLiveVolume: it catches up with the journal
// every 5 s and answers queries, so the index needs no lock. A rescan (no
// usable cache, journal lost, index inconsistent) runs on its own thread in
// background mode and builds a separate index; until it is handed over,
// queries are answered from the previous index and marked "updating".
//
// Cache saves follow the decided policy: at stop, after about 100,000
// changed IDs, or after 6 hours with changes.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/cache_store.h"
#include "core/ntfs_live.h"
#include "service/protocol.h"

namespace dirsize::service {

// Log lines from the service (console or a log file); must be thread-safe.
using LogFunction = std::function<void(const std::wstring&)>;

// Minimum time between the start of consecutive rescans: after the n-th
// rescan in a row (n >= 1) the next may start RescanBackoff(n) later: 1, 2,
// 4, 8 minutes, then 10 minutes. Without it a long burst of changes (a large
// install, an archive being extracted) would keep the service rebuilding.
std::chrono::seconds RescanBackoff(int consecutive);

// Decision 8.3, with a bound: when more files than this differ between the
// previous index and a rescan, the previous index was out of date (the rescan
// followed a gap), so the difference is real changes, not files held open:
// nothing is tracked.
constexpr size_t kMaxDifferingTracked = 500;

struct FolderAnswer {
    bool indexed = false;  // false: no index yet (first build running)
    bool found = false;
    bool updating = false;
    Totals totals;
    std::uint32_t childCount = 0;
    struct Child {
        std::uint64_t id = 0;
        std::wstring name;
        Totals totals;
    };
    std::vector<Child> children;  // the requested page, largest on disk first
};

class VolumeWorker {
public:
    VolumeWorker(wchar_t drive, VolumeIdentity identity, LogFunction log);
    ~VolumeWorker();
    VolumeWorker(const VolumeWorker&) = delete;
    VolumeWorker& operator=(const VolumeWorker&) = delete;

    void Start();
    void RequestStop();  // asks the worker to save and stop; returns at once
    void Stop();         // RequestStop, then waits

    wchar_t Drive() const { return drive_; }
    const VolumeIdentity& Identity() const { return identity_; }
    // A handle on the root folder, opened by the service; valid until Stop.
    // Used as the hint for OpenFileById when checking child folders.
    HANDLE RootHandle() const { return root_; }

    proto::VolumeStatus Status() const;

    // Runs on the worker thread; waits up to `timeout` for the answer.
    // Returns false when the worker is stopping or busy past the timeout.
    bool QueryFolder(std::uint64_t folderId, std::uint32_t pageStart, std::uint32_t pageSize,
                     std::chrono::milliseconds timeout, FolderAnswer& answer);

private:
    void Run();
    void RequestRescan(const wchar_t* why);
    void StartRescan(const std::wstring& why);
    void FinishRescan();
    void MaybeSave(bool stopping);
    void Answer(std::uint64_t folderId, std::uint32_t pageStart, std::uint32_t pageSize, FolderAnswer& answer) const;

    const wchar_t drive_;
    const VolumeIdentity identity_;
    LogFunction log_;
    HANDLE root_ = INVALID_HANDLE_VALUE;

    // Owned by the worker thread.
    std::unique_ptr<NtfsLiveVolume> volume_;
    std::uint64_t changesSinceSave_ = 0;
    std::chrono::steady_clock::time_point lastSave_ = std::chrono::steady_clock::now();

    // Rescan hand-over.
    std::thread rescanThread_;
    std::mutex rescanMutex_;
    std::unique_ptr<NtfsLiveVolume> rescanned_;
    std::atomic<bool> rescanDone_{false};
    std::atomic<bool> rescanRunning_{false};
    bool rescanWanted_ = false;
    std::wstring rescanReason_;
    int consecutiveRescans_ = 0;
    std::chrono::steady_clock::time_point rescanAllowedAt_{};
    std::chrono::steady_clock::time_point readySince_{};

    // Requests to the worker thread.
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_ = false;
    std::thread thread_;

    // Status, read by other threads.
    std::atomic<int> state_{static_cast<int>(proto::VolumeState::NoIndex)};
    std::atomic<std::uint64_t> memoryBytes_{0};
    std::atomic<std::int64_t> lastCatchUpTicks_{0};  // steady_clock ticks; 0 = never
};

}  // namespace dirsize::service
