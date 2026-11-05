#include "service/service_core.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "core/cache_store.h"

namespace dirsize::service {

// ---- log ---------------------------------------------------------------------------

Log::Log(bool echo) : echo_(echo) {
    std::wstring cache;
    // The cache folder's parent is the private DirSize folder (checked there).
    if (PrepareCacheFolder(true, cache).ok) {
        const size_t slash = cache.find_last_of(L'\\');
        if (slash != std::wstring::npos) path_ = cache.substr(0, slash) + L"\\service.log";
    }
}

void Log::Write(const std::wstring& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u.%03u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, t.wMilliseconds);
    const std::wstring text = stamp + line;
    if (echo_) std::fwprintf(stdout, L"%s\n", text.c_str());
    if (path_.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeHigh == 0 &&
        info.nFileSizeLow > (1u << 20)) {
        MoveFileExW(path_.c_str(), (path_ + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    HANDLE h = CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    // UTF-8 lines.
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                      nullptr);
    std::string utf8(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), n, nullptr, nullptr);
    utf8 += "\r\n";
    DWORD written = 0;
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
}

// ---- core --------------------------------------------------------------------------

ServiceCore::ServiceCore(LogFunction log) : log_(std::move(log)) {}

ServiceCore::~ServiceCore() {
    Stop();
    if (job_ != nullptr) CloseHandle(job_);  // no KILL_ON_JOB_CLOSE: the process keeps running
}

void ServiceCore::ApplyResourceLimits() {
    // Decision 8.4: at most one core for the whole process. The job's CpuRate
    // is a share of the whole machine (measured), so one core is
    // 10000 / logical processors.
    const DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_ == nullptr || !AssignProcessToJobObject(job_, GetCurrentProcess())) {
        log_(L"CPU cap not applied: job setup failed, Win32 error " + std::to_wstring(GetLastError()));
    } else {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
        cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        cpu.CpuRate = processors > 0 ? std::max<DWORD>(1, 10000 / processors) : 10000;
        if (SetInformationJobObject(job_, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu))) {
            log_(L"CPU cap: one core (CpuRate " + std::to_wstring(cpu.CpuRate) + L", " + std::to_wstring(processors) +
                 L" logical processors)");
        } else {
            log_(L"CPU cap not applied: SetInformationJobObject failed, Win32 error " +
                 std::to_wstring(GetLastError()));
        }
    }
    // EcoQoS: the service's work never contributes to the foreground.
    PROCESS_POWER_THROTTLING_STATE eco{};
    eco.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    eco.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    eco.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &eco, sizeof(eco))) {
        log_(L"EcoQoS not applied: Win32 error " + std::to_wstring(GetLastError()));
    }
}

DWORD ServiceCore::Start() {
    log_(L"DirSize service starting");
    ApplyResourceLimits();

    // Decision 8.2: every local fixed NTFS volume with a drive letter.
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        const wchar_t drive = static_cast<wchar_t>(L'A' + i);
        const std::wstring root = std::wstring(1, drive) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        wchar_t fs[MAX_PATH + 1] = {};
        if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs, MAX_PATH + 1) ||
            std::wstring(fs) != L"NTFS") {
            log_(root + L" skipped: not NTFS (" + fs + L")");
            continue;
        }
        VolumeIdentity identity;
        const StoreResult id = GetVolumeIdentity(drive, identity);
        if (!id.ok) {
            log_(root + L" skipped: " + id.reason + L" (Win32 error " + std::to_wstring(id.error) + L")");
            continue;
        }
        workers_.push_back(std::make_unique<VolumeWorker>(drive, identity, log_));
    }
    std::vector<VolumeWorker*> raw;
    for (auto& w : workers_) raw.push_back(w.get());
    pipe_ = std::make_unique<PipeServer>(raw, log_);
    const DWORD pipeError = pipe_->Start();
    if (pipeError != ERROR_SUCCESS) {
        // ERROR_ACCESS_DENIED with FILE_FLAG_FIRST_PIPE_INSTANCE: the name was
        // taken by someone else. Never serve under another name.
        log_(L"pipe could not be created: Win32 error " + std::to_wstring(pipeError));
        pipe_.reset();
        workers_.clear();
        return pipeError;
    }
    for (auto& w : workers_) w->Start();
    log_(L"serving " + std::to_wstring(workers_.size()) + L" volumes on " + kPipeName);
    return ERROR_SUCCESS;
}

void ServiceCore::Stop() {
    if (pipe_) {
        pipe_->Stop();
        pipe_.reset();
    }
    // All workers save at the same time; none waits behind another's rescan.
    for (auto& w : workers_) w->RequestStop();
    for (auto& w : workers_) w->Stop();
    if (!workers_.empty()) log_(L"DirSize service stopped");
    workers_.clear();
}

}  // namespace dirsize::service
