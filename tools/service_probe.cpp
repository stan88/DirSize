// Phase 4 feasibility probe (measurement only, not shipped). See
// docs/DESIGN-phase4.md section 9.
//
//   service-probe access          (no elevation needed)
//     Named pipe client at SecurityImpersonation and SecurityIdentification:
//     can the server open a folder while impersonating, and does AccessCheck
//     accept the token? Tested on a folder the user can list, a folder whose
//     DACL denies the user listing, and the DirSize cache folder.
//   service-probe backup <drive>  (elevated)
//     Full build and one journal catch-up with SeBackupPrivilege disabled,
//     then enabled.
//   service-probe cpu <drive>     (elevated)
//     Measures without a job, then puts this process in a job and measures,
//     for hard caps of 1, 1/2 and 1/4 of one core: the CPU share of one busy
//     thread, and a full build's wall time with the response time of a small
//     task on another thread.

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "core/ntfs_index.h"
#include "core/ntfs_live.h"

namespace {

using Clock = std::chrono::steady_clock;

double Since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

std::uint64_t FileTimeTo100ns(const FILETIME& t) {
    return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

double ProcessCpuSeconds() {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    return (FileTimeTo100ns(k) + FileTimeTo100ns(u)) / 1e7;
}

double ThreadCpuSeconds() {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    return (FileTimeTo100ns(k) + FileTimeTo100ns(u)) / 1e7;
}

bool SetPrivilege(const wchar_t* name, bool enable) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;
    bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) &&
              GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

// ---- access ------------------------------------------------------------------------

struct AccessResult {
    DWORD impersonateError = 0;
    DWORD openTokenError = 0;
    int level = -1;  // SECURITY_IMPERSONATION_LEVEL
    DWORD openWhileImpersonating = 0;  // 0 = opened
    DWORD securityError = 0;           // GetFileSecurityW
    DWORD accessCheckError = 0;
    BOOL accessStatus = FALSE;
    DWORD granted = 0;
};

const wchar_t* LevelName(int level) {
    switch (level) {
        case SecurityAnonymous: return L"Anonymous";
        case SecurityIdentification: return L"Identification";
        case SecurityImpersonation: return L"Impersonation";
        case SecurityDelegation: return L"Delegation";
        default: return L"(unknown)";
    }
}

// Server side, after one message was read from `pipe`.
AccessResult CheckAs(HANDLE pipe, const std::wstring& folder) {
    AccessResult r;
    if (!ImpersonateNamedPipeClient(pipe)) {
        r.impersonateError = GetLastError();
        return r;
    }
    HANDLE token = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
        r.openTokenError = GetLastError();
    } else {
        SECURITY_IMPERSONATION_LEVEL level{};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenImpersonationLevel, &level, sizeof(level), &size)) r.level = level;
    }
    HANDLE h = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        r.openWhileImpersonating = GetLastError();
    } else {
        CloseHandle(h);
    }
    RevertToSelf();
    if (token == nullptr) return r;

    // AccessCheck on the folder's security descriptor, read as ourselves.
    std::vector<std::uint8_t> sd(4096);
    DWORD needed = 0;
    const SECURITY_INFORMATION what = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    if (!GetFileSecurityW(folder.c_str(), what, sd.data(), static_cast<DWORD>(sd.size()), &needed)) {
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            sd.resize(needed);
            if (!GetFileSecurityW(folder.c_str(), what, sd.data(), static_cast<DWORD>(sd.size()), &needed)) {
                r.securityError = GetLastError();
            }
        } else {
            r.securityError = GetLastError();
        }
    }
    if (r.securityError == 0) {
        GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
        DWORD desired = FILE_LIST_DIRECTORY;
        MapGenericMask(&desired, &mapping);
        std::vector<std::uint8_t> privileges(1024);
        DWORD privilegesSize = static_cast<DWORD>(privileges.size());
        if (!AccessCheck(sd.data(), token, desired, &mapping, reinterpret_cast<PPRIVILEGE_SET>(privileges.data()),
                         &privilegesSize, &r.granted, &r.accessStatus)) {
            r.accessCheckError = GetLastError();
        }
    }
    CloseHandle(token);
    return r;
}

std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    std::wstring out;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return out;
    std::vector<std::uint8_t> buffer(256);
    DWORD size = 0;
    if (GetTokenInformation(token, TokenUser, buffer.data(), static_cast<DWORD>(buffer.size()), &size)) {
        wchar_t* text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
            out = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return out;
}

int RunAccess() {
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    const std::wstring base = std::wstring(temp) + L"dirsize-probe-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring allowed = base + L"-allowed";
    const std::wstring denied = base + L"-denied";
    CreateDirectoryW(allowed.c_str(), nullptr);
    // The user may do anything except list the folder (deny ACE first).
    const std::wstring sid = CurrentUserSid();
    const std::wstring sddl = L"D:P(D;;0x1;;;" + sid + L")(A;;FA;;;" + sid + L")(A;;FA;;;SY)(A;;FA;;;BA)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
        CreateDirectoryW(denied.c_str(), &sa);
        LocalFree(sd);
    }
    wchar_t programData[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    const std::vector<std::pair<const wchar_t*, std::wstring>> folders = {
        {L"user can list", allowed},
        {L"DACL denies listing", denied},
        {L"DirSize cache folder", std::wstring(programData) + L"\\DirSize\\cache"},
    };

    std::wprintf(L"Elevated: %s\n", [] {
        HANDLE t = nullptr;
        TOKEN_ELEVATION e{};
        DWORD n = 0;
        bool yes = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t) &&
                   GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
        if (t) CloseHandle(t);
        return yes ? L"yes" : L"no";
    }());
    const std::wstring name = L"\\\\.\\pipe\\dirsize-probe-" + std::to_wstring(GetCurrentProcessId());
    for (DWORD sqos : {SECURITY_IMPERSONATION, SECURITY_IDENTIFICATION}) {
        std::wprintf(L"\n== Client connects with SECURITY_SQOS_PRESENT | %s\n",
                     sqos == SECURITY_IMPERSONATION ? L"SECURITY_IMPERSONATION" : L"SECURITY_IDENTIFICATION");
        for (const auto& [label, folder] : folders) {
            HANDLE server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                             PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                                                 PIPE_REJECT_REMOTE_CLIENTS,
                                             1, 4096, 4096, 0, nullptr);
            if (server == INVALID_HANDLE_VALUE) {
                std::wprintf(L"  CreateNamedPipeW failed: %lu\n", GetLastError());
                return 1;
            }
            AccessResult result;
            std::thread serverThread([&] {
                if (!ConnectNamedPipe(server, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
                char message[16];
                DWORD read = 0;
                ReadFile(server, message, sizeof(message), &read, nullptr);
                result = CheckAs(server, folder);
                DWORD written = 0;
                WriteFile(server, "ok", 2, &written, nullptr);
                FlushFileBuffers(server);
                DisconnectNamedPipe(server);
            });
            HANDLE client = INVALID_HANDLE_VALUE;
            for (int attempt = 0; attempt < 100 && client == INVALID_HANDLE_VALUE; ++attempt) {
                client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                     SECURITY_SQOS_PRESENT | sqos, nullptr);
                if (client == INVALID_HANDLE_VALUE) Sleep(10);
            }
            if (client != INVALID_HANDLE_VALUE) {
                DWORD n = 0;
                WriteFile(client, "hi", 2, &n, nullptr);
                char reply[8];
                ReadFile(client, reply, sizeof(reply), &n, nullptr);
                CloseHandle(client);
            } else {
                std::wprintf(L"  client could not connect: %lu\n", GetLastError());
            }
            serverThread.join();
            CloseHandle(server);
            std::wprintf(L"  [%s]\n", label);
            std::wprintf(L"    impersonate error %lu, open token error %lu, token level %s\n", result.impersonateError,
                         result.openTokenError, LevelName(result.level));
            std::wprintf(L"    open folder while impersonating: %s (error %lu)\n",
                         result.openWhileImpersonating == 0 ? L"opened" : L"failed", result.openWhileImpersonating);
            if (result.securityError != 0) {
                std::wprintf(L"    GetFileSecurityW (as the server) failed: %lu\n", result.securityError);
            } else {
                std::wprintf(L"    AccessCheck: error %lu, access %s, granted 0x%08lx\n", result.accessCheckError,
                             result.accessStatus ? L"GRANTED" : L"denied", result.granted);
            }
        }
    }
    RemoveDirectoryW(allowed.c_str());
    RemoveDirectoryW(denied.c_str());
    return 0;
}

// ---- backup ------------------------------------------------------------------------

void BuildOnce(wchar_t drive, const wchar_t* label) {
    const auto t0 = Clock::now();
    dirsize::NtfsIndexResult build;
    std::unique_ptr<dirsize::NtfsLiveVolume> v = dirsize::NtfsLiveVolume::Build(drive, build);
    std::wprintf(L"  [%s] build %s in %.3f s", label, v ? L"ok" : L"FAILED", Since(t0));
    if (!v) {
        std::wprintf(L": %s (error %lu)\n", build.failedStep, build.error);
        return;
    }
    const dirsize::VolumeIndex& x = v->Live().Index();
    const auto& t = x.dirs[x.root].subtree;
    std::wprintf(L"; folders %llu, files %llu, bytes %llu; access denied %llu, other errors %llu\n", t.folders, t.files,
                 t.logicalBytes, x.issues.accessDenied, x.issues.otherErrors);
    Sleep(2000);
    const dirsize::CatchUpResult r = v->CatchUp();
    std::wprintf(L"    journal catch-up: %s (error %lu), %llu records\n", r.needsRescan ? r.reason : L"ok", r.error,
                 r.records);
}

int RunBackup(wchar_t drive) {
    std::wprintf(L"Volume %c:\n", drive);
    std::wprintf(L"Disable SeBackupPrivilege: %s\n", SetPrivilege(SE_BACKUP_NAME, false) ? L"done" : L"failed");
    BuildOnce(drive, L"SeBackupPrivilege disabled");
    std::wprintf(L"Enable SeBackupPrivilege: %s\n", SetPrivilege(SE_BACKUP_NAME, true) ? L"done" : L"failed");
    BuildOnce(drive, L"SeBackupPrivilege enabled");
    return 0;
}

// ---- cpu ---------------------------------------------------------------------------

bool SetCap(HANDLE job, DWORD rate) {
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION info{};
    if (rate != 0) {
        info.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        info.CpuRate = rate;
    }
    return SetInformationJobObject(job, JobObjectCpuRateControlInformation, &info, sizeof(info)) != 0;
}

// A small fixed task, timed repeatedly on another thread: how long does a
// UI request wait while the build runs under the cap?
struct Latency {
    std::atomic<bool> stop{false};
    std::vector<double> samples;
    std::thread thread;
    void Start() {
        thread = std::thread([this] {
            volatile std::uint64_t sink = 0;
            while (!stop) {
                const auto t = Clock::now();
                for (int i = 0; i < 200000; ++i) sink = sink + static_cast<std::uint64_t>(i) * 7;
                samples.push_back(Since(t) * 1000);
                Sleep(20);
            }
        });
    }
    void Finish(double& p50, double& p99, double& worst) {
        stop = true;
        thread.join();
        std::sort(samples.begin(), samples.end());
        if (samples.empty()) return;
        p50 = samples[samples.size() / 2];
        p99 = samples[std::min(samples.size() - 1, samples.size() * 99 / 100)];
        worst = samples.back();
    }
};

// Runs a full build with the latency task and prints one line.
void TimedBuild(wchar_t drive, const wchar_t* label) {
    Latency latency;
    latency.Start();
    const double cpu0 = ProcessCpuSeconds();
    const auto t0 = Clock::now();
    dirsize::NtfsIndexResult build = dirsize::BuildNtfsIndex(drive);
    const double wall = Since(t0);
    const double cpu = ProcessCpuSeconds() - cpu0;
    double p50 = 0, p99 = 0, worst = 0;
    latency.Finish(p50, p99, worst);
    std::wprintf(L"  %-22s build %s, wall %.2f s, CPU %.2f s; task ms p50 %.2f, p99 %.2f, max %.2f (%zu samples)\n",
                 label, build.ok ? L"ok" : L"FAILED", wall, cpu, p50, p99, worst, latency.samples.size());
}

double BusyThreadShare() {
    double share = 0;
    std::thread([&] {
        const double cpu0 = ThreadCpuSeconds();
        const auto t0 = Clock::now();
        volatile std::uint64_t sink = 0;
        while (Since(t0) < 3.0) sink = sink + 1;
        share = (ThreadCpuSeconds() - cpu0) / Since(t0);
    }).join();
    return share;
}

// The job's CpuRate is a share of the whole machine (first run: a 10% cap on
// 20 logical processors still let one thread use 98% of a core), so caps are
// given here as a share of ONE core: CpuRate = share * 10000 / processors.
int RunCpu(wchar_t drive) {
    const DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    std::wprintf(L"Volume %c:, logical processors %lu\n", drive, processors);
    SetPrivilege(SE_BACKUP_NAME, true);

    std::wprintf(L"\n== No job\n");
    std::wprintf(L"  one busy thread got %.1f%% of one core\n", BusyThreadShare() * 100);
    TimedBuild(drive, L"no cap:");

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr || !AssignProcessToJobObject(job, GetCurrentProcess())) {
        std::wprintf(L"job setup failed: %lu\n", GetLastError());
        return 1;
    }
    struct Cap {
        const wchar_t* label;
        double coreShare;
    };
    const Cap caps[] = {{L"1 core:", 1.0}, {L"1/2 core:", 0.5}, {L"1/4 core:", 0.25}};
    std::wprintf(L"\n== In a job with a hard cap given as a share of one core\n");
    for (const Cap& c : caps) {
        const DWORD rate = std::max<DWORD>(1, static_cast<DWORD>(c.coreShare * 10000 / processors + 0.5));
        if (!SetCap(job, rate)) {
            std::wprintf(L"  SetInformationJobObject(CpuRate %lu) failed: %lu\n", rate, GetLastError());
            continue;
        }
        std::wprintf(L"  [%s CpuRate %lu = %.2f%% of the machine] one busy thread got %.1f%% of one core\n", c.label, rate,
                     rate / 100.0, BusyThreadShare() * 100);
        TimedBuild(drive, c.label);
    }
    CloseHandle(job);
    return 0;
}

bool DriveArg(int argc, wchar_t** argv, wchar_t& drive) {
    if (argc < 3 || !iswalpha(argv[2][0])) return false;
    drive = static_cast<wchar_t>(towupper(argv[2][0]));
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t drive = 0;
    if (argc >= 2 && std::wstring(argv[1]) == L"access") return RunAccess();
    if (argc >= 2 && std::wstring(argv[1]) == L"backup" && DriveArg(argc, argv, drive)) return RunBackup(drive);
    if (argc >= 2 && std::wstring(argv[1]) == L"cpu" && DriveArg(argc, argv, drive)) return RunCpu(drive);
    std::wprintf(L"Usage: service-probe access | backup <drive> | cpu <drive>\n");
    return 2;
}
