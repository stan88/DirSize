// dirsize-service: the DirSize Windows service (docs/DESIGN-phase4.md).
//
//   dirsize-service             run under the service control manager
//   dirsize-service console     run in this console until Ctrl+C (tests)
//   dirsize-service install     copy to Program Files\DirSize and register (elevated)
//   dirsize-service uninstall   stop, unregister, remove the copy (elevated)

#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "service/service_core.h"

namespace {

using namespace dirsize::service;

constexpr wchar_t kServiceName[] = L"DirSize";
constexpr wchar_t kDisplayName[] = L"DirSize";
constexpr wchar_t kDescription[] = L"Keeps the folder sizes of local NTFS drives up to date for DirSize.";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stop = nullptr;
std::atomic<DWORD> g_checkPoint{0};

void Report(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted =
        state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN) : 0;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : ++g_checkPoint;
    SetServiceStatus(g_statusHandle, &g_status);
}

// Returns at once (the documentation allows 30 s); the work happens on the
// service thread.
DWORD WINAPI Handler(DWORD control, DWORD, LPVOID, LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_PRESHUTDOWN:
            Report(SERVICE_STOP_PENDING, NO_ERROR, 30000);
            SetEvent(g_stop);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, Handler, nullptr);
    if (g_statusHandle == nullptr) return;
    Report(SERVICE_START_PENDING, NO_ERROR, 30000);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Log log(false);
    ServiceCore core(log.Function());
    const DWORD error = core.Start();
    if (error != ERROR_SUCCESS) {
        Report(SERVICE_STOPPED, error);
        return;
    }
    Report(SERVICE_RUNNING);
    WaitForSingleObject(g_stop, INFINITE);
    // Stopping can take a while when a rescan is running; keep the SCM
    // informed so it does not assume the service hung.
    std::atomic<bool> stopped{false};
    std::thread progress([&] {
        while (!stopped) {
            Sleep(2000);
            if (!stopped) Report(SERVICE_STOP_PENDING, NO_ERROR, 30000);
        }
    });
    core.Stop();
    stopped = true;
    progress.join();
    Report(SERVICE_STOPPED);
}

// ---- console mode ------------------------------------------------------------------------

HANDLE g_consoleStop = nullptr;

BOOL WINAPI ConsoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        SetEvent(g_consoleStop);
        return TRUE;
    }
    return FALSE;
}

int RunConsole() {
    g_consoleStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    Log log(true);
    if (!log.Path().empty()) std::wprintf(L"log file: %s\n", log.Path().c_str());
    ServiceCore core(log.Function());
    const DWORD error = core.Start();
    if (error != ERROR_SUCCESS) {
        std::wprintf(L"start failed: Win32 error %lu\n", error);
        return 1;
    }
    std::wprintf(L"running; press Ctrl+C to stop\n");
    WaitForSingleObject(g_consoleStop, INFINITE);
    core.Stop();
    return 0;
}

// ---- install / uninstall -------------------------------------------------------------------

std::wstring InstalledPath() {
    PWSTR programFiles = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles))) {
        path = std::wstring(programFiles) + L"\\DirSize\\dirsize-service.exe";
    }
    CoTaskMemFree(programFiles);
    return path;
}

int Fail(const wchar_t* what) {
    std::wprintf(L"%s failed: Win32 error %lu\n", what, GetLastError());
    return 1;
}

int Install() {
    // The service runs as LocalSystem, so its executable must not be
    // writable by ordinary users: copy it under Program Files.
    const std::wstring target = InstalledPath();
    if (target.empty()) return Fail(L"find Program Files");
    const std::wstring folder = target.substr(0, target.find_last_of(L'\\'));
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return Fail(L"create the Program Files folder");
    }
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return Fail(L"GetModuleFileNameW");
    if (!CopyFileW(self, target.c_str(), FALSE)) return Fail(L"copy the executable");

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (scm == nullptr) return Fail(L"OpenSCManagerW (run elevated)");
    const std::wstring command = L"\"" + target + L"\"";
    SC_HANDLE service = CreateServiceW(scm, kServiceName, kDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                       SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr,
                                       nullptr, nullptr, nullptr);  // LocalSystem
    if (service == nullptr) {
        const int r = Fail(L"CreateServiceW");
        CloseServiceHandle(scm);
        return r;
    }
    SERVICE_DESCRIPTIONW description{const_cast<LPWSTR>(kDescription)};
    SERVICE_DELAYED_AUTO_START_INFO delayed{TRUE};
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 60000}, {SC_ACTION_RESTART, 60000}, {SC_ACTION_NONE, 0}};
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = 24 * 60 * 60;
    failure.cActions = 3;
    failure.lpsaActions = actions;
    SERVICE_FAILURE_ACTIONS_FLAG failureFlag{TRUE};
    wchar_t privileges[] = L"SeChangeNotifyPrivilege\0";  // multi-string: ends with two NULs
    SERVICE_REQUIRED_PRIVILEGES_INFOW required{privileges};
    SERVICE_SID_INFO sid{SERVICE_SID_TYPE_UNRESTRICTED};
    struct Setting {
        DWORD level;
        void* info;
        const wchar_t* name;
    } settings[] = {
        {SERVICE_CONFIG_DESCRIPTION, &description, L"description"},
        {SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed, L"delayed auto-start"},
        {SERVICE_CONFIG_FAILURE_ACTIONS, &failure, L"failure actions"},
        {SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failureFlag, L"failure actions flag"},
        {SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &required, L"required privileges"},
        {SERVICE_CONFIG_SERVICE_SID_INFO, &sid, L"service SID"},
    };
    int result = 0;
    for (const Setting& s : settings) {
        if (!ChangeServiceConfig2W(service, s.level, s.info)) {
            std::wprintf(L"ChangeServiceConfig2W(%s) failed: Win32 error %lu\n", s.name, GetLastError());
            result = 1;
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    if (result == 0) {
        std::wprintf(L"installed %s as service \"%s\" (automatic, delayed start). Start it with:\n"
                     L"  Start-Service DirSize\n",
                     target.c_str(), kServiceName);
    }
    return result;
}

int Uninstall() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) return Fail(L"OpenSCManagerW (run elevated)");
    SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (service == nullptr) {
        const int r = Fail(L"OpenServiceW");
        CloseServiceHandle(scm);
        return r;
    }
    SERVICE_STATUS status{};
    if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        std::wprintf(L"stopping...\n");
        for (int i = 0; i < 120; ++i) {
            SERVICE_STATUS_PROCESS p{};
            DWORD needed = 0;
            if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&p), sizeof(p),
                                      &needed) ||
                p.dwCurrentState == SERVICE_STOPPED) {
                break;
            }
            Sleep(500);
        }
    }
    const int result = DeleteService(service) ? 0 : Fail(L"DeleteService");
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    const std::wstring target = InstalledPath();
    if (!target.empty() && DeleteFileW(target.c_str())) {
        RemoveDirectoryW(target.substr(0, target.find_last_of(L'\\')).c_str());
    }
    if (result == 0) std::wprintf(L"uninstalled (the cache in ProgramData\\DirSize is kept)\n");
    return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"console") return RunConsole();
    if (argc == 2 && std::wstring(argv[1]) == L"install") return Install();
    if (argc == 2 && std::wstring(argv[1]) == L"uninstall") return Uninstall();
    if (argc == 1) {
        SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), ServiceMain}, {nullptr, nullptr}};
        if (StartServiceCtrlDispatcherW(table)) return 0;
        if (GetLastError() != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) return 1;
        // Started from a console, not by the SCM: fall through to the usage.
    }
    std::wprintf(L"Usage: dirsize-service console | install | uninstall\n"
                 L"  console    run in this console until Ctrl+C (elevated)\n"
                 L"  install    copy to Program Files\\DirSize and register the service (elevated)\n"
                 L"  uninstall  stop and remove the service (elevated)\n");
    return 2;
}
