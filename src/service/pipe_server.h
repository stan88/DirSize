#pragma once

// The service's named pipe (docs/DESIGN-phase4.md section 6).
//
// \\.\pipe\DirSize, message mode, remote clients rejected. The first
// instance is created with FILE_FLAG_FIRST_PIPE_INSTANCE: if the name exists
// already (another process got there first), Start() fails and the service
// stops instead of serving under that name. A fixed number of instances, one
// thread each, with overlapped I/O so every wait has a timeout and Stop()
// interrupts it.

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "service/volume_worker.h"

namespace dirsize::service {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\DirSize";
constexpr int kPipeInstances = 4;
constexpr DWORD kRequestTimeoutMs = 5000;

struct HandledRequest {
    std::vector<std::byte> response;
    bool closeAfter = false;  // malformed request: drop the connection
};

// Decodes one request, checks the caller's access and answers it. `pipe` is
// the server end the request was read from (for the caller's token).
HandledRequest HandleRequest(const std::byte* data, size_t size, HANDLE pipe,
                             const std::vector<VolumeWorker*>& workers, const LogFunction& log);

class PipeServer {
public:
    PipeServer(std::vector<VolumeWorker*> workers, LogFunction log, std::wstring name = kPipeName);
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Creates the instances and starts serving. Returns ERROR_SUCCESS or the
    // error of the first CreateNamedPipeW (ERROR_ACCESS_DENIED: name taken).
    DWORD Start();
    void Stop();

private:
    void Serve(HANDLE pipe);

    std::vector<VolumeWorker*> workers_;
    LogFunction log_;
    std::wstring name_;
    HANDLE stop_ = nullptr;  // manual-reset event
    std::vector<HANDLE> pipes_;
    std::vector<std::thread> threads_;
};

}  // namespace dirsize::service
