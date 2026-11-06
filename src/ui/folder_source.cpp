#include "ui/folder_source.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "client/pipe_client.h"
#include "core/dir_listing.h"
#include "core/dir_walker.h"
#include "core/path_util.h"

namespace dirsize::ui {

struct FolderSource::State {
    HWND notify = nullptr;
    UINT message = 0;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    bool pending = false;
    std::wstring path;
    bool includeFiles = false;
    std::uint64_t next = 0;      // last request number handed out
    std::uint64_t current = 0;   // request being loaded
    std::atomic<bool> cancel{false};
    HANDLE done = nullptr;       // set when the worker exits
    ~State() {
        if (done) CloseHandle(done);
    }
};

namespace {

enum class ServiceState { NotInstalled, Stopped, StoppedAutoStart, Starting, Running, Unknown };

// What the SCM says about the DirSize service (only query rights).
ServiceState QueryServiceState() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return ServiceState::Unknown;
    ServiceState result = ServiceState::Unknown;
    SC_HANDLE service = OpenServiceW(scm, L"DirSize", SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!service && GetLastError() == ERROR_ACCESS_DENIED) {
        service = OpenServiceW(scm, L"DirSize", SERVICE_QUERY_STATUS);
    }
    if (!service) {
        result = GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? ServiceState::NotInstalled : ServiceState::Unknown;
    } else {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status),
                                 &needed)) {
            switch (status.dwCurrentState) {
                case SERVICE_RUNNING: result = ServiceState::Running; break;
                case SERVICE_START_PENDING:
                case SERVICE_CONTINUE_PENDING: result = ServiceState::Starting; break;
                case SERVICE_STOPPED: {
                    result = ServiceState::Stopped;
                    // Stopped but set to start automatically: during the
                    // delayed start after boot (VM test: no pipe yet).
                    std::vector<BYTE> buffer(8192);
                    if (QueryServiceConfigW(service, reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data()),
                                            static_cast<DWORD>(buffer.size()), &needed) &&
                        reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data())->dwStartType == SERVICE_AUTO_START) {
                        result = ServiceState::StoppedAutoStart;
                    }
                    break;
                }
                default: result = ServiceState::Unknown; break;
            }
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
    return result;
}

ServiceNote NoteFor(ServiceState s) {
    switch (s) {
        case ServiceState::NotInstalled: return ServiceNote::NotInstalled;
        case ServiceState::Stopped: return ServiceNote::NotRunning;
        case ServiceState::StoppedAutoStart:
        case ServiceState::Starting:
        case ServiceState::Running: return ServiceNote::Starting;  // running but no pipe yet
        case ServiceState::Unknown: break;
    }
    return ServiceNote::NotRunning;
}

bool IsDrivePath(const std::wstring& p) {
    return p.size() >= 3 && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' &&
           p[2] == L'\\';
}

// Folders from the service. Returns false (with `note`) when the front end
// should walk instead; true when `r` is final (data or an error to show).
bool LoadFromService(const std::wstring& path, FolderResult& r, ServiceNote& note) {
    client::PipeClient pipe;
    client::ClientError error;
    if (!pipe.Connect(true, error)) {
        note = NoteFor(QueryServiceState());
        return false;
    }
    proto::QueryFolderRequest request;
    request.path = path;
    request.pageSize = proto::kMaxPageSize;
    for (;;) {
        proto::FolderResponse folder;
        proto::ErrorResponse serverError;
        if (!pipe.QueryFolder(request, folder, serverError, error)) {
            if (error.win32 == ERROR_SUCCESS) {
                // The service answered with an error code (PipeClient
                // reports a refusal with ERROR_SUCCESS).
                switch (serverError.code) {
                    case proto::ErrorCode::NotFound: r.status = LoadStatus::NotFound; return true;
                    case proto::ErrorCode::AccessDenied: r.status = LoadStatus::AccessDenied; return true;
                    case proto::ErrorCode::NotIndexed: note = ServiceNote::NotIndexed; return false;
                    case proto::ErrorCode::Busy: note = ServiceNote::Indexing; return false;
                    case proto::ErrorCode::PathRejected: note = ServiceNote::NotLocal; return false;
                    default: note = ServiceNote::NotRunning; return false;
                }
            }
            note = ServiceNote::NotRunning;  // the pipe broke: walk
            return false;
        }
        if (request.pageStart == 0) {
            r.totalLogical = folder.totals.logicalBytes;
            r.totalAllocated = folder.totals.allocatedBytes;
            r.totalFiles = folder.totals.files;
            if (folder.updating) r.note = ServiceNote::Updating;
        }
        for (const proto::ChildFolder& c : folder.children) {
            ListItem item;
            item.name = c.name;
            item.isFolder = true;
            item.accessible = c.accessible;
            item.logicalBytes = c.totals.logicalBytes;
            item.allocatedBytes = c.totals.allocatedBytes;
            item.files = c.totals.files;
            r.items.push_back(std::move(item));
        }
        request.pageStart += static_cast<std::uint32_t>(folder.children.size());
        if (folder.children.empty() || request.pageStart >= folder.childCount) break;
    }
    r.fromService = true;
    return true;
}

// Folders by walking. Runs in background thread mode (lower I/O and CPU
// scheduling priority, SetThreadPriority THREAD_MODE_BACKGROUND_BEGIN).
bool LoadByWalking(const std::wstring& path, FolderResult& r, const std::atomic<bool>& cancel) {
    const bool background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0;
    WalkResult walk = WalkTree(ToExtendedPath(path), &cancel);
    if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    if (walk.cancelled) return false;
    if (!walk.rootOpened) {
        r.status = walk.rootError == ERROR_ACCESS_DENIED ? LoadStatus::AccessDenied
                   : walk.rootError == ERROR_FILE_NOT_FOUND || walk.rootError == ERROR_PATH_NOT_FOUND
                       ? LoadStatus::NotFound
                       : LoadStatus::Failed;
        r.error = walk.rootError;
        return true;
    }
    r.totalLogical = walk.totals.logicalBytes;
    r.totalAllocated = walk.totals.allocatedBytes;
    r.totalFiles = walk.totals.files;
    for (const ChildFolder& c : walk.children) {
        ListItem item;
        item.name = c.name;
        item.isFolder = true;
        item.logicalBytes = c.totals.logicalBytes;
        item.allocatedBytes = c.totals.allocatedBytes;
        item.files = c.totals.files;
        r.items.push_back(std::move(item));
    }
    return true;
}

// Files directly in the folder, listed with the user's rights.
void AddFiles(const std::wstring& path, FolderResult& r) {
    HANDLE dir = OpenDirectoryForListing(ToExtendedPath(path).c_str());
    if (dir == INVALID_HANDLE_VALUE) return;
    DirectoryLister lister;
    lister.List(dir, [&](const ListedItem& entry) {
        if (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) return;
        ListItem item;
        item.name = std::wstring(entry.name);
        item.logicalBytes = entry.logicalBytes;
        item.allocatedBytes = entry.allocatedBytes;
        item.files = 1;
        r.items.push_back(std::move(item));
    });
    CloseHandle(dir);
}

void Worker(std::shared_ptr<FolderSource::State> s) {
    for (;;) {
        std::wstring path;
        bool includeFiles = false;
        std::uint64_t request = 0;
        {
            std::unique_lock lock(s->mutex);
            s->wake.wait(lock, [&] { return s->stop || s->pending; });
            if (s->stop) break;
            s->pending = false;
            s->cancel = false;
            path = s->path;
            includeFiles = s->includeFiles;
            request = s->current = s->next;
        }
        auto r = std::make_unique<FolderResult>();
        r->request = request;
        r->path = path;
        bool final = false;
        ServiceNote note = ServiceNote::None;
        if (IsDrivePath(path)) {
            final = LoadFromService(path, *r, note);
        } else {
            note = ServiceNote::NotLocal;
        }
        if (!final && !s->cancel) {
            // Drop anything a failed later page left behind.
            r->items.clear();
            r->totalLogical = r->totalAllocated = r->totalFiles = 0;
            r->status = LoadStatus::Ok;
            r->note = note;
            auto notice = std::make_unique<FolderResult>();
            notice->request = request;
            notice->path = path;
            notice->walking = true;
            notice->note = note;
            FolderResult* rawNotice = notice.release();
            if (!PostMessageW(s->notify, s->message, 0, reinterpret_cast<LPARAM>(rawNotice))) delete rawNotice;
            final = LoadByWalking(path, *r, s->cancel);
        }
        if (!final || s->cancel) continue;  // cancelled: a newer request is waiting
        if (includeFiles && r->status == LoadStatus::Ok) AddFiles(path, *r);
        FolderResult* raw = r.release();
        if (!PostMessageW(s->notify, s->message, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
    }
    SetEvent(s->done);
}

}  // namespace

FolderSource::FolderSource(HWND notify, UINT message) : state_(std::make_shared<State>()) {
    state_->notify = notify;
    state_->message = message;
    state_->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread(Worker, state_).detach();
}

FolderSource::~FolderSource() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stop = true;
        state_->cancel = true;
    }
    state_->wake.notify_all();
    // Bounded wait: a pipe call can take up to its 5 s timeout. The thread
    // keeps the shared state alive, so leaving it is safe.
    WaitForSingleObject(state_->done, 2000);
}

std::uint64_t FolderSource::Request(const std::wstring& folder, bool includeFiles) {
    std::uint64_t id;
    {
        std::lock_guard lock(state_->mutex);
        id = ++state_->next;
        state_->path = folder;
        state_->includeFiles = includeFiles;
        state_->pending = true;
        state_->cancel = true;  // stop the load in progress
    }
    state_->wake.notify_one();
    return id;
}

}  // namespace dirsize::ui
