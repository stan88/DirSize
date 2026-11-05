#include "client/pipe_client.h"

namespace dirsize::client {
namespace {

bool Fail(ClientError& e, const wchar_t* what, unsigned long win32) {
    e.what = what;
    e.win32 = win32;
    return false;
}

}  // namespace

unsigned long DirSizeServiceProcessId(unsigned long& error) {
    error = ERROR_SUCCESS;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) {
        error = GetLastError();
        return 0;
    }
    unsigned long pid = 0;
    SC_HANDLE service = OpenServiceW(scm, L"DirSize", SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        error = GetLastError();
    } else {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status),
                                  &needed)) {
            error = GetLastError();
        } else if (status.dwCurrentState == SERVICE_RUNNING) {
            pid = status.dwProcessId;  // valid only while running (documented)
        } else {
            error = ERROR_SERVICE_NOT_ACTIVE;
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
    return pid;
}

PipeClient::~PipeClient() {
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
}

bool PipeClient::Connect(bool requireService, ClientError& error, const wchar_t* pipeName) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        pipe_ = CreateFileW(pipeName, kClientPipeAccess, 0, nullptr, OPEN_EXISTING,
                            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) break;
        const DWORD e = GetLastError();
        if (e != ERROR_PIPE_BUSY) return Fail(error, L"connect to the DirSize service", e);
        if (!WaitNamedPipeW(pipeName, 2000)) return Fail(error, L"wait for a free pipe instance", GetLastError());
    }
    if (pipe_ == INVALID_HANDLE_VALUE) return Fail(error, L"connect to the DirSize service", ERROR_PIPE_BUSY);
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr)) {
        return Fail(error, L"set message mode", GetLastError());
    }
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe_, &pid)) return Fail(error, L"get the server process ID", GetLastError());
    serverPid_ = pid;
    if (requireService) {
        unsigned long scmError = 0;
        const unsigned long servicePid = DirSizeServiceProcessId(scmError);
        if (servicePid == 0) return Fail(error, L"find the running DirSize service", scmError);
        if (servicePid != pid) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
            return Fail(error, L"the pipe is served by another process, not the DirSize service", ERROR_ACCESS_DENIED);
        }
    }
    return true;
}

bool PipeClient::Exchange(const std::vector<std::byte>& request, std::vector<std::byte>& response,
                          ClientError& error) {
    DWORD n = 0;
    if (!WriteFile(pipe_, request.data(), static_cast<DWORD>(request.size()), &n, nullptr) || n != request.size()) {
        return Fail(error, L"send the request", GetLastError());
    }
    response.resize(proto::kMaxResponseBytes);
    if (!ReadFile(pipe_, response.data(), static_cast<DWORD>(response.size()), &n, nullptr)) {
        return Fail(error, L"read the response", GetLastError());  // ERROR_MORE_DATA: larger than allowed
    }
    response.resize(n);
    return true;
}

namespace {

// Decodes an error response, or the expected type with `decode`.
template <typename Decode>
bool Receive(const std::vector<std::byte>& bytes, proto::MessageType expected, Decode decode,
             proto::ErrorResponse& serverError, ClientError& error) {
    proto::Message m;
    if (proto::DecodeMessage(bytes.data(), bytes.size(), proto::kMaxResponseBytes, m) != proto::DecodeStatus::Ok) {
        return Fail(error, L"malformed response", ERROR_INVALID_DATA);
    }
    if (m.type == proto::MessageType::ErrorResponse) {
        if (proto::DecodeError(m, serverError) != proto::DecodeStatus::Ok) {
            return Fail(error, L"malformed error response", ERROR_INVALID_DATA);
        }
        return Fail(error, L"the service refused the request", ERROR_SUCCESS);
    }
    if (m.type != expected || decode(m) != proto::DecodeStatus::Ok) {
        return Fail(error, L"malformed response", ERROR_INVALID_DATA);
    }
    return true;
}

}  // namespace

bool PipeClient::Status(proto::StatusResponse& status, proto::ErrorResponse& serverError, ClientError& error) {
    std::vector<std::byte> response;
    if (!Exchange(proto::EncodeStatusRequest(), response, error)) return false;
    return Receive(response, proto::MessageType::StatusResponse,
                   [&](const proto::Message& m) { return proto::DecodeStatusResponse(m, status); }, serverError, error);
}

bool PipeClient::QueryFolder(const proto::QueryFolderRequest& request, proto::FolderResponse& folder,
                             proto::ErrorResponse& serverError, ClientError& error) {
    std::vector<std::byte> response;
    if (!Exchange(proto::EncodeQueryFolder(request), response, error)) return false;
    return Receive(response, proto::MessageType::FolderResponse,
                   [&](const proto::Message& m) { return proto::DecodeFolderResponse(m, folder); }, serverError, error);
}

}  // namespace dirsize::client
