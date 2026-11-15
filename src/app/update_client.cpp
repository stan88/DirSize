#include "app/update_client.h"

#include <winhttp.h>

#include <string>
#include <thread>

namespace dirsize::app {
namespace {

constexpr wchar_t kHost[] = L"api.github.com";
constexpr wchar_t kPath[] = L"/repos/stan88/DirSize/releases/latest";
// Recommended by GitHub's REST documentation; the API version pins the
// answer's format.
constexpr wchar_t kHeaders[] =
    L"Accept: application/vnd.github+json\r\n"
    L"X-GitHub-Api-Version: 2026-03-10\r\n";
constexpr DWORD kTimeoutMs = 15000;
constexpr size_t kMaxBody = 1024 * 1024;  // the answer is a few KB

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h != nullptr) WinHttpCloseHandle(h);
    }
};

// The HTTP status and body, or false with a Win32 error.
bool Fetch(const std::wstring& agent, DWORD& status, std::string& body, DWORD& error) {
    // GitHub requires a User-Agent; it carries only the program name and
    // version.
    Handle session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0)};
    if (session.h == nullptr) return error = GetLastError(), false;
    if (!WinHttpSetTimeouts(session.h, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs)) {
        return error = GetLastError(), false;
    }
    Handle connection{WinHttpConnect(session.h, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (connection.h == nullptr) return error = GetLastError(), false;
    Handle request{WinHttpOpenRequest(connection.h, L"GET", kPath, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (request.h == nullptr) return error = GetLastError(), false;
    if (!WinHttpSendRequest(request.h, kHeaders, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        return error = GetLastError(), false;
    }
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        return error = GetLastError(), false;
    }
    body.clear();
    char buffer[8192];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.h, buffer, sizeof(buffer), &read)) return error = GetLastError(), false;
        if (read == 0) break;
        if (body.size() + read > kMaxBody) return error = ERROR_INVALID_DATA, false;
        body.append(buffer, read);
    }
    error = ERROR_SUCCESS;
    return true;
}

}  // namespace

bool StartUpdateCheck(HWND notify, UINT message, const std::wstring& currentVersion) {
    // The thread holds the window handle, not the window object: the window
    // may close while the request runs.
    try {
        std::thread([notify, message, currentVersion] {
            auto* outcome = new UpdateOutcome;
            DWORD status = 0, error = 0;
            std::string body;
            if (Fetch(L"DirSize/" + currentVersion, status, body, error)) {
                outcome->state = ui::ClassifyUpdate(status, body, currentVersion, outcome->latest);
                outcome->code = status;
            } else {
                outcome->state = ui::UpdateState::NetworkError;
                outcome->code = error;
            }
            if (!PostMessageW(notify, message, 0, reinterpret_cast<LPARAM>(outcome))) delete outcome;
        }).detach();
    } catch (...) {
        return false;  // no thread
    }
    return true;
}

}  // namespace dirsize::app
