#pragma once

// Client of the DirSize service pipe (docs/DESIGN-phase4.md section 6).
//
// Connects at SecurityIdentification level, so the server can tell who is
// asking but cannot act as the user. Before sending anything it checks that
// the pipe's server process is the DirSize service the SCM reports
// (GetNamedPipeServerProcessId against QueryServiceStatusEx); a process that
// took the pipe name first is refused. For tests against "dirsize-service
// console" that check can be turned off.

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

#include "service/protocol.h"

namespace dirsize::client {

constexpr wchar_t kServicePipeName[] = L"\\\\.\\pipe\\DirSize";

// What the client asks for on the pipe: only the rights it uses. (Measured in
// service-tests: with the service's Authenticated Users entry, which leaves
// out FILE_CREATE_PIPE_INSTANCE, both these rights and
// GENERIC_READ | GENERIC_WRITE connect.)
constexpr DWORD kClientPipeAccess =
    FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;

struct ClientError {
    unsigned long win32 = 0;
    std::wstring what;
};

class PipeClient {
public:
    PipeClient() = default;
    ~PipeClient();
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // `requireService`: refuse unless the server is the running DirSize service.
    // `pipeName` is for tests; the service's pipe is the default.
    bool Connect(bool requireService, ClientError& error, const wchar_t* pipeName = kServicePipeName);
    unsigned long ServerProcessId() const { return serverPid_; }

    // Sends one request and reads one response message.
    bool Exchange(const std::vector<std::byte>& request, std::vector<std::byte>& response, ClientError& error);

    bool Status(proto::StatusResponse& status, proto::ErrorResponse& serverError, ClientError& error);
    bool QueryFolder(const proto::QueryFolderRequest& request, proto::FolderResponse& folder,
                     proto::ErrorResponse& serverError, ClientError& error);

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    unsigned long serverPid_ = 0;
};

// Process ID of the running DirSize service, or 0 (not installed, stopped).
unsigned long DirSizeServiceProcessId(unsigned long& error);

}  // namespace dirsize::client
