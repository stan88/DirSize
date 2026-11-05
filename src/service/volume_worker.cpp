#include "service/volume_worker.h"

#include <algorithm>

#include "core/dir_listing.h"

namespace dirsize::service {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kCatchUpInterval = std::chrono::seconds(5);
constexpr std::uint64_t kSaveAfterChanges = 100000;
constexpr auto kSaveAfterTime = std::chrono::hours(6);
constexpr auto kStableBeforeBackoffReset = std::chrono::minutes(30);

std::wstring Hex64(std::uint64_t v) {
    wchar_t text[32];
    swprintf_s(text, L"0x%016llx", v);
    return text;
}

}  // namespace

std::chrono::seconds RescanBackoff(int consecutive) {
    if (consecutive < 1) return std::chrono::seconds(0);
    const int shift = std::min(consecutive - 1, 4);
    return std::chrono::seconds(std::min(60 << shift, 600));
}

VolumeWorker::VolumeWorker(wchar_t drive, VolumeIdentity identity, LogFunction log)
    : drive_(drive), identity_(std::move(identity)), log_(std::move(log)) {
    const std::wstring rootPath = std::wstring(1, drive_) + L":\\";
    root_ = CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

VolumeWorker::~VolumeWorker() {
    Stop();
    if (root_ != INVALID_HANDLE_VALUE) CloseHandle(root_);
}

void VolumeWorker::Start() { thread_ = std::thread([this] { Run(); }); }

void VolumeWorker::RequestStop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
}

void VolumeWorker::Stop() {
    RequestStop();
    if (thread_.joinable()) thread_.join();
}

proto::VolumeStatus VolumeWorker::Status() const {
    proto::VolumeStatus s;
    s.drive = drive_;
    s.state = static_cast<proto::VolumeState>(state_.load());
    s.memoryBytes = memoryBytes_.load();
    const std::int64_t last = lastCatchUpTicks_.load();
    if (last != 0) {
        const auto age = Clock::now() - Clock::time_point(Clock::duration(last));
        s.secondsSinceCatchUp = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(age).count());
    } else {
        s.secondsSinceCatchUp = UINT64_MAX;
    }
    return s;
}

bool VolumeWorker::QueryFolder(std::uint64_t folderId, std::uint32_t pageStart, std::uint32_t pageSize,
                               std::chrono::milliseconds timeout, FolderAnswer& answer) {
    // Shared with the job: the job may run after a timeout, so it must not
    // write into the caller's `answer`.
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        FolderAnswer answer;
    };
    auto shared = std::make_shared<Shared>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return false;
        jobs_.push_back([this, shared, folderId, pageStart, pageSize] {
            FolderAnswer a;
            Answer(folderId, pageStart, pageSize, a);
            std::lock_guard<std::mutex> l(shared->m);
            shared->answer = std::move(a);
            shared->done = true;
            shared->cv.notify_all();
        });
    }
    wake_.notify_all();
    std::unique_lock<std::mutex> lock(shared->m);
    if (!shared->cv.wait_for(lock, timeout, [&] { return shared->done; })) return false;
    answer = std::move(shared->answer);
    return true;
}

void VolumeWorker::Answer(std::uint64_t folderId, std::uint32_t pageStart, std::uint32_t pageSize,
                          FolderAnswer& answer) const {
    answer = FolderAnswer{};
    if (!volume_) return;
    answer.indexed = true;
    const LiveIndex& live = volume_->Live();
    const VolumeIndex& x = live.Index();
    const DirIndex d = live.FindDir(folderId);
    if (!live.DirLive(d)) return;
    answer.found = true;
    answer.updating = state_.load() == static_cast<int>(proto::VolumeState::Updating);
    answer.totals = x.dirs[d].subtree;
    std::vector<DirIndex> children;
    for (DirIndex c = x.dirs[d].firstChild; c != kNoDir; c = x.dirs[c].nextSibling) {
        if (live.DirLive(c)) children.push_back(c);
    }
    // Largest on disk first; ties by name, so pages are stable.
    std::sort(children.begin(), children.end(), [&](DirIndex a, DirIndex b) {
        const auto& ta = x.dirs[a].subtree;
        const auto& tb = x.dirs[b].subtree;
        if (ta.allocatedBytes != tb.allocatedBytes) return ta.allocatedBytes > tb.allocatedBytes;
        return x.Name(a) < x.Name(b);
    });
    answer.childCount = static_cast<std::uint32_t>(children.size());
    for (size_t i = pageStart; i < children.size() && i < static_cast<size_t>(pageStart) + pageSize; ++i) {
        const DirIndex c = children[i];
        answer.children.push_back({x.dirs[c].id, std::wstring(x.Name(c)), x.dirs[c].subtree});
    }
}

void VolumeWorker::Run() {
    log_(std::wstring(1, drive_) + L": starting");
    CacheOpenResult opened;
    volume_ = NtfsLiveVolume::FromCache(drive_, opened);
    if (volume_) {
        const CatchUpResult r = volume_->CatchUp();
        log_(std::wstring(1, drive_) + L": cache loaded, " + (r.needsRescan ? L"at least " : L"") +
             std::to_wstring(r.changedIds) + L" changed IDs since the save" +
             (r.needsRescan ? std::wstring(L"; cannot catch up: ") + r.reason : std::wstring()));
        memoryBytes_ = volume_->Live().Index().MemoryBytes();
        if (r.needsRescan) {
            state_ = static_cast<int>(proto::VolumeState::Updating);
            RequestRescan(r.reason);
        } else {
            state_ = static_cast<int>(proto::VolumeState::Ready);
            readySince_ = Clock::now();
            lastCatchUpTicks_ = Clock::now().time_since_epoch().count();
            changesSinceSave_ += r.changedIds;
        }
    } else {
        log_(std::wstring(1, drive_) + L": no usable cache: " + opened.reason);
        RequestRescan(L"no usable cache");
    }

    auto nextCatchUp = Clock::now() + kCatchUpInterval;
    for (;;) {
        std::deque<std::function<void()>> jobs;
        bool stop = false;
        {
            const auto deadline = rescanWanted_ && !rescanRunning_ ? std::min(nextCatchUp, rescanAllowedAt_)
                                                                    : nextCatchUp;
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_until(lock, deadline, [&] { return stopping_ || !jobs_.empty() || rescanDone_.load(); });
            jobs.swap(jobs_);
            stop = stopping_;
        }
        for (auto& job : jobs) job();
        if (rescanDone_) FinishRescan();
        if (stop) break;
        const auto now = Clock::now();
        if (rescanWanted_ && !rescanRunning_ && now >= rescanAllowedAt_) StartRescan(rescanReason_);
        if (state_.load() == static_cast<int>(proto::VolumeState::Ready) && consecutiveRescans_ != 0 &&
            now - readySince_ >= kStableBeforeBackoffReset) {
            consecutiveRescans_ = 0;  // stable again: the next rescan may start at once
        }
        if (now >= nextCatchUp) {
            nextCatchUp = now + kCatchUpInterval;
            // While a rescan runs the old index is kept as it is: it already
            // needed one, so catching it up would only ask again.
            if (volume_ && !rescanRunning_ && !rescanWanted_) {
                const CatchUpResult r = volume_->CatchUp();
                if (r.openDropped != 0) {
                    log_(std::wstring(1, drive_) + L": " + std::to_wstring(r.openDropped) +
                         L" tracked open files dropped (limit " + std::to_wstring(kMaxTrackedOpenFiles) + L")");
                }
                if (r.needsRescan) {
                    log_(std::wstring(1, drive_) + L": needs a rescan: " + r.reason + L" (Win32 error " +
                         std::to_wstring(r.error) + L")");
                    state_ = static_cast<int>(proto::VolumeState::Updating);
                    RequestRescan(r.reason);
                } else {
                    lastCatchUpTicks_ = Clock::now().time_since_epoch().count();
                    changesSinceSave_ += r.changedIds;
                    memoryBytes_ = volume_->Live().Index().MemoryBytes();
                    MaybeSave(false);
                }
            }
        }
    }
    // Save first: a running build cannot be cancelled, and the shutdown
    // window (PRESHUTDOWN, 10 s by default) may end before it does.
    MaybeSave(true);
    if (rescanThread_.joinable()) rescanThread_.join();
    log_(std::wstring(1, drive_) + L": stopped");
}

void VolumeWorker::RequestRescan(const wchar_t* why) {
    if (rescanRunning_ || rescanWanted_) return;
    rescanWanted_ = true;
    rescanReason_ = why;
    const auto now = Clock::now();
    if (rescanAllowedAt_ > now) {
        log_(std::wstring(1, drive_) + L": rescan wanted (" + why + L"), waiting " +
             std::to_wstring(std::chrono::duration_cast<std::chrono::seconds>(rescanAllowedAt_ - now).count()) +
             L" s (rescans in a row: " + std::to_wstring(consecutiveRescans_) + L")");
    }
}

void VolumeWorker::StartRescan(const std::wstring& why) {
    if (rescanRunning_) return;
    if (rescanThread_.joinable()) rescanThread_.join();
    rescanWanted_ = false;
    ++consecutiveRescans_;
    rescanAllowedAt_ = Clock::now() + RescanBackoff(consecutiveRescans_);
    log_(std::wstring(1, drive_) + L": full rescan started (" + why + L")");
    rescanRunning_ = true;
    rescanDone_ = false;
    rescanThread_ = std::thread([this] {
        // Background mode for this thread only: the threads answering
        // queries keep their normal priority (DESIGN-phase4.md section 4).
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        const auto t0 = Clock::now();
        NtfsIndexResult build;
        std::unique_ptr<NtfsLiveVolume> v = NtfsLiveVolume::Build(drive_, build);
        const double seconds = std::chrono::duration<double>(Clock::now() - t0).count();
        if (!v) {
            log_(std::wstring(1, drive_) + L": rescan failed: " + build.failedStep + L" (Win32 error " +
                 std::to_wstring(build.error) + L")");
        } else {
            log_(std::wstring(1, drive_) + L": rescan finished in " + std::to_wstring(seconds) + L" s");
        }
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        {
            std::lock_guard<std::mutex> lock(rescanMutex_);
            rescanned_ = std::move(v);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rescanDone_ = true;
        }
        wake_.notify_all();
    });
}

void VolumeWorker::FinishRescan() {
    if (rescanThread_.joinable()) rescanThread_.join();
    std::unique_ptr<NtfsLiveVolume> v;
    {
        std::lock_guard<std::mutex> lock(rescanMutex_);
        v = std::move(rescanned_);
    }
    rescanDone_ = false;
    rescanRunning_ = false;
    if (!v) {
        RequestRescan(L"retry after a failed rescan");  // subject to the backoff
        return;
    }

    // Decision 8.3: files whose size differs between the previous index and
    // the rescan are tracked from now on.
    if (volume_) {
        const LiveIndex& before = volume_->Live();
        const VolumeIndex& fresh = v->Live().Index();
        std::vector<std::uint64_t> differing;
        for (const FileEntry& f : fresh.files) {
            const std::uint32_t i = before.FindFile(f.id);
            if (i == kNoFile) continue;
            const FileEntry& old = before.Index().files[i];
            if ((old.flags & kEntryDead) == 0 &&
                (old.logicalBytes != f.logicalBytes || old.allocatedBytes != f.allocatedBytes)) {
                differing.push_back(f.id);
            }
        }
        if (differing.size() <= kMaxDifferingTracked) {
            v->TrackFiles(differing);
            log_(std::wstring(1, drive_) + L": " + std::to_wstring(differing.size()) +
                 L" files differed from the previous index; now tracked");
        } else {
            log_(std::wstring(1, drive_) + L": " + std::to_wstring(differing.size()) +
                 L" files differed from the previous index (it was out of date); none tracked");
        }
    }
    const CatchUpResult r = v->CatchUp();  // changes made while the rescan ran
    if (r.needsRescan) {
        log_(std::wstring(1, drive_) + L": the new index needs a rescan as well: " + r.reason);
        volume_ = std::move(v);
        state_ = static_cast<int>(proto::VolumeState::Updating);
        RequestRescan(r.reason);  // subject to the backoff
        return;
    }
    volume_ = std::move(v);
    state_ = static_cast<int>(proto::VolumeState::Ready);
    readySince_ = Clock::now();
    lastCatchUpTicks_ = Clock::now().time_since_epoch().count();
    memoryBytes_ = volume_->Live().Index().MemoryBytes();
    changesSinceSave_ = kSaveAfterChanges;  // a new index: save it now
    MaybeSave(false);
}

void VolumeWorker::MaybeSave(bool stopping) {
    if (!volume_ || volume_->Live().Broken() || state_.load() != static_cast<int>(proto::VolumeState::Ready)) return;
    const auto now = Clock::now();
    const bool due = changesSinceSave_ >= kSaveAfterChanges ||
                     (changesSinceSave_ > 0 && now - lastSave_ >= kSaveAfterTime) ||
                     (stopping && changesSinceSave_ > 0);
    if (!due) return;
    const CacheSaveResult s = volume_->SaveCache();
    if (s.ok) {
        log_(std::wstring(1, drive_) + L": cache saved (" + std::to_wstring(s.bytes >> 20) + L" MB, applied USN " +
             std::to_wstring(volume_->AppliedUsn()) + L", journal " + Hex64(volume_->JournalId()) + L")");
        changesSinceSave_ = 0;
        lastSave_ = now;
    } else {
        log_(std::wstring(1, drive_) + L": cache save failed: " + s.reason + L" (Win32 error " +
             std::to_wstring(s.error) + L")");
    }
}

}  // namespace dirsize::service
