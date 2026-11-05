#include "ui/tree_loader.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "core/path_util.h"
#include "ui/strings.h"

namespace dirsize::ui {

struct TreeLoader::State {
    HWND notify = nullptr;
    UINT listingMessage = 0;
    UINT drivesMessage = 0;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    bool drives = false;
    std::deque<std::wstring> queue;
    HANDLE done = nullptr;  // set when the worker exits
    ~State() {
        if (done) CloseHandle(done);
    }
};

namespace {

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

void Post(const TreeLoader::State& s, UINT message, void* result, void (*drop)(void*)) {
    if (!PostMessageW(s.notify, message, 0, reinterpret_cast<LPARAM>(result))) drop(result);
}

std::unique_ptr<TreeListing> ListFolder(const std::wstring& path) {
    auto r = std::make_unique<TreeListing>();
    r->path = path;
    std::wstring pattern = JoinPath(ToExtendedPath(path), L"*");
    WIN32_FIND_DATAW fd{};
    // FindExSearchLimitToDirectories is only advisory (the documentation),
    // so IsTreeFolder still checks every entry.
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        // The documentation does not name the error for "nothing matched".
        // A drive root has no "." entry, so an empty one can match nothing;
        // ERROR_FILE_NOT_FOUND is taken as "no subfolders" (an assumption,
        // not documented). Anything else is a failure.
        r->ok = error == ERROR_FILE_NOT_FOUND;
        r->error = error;
        return r;
    }
    do {
        if (IsTreeFolder(fd.dwFileAttributes, fd.dwReserved0, fd.cFileName)) r->names.emplace_back(fd.cFileName);
    } while (FindNextFileW(find, &fd));
    DWORD error = GetLastError();
    FindClose(find);
    r->ok = error == ERROR_NO_MORE_FILES;
    r->error = r->ok ? 0 : error;
    if (!r->ok) r->names.clear();
    return r;
}

std::wstring DriveText(const std::wstring& label, const std::wstring& root) {
    std::wstring letter = root.substr(0, 2);  // "C:"
    if (label.empty()) return letter;
    std::wstring text = Text(IDS_DRIVE_LABEL);  // "%1 (%2)"
    size_t p1 = text.find(L"%1");
    if (p1 != std::wstring::npos) text.replace(p1, 2, label);
    size_t p2 = text.find(L"%2");
    if (p2 != std::wstring::npos) text.replace(p2, 2, letter);
    return text;
}

void ListDrives(const TreeLoader::State& s) {
    std::vector<wchar_t> buffer(512);
    DWORD n = GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (n > buffer.size()) {
        buffer.resize(n + 1);
        n = GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size()), buffer.data());
    }
    if (n == 0 || n > buffer.size()) return;
    auto list = std::make_unique<DriveList>();
    for (const wchar_t* p = buffer.data(); *p; p += wcslen(p) + 1) {
        TreeRoot root;
        root.path = p;  // "C:\"
        root.label = DriveText(L"", root.path);
        list->roots.push_back(std::move(root));
    }
    // Letters first, so the tree is never empty while a label is slow.
    auto first = std::make_unique<DriveList>(*list);
    Post(s, s.drivesMessage, first.release(), [](void* p) { delete static_cast<DriveList*>(p); });
    bool changed = false;
    for (TreeRoot& root : list->roots) {
        UINT type = GetDriveTypeW(root.path.c_str());
        if (type == DRIVE_REMOTE || type == DRIVE_NO_ROOT_DIR) continue;
        wchar_t label[MAX_PATH + 1] = {};
        if (GetVolumeInformationW(root.path.c_str(), label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0) &&
            label[0]) {
            root.label = DriveText(label, root.path);
            changed = true;
        }
    }
    if (changed) Post(s, s.drivesMessage, list.release(), [](void* p) { delete static_cast<DriveList*>(p); });
}

void Worker(std::shared_ptr<TreeLoader::State> s) {
    // No "insert a disk" dialogs from this thread (empty card readers).
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, nullptr);
    for (;;) {
        std::wstring path;
        bool drives = false;
        {
            std::unique_lock lock(s->mutex);
            s->wake.wait(lock, [&] { return s->stop || s->drives || !s->queue.empty(); });
            if (s->stop) break;
            if (s->drives) {
                drives = true;
                s->drives = false;
            } else {
                path = std::move(s->queue.front());
                s->queue.pop_front();
            }
        }
        if (drives) {
            ListDrives(*s);
            continue;
        }
        auto r = ListFolder(path);
        Post(*s, s->listingMessage, r.release(), [](void* p) { delete static_cast<TreeListing*>(p); });
    }
    SetEvent(s->done);
}

}  // namespace

TreeLoader::TreeLoader(HWND notify, UINT listingMessage, UINT drivesMessage) : state_(std::make_shared<State>()) {
    state_->notify = notify;
    state_->listingMessage = listingMessage;
    state_->drivesMessage = drivesMessage;
    state_->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread(Worker, state_).detach();
}

TreeLoader::~TreeLoader() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stop = true;
        state_->queue.clear();
    }
    state_->wake.notify_all();
    // Bounded wait: a listing of a slow drive can take long. The thread
    // keeps the shared state alive, and posts to a destroyed window fail
    // (the result is then deleted), so leaving it is safe.
    WaitForSingleObject(state_->done, 1000);
}

void TreeLoader::List(const std::wstring& path) {
    {
        std::lock_guard lock(state_->mutex);
        for (const std::wstring& queued : state_->queue) {
            if (SamePath(queued, path)) return;
        }
        state_->queue.push_back(path);
    }
    state_->wake.notify_one();
}

void TreeLoader::ListDrives() {
    {
        std::lock_guard lock(state_->mutex);
        state_->drives = true;
    }
    state_->wake.notify_one();
}

}  // namespace dirsize::ui
