#include "service/pipe_server.h"

#include <sddl.h>

#include "core/dir_listing.h"
#include "service/access.h"

namespace dirsize::service {
namespace {

// SYSTEM, Administrators and the owner (the creating process: SYSTEM for
// the service): full access, which includes FILE_CREATE_PIPE_INSTANCE needed
// for the second and later instances. Authenticated Users: read and write
// (FILE_GENERIC_READ | FILE_GENERIC_WRITE) without FILE_CREATE_PIPE_INSTANCE
// (0x4), so they cannot add instances of their own. No access for anonymous
// or Everyone.
constexpr wchar_t kPipeSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)(A;;0x0012019B;;;AU)";

// Waits for an overlapped operation with a timeout; on timeout or stop it
// cancels and still waits for the completion, as CancelIoEx requires.
bool WaitIo(HANDLE h, OVERLAPPED& ov, DWORD timeoutMs, HANDLE stop, DWORD& bytes) {
    HANDLE events[2] = {ov.hEvent, stop};
    const DWORD w = WaitForMultipleObjects(2, events, FALSE, timeoutMs);
    if (w != WAIT_OBJECT_0) CancelIoEx(h, &ov);
    const BOOL ok = GetOverlappedResult(h, &ov, &bytes, TRUE);
    return w == WAIT_OBJECT_0 && ok;
}

proto::ErrorCode ErrorForOpen(DWORD error) {
    switch (error) {
        case ERROR_ACCESS_DENIED: return proto::ErrorCode::AccessDenied;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_DIRECTORY:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME: return proto::ErrorCode::NotFound;
        default: return proto::ErrorCode::NotFound;
    }
}

HandledRequest Error(proto::ErrorCode code, bool close = false) { return {proto::EncodeError(code), close}; }

}  // namespace

HandledRequest HandleRequest(const std::byte* data, size_t size, HANDLE pipe,
                             const std::vector<VolumeWorker*>& workers, const LogFunction& log) {
    proto::Message m;
    const proto::DecodeStatus header = proto::DecodeMessage(data, size, proto::kMaxRequestBytes, m);
    if (header == proto::DecodeStatus::BadVersion) return Error(proto::ErrorCode::UnsupportedVersion, true);
    if (header != proto::DecodeStatus::Ok) return Error(proto::ErrorCode::BadRequest, true);

    if (m.type == proto::MessageType::StatusRequest) {
        if (proto::DecodeStatusRequest(m) != proto::DecodeStatus::Ok) return Error(proto::ErrorCode::BadRequest, true);
        proto::StatusResponse s;
        for (VolumeWorker* w : workers) s.volumes.push_back(w->Status());
        return {proto::EncodeStatusResponse(s), false};
    }
    if (m.type != proto::MessageType::QueryFolderRequest) return Error(proto::ErrorCode::BadRequest, true);

    proto::QueryFolderRequest q;
    if (proto::DecodeQueryFolder(m, q) != proto::DecodeStatus::Ok) return Error(proto::ErrorCode::BadRequest, true);
    if (CheckRequestPath(q.path) != PathVerdict::Ok) return Error(proto::ErrorCode::PathRejected);

    // Who is asking. Any failure denies: the service must never fall back to
    // its own rights.
    ClientToken token;
    if (const DWORD e = token.FromPipe(pipe); e != ERROR_SUCCESS) {
        log(L"client token refused: Win32 error " + std::to_wstring(e));
        return Error(proto::ErrorCode::AccessDenied);
    }
    DWORD error = 0;
    HANDLE folder = OpenFolderAsService(q.path, error);
    if (folder == INVALID_HANDLE_VALUE) {
        if (error == ERROR_ACCESS_DENIED) log(L"query refused: the service cannot open the folder (Win32 error 5)");
        return Error(ErrorForOpen(error));
    }
    struct Close {
        HANDLE h;
        ~Close() { CloseHandle(h); }
    } closeFolder{folder};
    if (!ClientMayList(token.get(), folder, error)) {
        log(L"query refused by AccessCheck for the caller (Win32 error " + std::to_wstring(error) + L")");
        return Error(proto::ErrorCode::AccessDenied);
    }

    std::wstring guid;
    if (!VolumeGuidOfHandle(folder, guid)) return Error(proto::ErrorCode::NotIndexed);
    VolumeWorker* worker = nullptr;
    for (VolumeWorker* w : workers) {
        if (w->Identity().guidText == guid) worker = w;
    }
    if (worker == nullptr) return Error(proto::ErrorCode::NotIndexed);
    std::uint64_t id = 0;
    if (!QueryFileId(folder, id)) return Error(proto::ErrorCode::Internal);

    FolderAnswer a;
    if (!worker->QueryFolder(id, q.pageStart, q.pageSize, std::chrono::milliseconds(3000), a)) {
        return Error(proto::ErrorCode::Busy);
    }
    if (!a.indexed) return Error(proto::ErrorCode::Busy);  // first build still running
    if (!a.found) return Error(proto::ErrorCode::NotFound);

    proto::FolderResponse r;
    r.updating = a.updating;
    r.totals = a.totals;
    r.childCount = a.childCount;
    r.pageStart = q.pageStart < a.childCount ? q.pageStart : a.childCount;
    for (const FolderAnswer::Child& c : a.children) {
        // Decision 8.1: sizes are shown for every child; the ones the caller
        // may not list are marked.
        bool accessible = false;
        FILE_ID_DESCRIPTOR descriptor{};
        descriptor.dwSize = sizeof(descriptor);
        descriptor.Type = FileIdType;
        descriptor.FileId.QuadPart = static_cast<LONGLONG>(c.id);
        HANDLE h = OpenFileById(worker->RootHandle(), &descriptor, READ_CONTROL | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                FILE_FLAG_BACKUP_SEMANTICS);
        if (h != INVALID_HANDLE_VALUE) {
            accessible = ClientMayList(token.get(), h, error);
            CloseHandle(h);
        }
        r.children.push_back({c.name, c.totals, accessible});
    }
    return {proto::EncodeFolderResponse(r), false};
}

PipeServer::PipeServer(std::vector<VolumeWorker*> workers, LogFunction log, std::wstring name)
    : workers_(std::move(workers)), log_(std::move(log)), name_(std::move(name)) {}

PipeServer::~PipeServer() { Stop(); }

DWORD PipeServer::Start() {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSddl, SDDL_REVISION_1, &sd, nullptr)) {
        return GetLastError();
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD result = ERROR_SUCCESS;
    for (int i = 0; i < kPipeInstances; ++i) {
        const DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (i == 0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        HANDLE p = CreateNamedPipeW(name_.c_str(), openMode,
                                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    kPipeInstances, static_cast<DWORD>(proto::kMaxResponseBytes),
                                    static_cast<DWORD>(proto::kMaxRequestBytes), 0, &sa);
        if (p == INVALID_HANDLE_VALUE) {
            result = GetLastError();
            log_(L"CreateNamedPipeW for instance " + std::to_wstring(i + 1) + L" failed: Win32 error " +
                 std::to_wstring(result) +
                 (i == 0 && (result == ERROR_ACCESS_DENIED || result == ERROR_PIPE_BUSY)
                      ? L" (the pipe name is already in use by another process)"
                      : L""));
            break;
        }
        pipes_.push_back(p);
    }
    LocalFree(sd);
    if (result != ERROR_SUCCESS) {
        for (HANDLE p : pipes_) CloseHandle(p);
        pipes_.clear();
        return result;
    }
    for (HANDLE p : pipes_) threads_.emplace_back([this, p] { Serve(p); });
    return ERROR_SUCCESS;
}

void PipeServer::Stop() {
    if (stop_ != nullptr) SetEvent(stop_);
    for (auto& t : threads_) t.join();
    threads_.clear();
    for (HANDLE p : pipes_) CloseHandle(p);
    pipes_.clear();
    if (stop_ != nullptr) {
        CloseHandle(stop_);
        stop_ = nullptr;
    }
}

void PipeServer::Serve(HANDLE pipe) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<std::byte> buffer(proto::kMaxRequestBytes);
    while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
        ResetEvent(ov.hEvent);
        DWORD bytes = 0;
        if (!ConnectNamedPipe(pipe, &ov)) {
            const DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) {
                HANDLE events[2] = {ov.hEvent, stop_};
                const DWORD w = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                if (w != WAIT_OBJECT_0) {
                    CancelIoEx(pipe, &ov);
                    GetOverlappedResult(pipe, &ov, &bytes, TRUE);
                    break;
                }
                if (!GetOverlappedResult(pipe, &ov, &bytes, FALSE)) {
                    DisconnectNamedPipe(pipe);
                    continue;
                }
            } else if (e != ERROR_PIPE_CONNECTED) {
                log_(L"ConnectNamedPipe failed: Win32 error " + std::to_wstring(e));
                if (WaitForSingleObject(stop_, 1000) == WAIT_OBJECT_0) break;
                DisconnectNamedPipe(pipe);
                continue;
            }
        }
        // Requests until the client closes, sends something malformed, or is
        // silent for kRequestTimeoutMs.
        for (;;) {
            ResetEvent(ov.hEvent);
            DWORD read = 0;
            BOOL ok = ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &ov);
            DWORD e = ok ? ERROR_SUCCESS : GetLastError();
            if (!ok && e != ERROR_IO_PENDING) break;  // includes ERROR_MORE_DATA: request too large
            if (!WaitIo(pipe, ov, kRequestTimeoutMs, stop_, read)) break;
            const HandledRequest h = HandleRequest(buffer.data(), read, pipe, workers_, log_);
            ResetEvent(ov.hEvent);
            DWORD written = 0;
            ok = WriteFile(pipe, h.response.data(), static_cast<DWORD>(h.response.size()), nullptr, &ov);
            e = ok ? ERROR_SUCCESS : GetLastError();
            if (!ok && e != ERROR_IO_PENDING) break;
            if (!WaitIo(pipe, ov, kRequestTimeoutMs, stop_, written) || written != h.response.size()) break;
            if (h.closeAfter) break;
        }
        DisconnectNamedPipe(pipe);
    }
    CloseHandle(ov.hEvent);
}

}  // namespace dirsize::service
