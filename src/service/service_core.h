#pragma once

// What the DirSize service does, independent of how it runs (as a Windows
// service or in console mode for tests). docs/DESIGN-phase4.md sections 3-6.

#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "service/pipe_server.h"
#include "service/volume_worker.h"

namespace dirsize::service {

// Appends to %ProgramData%\DirSize\service.log (the private DirSize folder),
// keeping one older file when it passes 1 MB; also prints when `echo`.
class Log {
public:
    explicit Log(bool echo);
    void Write(const std::wstring& line);
    LogFunction Function() {
        return [this](const std::wstring& line) { Write(line); };
    }
    const std::wstring& Path() const { return path_; }

private:
    std::mutex mutex_;
    std::wstring path_;
    bool echo_;
};

class ServiceCore {
public:
    explicit ServiceCore(LogFunction log);
    ~ServiceCore();

    // Resource limits, volumes, workers, pipe. Returns ERROR_SUCCESS or the
    // error that stopped it (for example ERROR_ACCESS_DENIED when the pipe
    // name is already taken).
    DWORD Start();
    void Stop();  // stops the pipe, then the workers (each saves its cache)

private:
    void ApplyResourceLimits();

    LogFunction log_;
    HANDLE job_ = nullptr;
    std::vector<std::unique_ptr<VolumeWorker>> workers_;
    std::unique_ptr<PipeServer> pipe_;
};

}  // namespace dirsize::service
