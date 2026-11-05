#include "ui/explorer_tracker.h"

#include <ole2.h>  // WIN32_LEAN_AND_MEAN leaves out OLE, which exdisp.h needs
#include <exdisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "ui/panel_rules.h"

using Microsoft::WRL::ComPtr;

namespace dirsize::ui {

struct ExplorerTracker::State {
    HWND panel = nullptr;
    UINT message = 0;
    std::atomic<bool> stop{false};
    std::atomic<DWORD> threadId{0};
    HANDLE done = nullptr;
    ~State() {
        if (done) CloseHandle(done);
    }
};

namespace {

constexpr UINT kTickMs = 250;
constexpr UINT kNavigateMessage = WM_APP + 1;  // thread message, lParam = std::wstring*
constexpr unsigned kFullCheckEvery = 8;  // ticks: 2 s

HWND Root(HWND h) { return h ? GetAncestor(h, GA_ROOT) : nullptr; }

bool IsExplorerWindow(HWND h) {
    wchar_t name[64] = {};
    return h && GetClassNameW(h, name, 64) && wcscmp(name, L"CabinetWClass") == 0;
}

// Explorer went away (restart, crash): the proxies are dead.
bool IsDisconnected(HRESULT hr) {
    return hr == RPC_E_DISCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) ||
           hr == CO_E_OBJNOTCONNECTED;
}

bool IsFileSystemFolder(const std::wstring& path) {
    if (path.empty() || path[0] == L':') return false;  // "::{GUID}": This PC, Quick access, ...
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

struct ViewInfo {
    HWND browser = nullptr;  // IShellBrowser window (one per tab)
    HWND view = nullptr;     // IShellView window
    ComPtr<IShellBrowser> shellBrowser;
    ComPtr<IShellView> shellView;
    std::wstring folder;
};

struct Selection {
    bool ok = false;
    HRESULT hr = S_OK;
    std::uint32_t count = 0;
    std::wstring path;
};

Selection ReadSelection(IShellView* view) {
    Selection s;
    ComPtr<IFolderView2> fv;
    ComPtr<IShellItemArray> items;
    HRESULT hr = view->QueryInterface(IID_PPV_ARGS(&fv));
    if (SUCCEEDED(hr)) hr = fv->GetSelection(FALSE, &items);
    // Measured: with nothing selected it returns HRESULT_FROM_WIN32(
    // ERROR_NOT_FOUND), not the S_FALSE the documentation describes.
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) hr = S_FALSE;
    s.hr = hr;
    if (FAILED(hr)) return s;
    s.ok = true;
    DWORD count = 0;
    if (items) items->GetCount(&count);
    s.count = count;
    if (count == 1) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(0, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            s.path = path;
            CoTaskMemFree(path);
        }
    }
    return s;
}

// The views of top-level window `top`. Returns the failing HRESULT of the
// collection itself (S_OK otherwise); items that fail are skipped.
HRESULT ReadViews(IShellWindows* windows, HWND top, std::vector<ViewInfo>& out) {
    long count = 0;
    HRESULT hr = windows->get_Count(&count);
    if (FAILED(hr)) return hr;
    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> disp;
        hr = windows->Item(index, &disp);
        if (IsDisconnected(hr)) return hr;
        if (hr != S_OK || !disp) continue;
        ComPtr<IWebBrowser2> web;
        SHANDLE_PTR handle = 0;
        if (FAILED(disp.As(&web)) || FAILED(web->get_HWND(&handle)) || reinterpret_cast<HWND>(handle) != top) {
            continue;
        }
        ComPtr<IServiceProvider> provider;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> view;
        ComPtr<IFolderView> folderView;
        ComPtr<IPersistFolder2> persist;
        if (FAILED(web.As(&provider)) ||
            FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
            FAILED(browser->QueryActiveShellView(&view)) || !view || FAILED(view.As(&folderView)) ||
            FAILED(folderView->GetFolder(IID_PPV_ARGS(&persist)))) {
            continue;
        }
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(persist->GetCurFolder(&pidl)) || !pidl) continue;
        PWSTR name = nullptr;
        hr = SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &name);
        ILFree(pidl);
        if (FAILED(hr)) continue;
        ViewInfo v;
        v.folder = name;
        CoTaskMemFree(name);
        browser->GetWindow(&v.browser);
        view->GetWindow(&v.view);
        v.shellView = view;
        v.shellBrowser = browser;
        if (!v.view || !IsWindowVisible(v.view)) continue;
        out.push_back(std::move(v));
    }
    return S_OK;
}

// Windows 11 tabs, measured (not documented): every tab's view reports
// visible, and the tab in front is the one whose browser window comes first
// in its parent's child z-order. -1 when they do not share a parent.
int FrontmostTab(const std::vector<HWND>& browsers) {
    if (browsers.empty()) return -1;
    HWND parent = GetParent(browsers[0]);
    if (!parent) return -1;
    for (HWND b : browsers) {
        if (GetParent(b) != parent) return -1;
    }
    for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        auto it = std::find(browsers.begin(), browsers.end(), c);
        if (it != browsers.end()) return static_cast<int>(it - browsers.begin());
    }
    return -1;
}

class Tracker {
public:
    explicit Tracker(std::shared_ptr<ExplorerTracker::State> s) : s_(std::move(s)) {}

    void Tick() {
        HWND fg = Root(GetForegroundWindow());
        if (fg == s_->panel || !IsExplorerWindow(fg)) return;  // keep the state; no Explorer calls
        bool targetChanged = false;
        if (fg != target_) {
            target_ = fg;
            Forget();
            rules_.ViewLost();
            targetChanged = true;
        }
        if (!windows_ && FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows_)))) {
            windows_.Reset();
            return;
        }
        ++ticks_;
        const bool tabSwitched = tabs_.size() > 1 && [&] {
            int front = FrontmostTab(tabs_);
            return front < 0 || tabs_[front] != active_.browser;
        }();
        const bool full = !active_.shellView || !IsWindow(active_.view) || !IsWindowVisible(active_.view) ||
                          tabSwitched || ticks_ % kFullCheckEvery == 0;
        if (full) {
            FullCheck();
        } else {
            Selection sel = ReadSelection(active_.shellView.Get());
            if (sel.ok) {
                rules_.Selection(sel.count, sel.path, sel.count == 1 && IsFileSystemFolder(sel.path));
            } else if (IsDisconnected(sel.hr)) {
                Reset();
            }
        }
        if (rules_.TakeChanged() || targetChanged) Post();
    }

    void Navigate(const std::wstring& folder) {
        if (!active_.shellBrowser) return;
        // SHParseDisplayName: "call this function from a background thread"
        // (its documentation) -- this is one.
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(SHParseDisplayName(folder.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) return;
        HRESULT hr = active_.shellBrowser->BrowseObject(pidl, SBSP_SAMEBROWSER | SBSP_ABSOLUTE);
        ILFree(pidl);
        if (IsDisconnected(hr)) Reset();
        // The next tick sees the new view and the panel follows it.
    }

private:
    void FullCheck() {
        std::vector<ViewInfo> views;
        HRESULT hr = ReadViews(windows_.Get(), target_, views);
        if (FAILED(hr)) {
            Reset();  // Explorer restarted: create IShellWindows again next tick
            return;
        }
        const ViewInfo* active = nullptr;
        std::vector<HWND> browsers;
        for (const ViewInfo& v : views) browsers.push_back(v.browser);
        if (views.size() == 1) {
            active = &views[0];
        } else if (views.size() > 1) {
            int front = FrontmostTab(browsers);
            if (front >= 0) active = &views[front];
        }
        if (!active) {
            Forget();
            rules_.ViewLost();
            return;
        }
        tabs_ = browsers;
        if (active->view != active_.view || active->folder != active_.folder) {
            active_ = *active;
            Selection sel = ReadSelection(active_.shellView.Get());
            rules_.Navigated(active_.folder, IsFileSystemFolder(active_.folder),
                             sel.ok ? SelectionKey(sel.count, sel.path) : std::wstring(L"?"));
        }
    }

    void Forget() {
        active_ = ViewInfo{};
        tabs_.clear();
    }

    void Reset() {
        Forget();
        windows_.Reset();
        rules_.ViewLost();
    }

    void Post() {
        auto* u = new ExplorerUpdate{target_, rules_.Shown()};
        if (!PostMessageW(s_->panel, s_->message, 0, reinterpret_cast<LPARAM>(u))) delete u;
    }

    std::shared_ptr<ExplorerTracker::State> s_;
    ComPtr<IShellWindows> windows_;
    HWND target_ = nullptr;
    ViewInfo active_;
    std::vector<HWND> tabs_;
    PanelRules rules_;
    unsigned ticks_ = 0;
};

void Worker(std::shared_ptr<ExplorerTracker::State> s) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the message queue
    s->threadId = GetCurrentThreadId();
    if (com) {
        Tracker tracker(s);
        UINT_PTR timer = SetTimer(nullptr, 0, kTickMs, nullptr);
        while (!s->stop && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_TIMER && msg.wParam == timer) {
                tracker.Tick();
                continue;
            }
            if (msg.message == kNavigateMessage) {
                std::unique_ptr<std::wstring> folder(reinterpret_cast<std::wstring*>(msg.lParam));
                tracker.Navigate(*folder);
                continue;
            }
            DispatchMessageW(&msg);
        }
        KillTimer(nullptr, timer);
    }
    if (com) CoUninitialize();
    SetEvent(s->done);
}

}  // namespace

ExplorerTracker::ExplorerTracker(HWND panel, UINT message) : state_(std::make_shared<State>()) {
    state_->panel = panel;
    state_->message = message;
    state_->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread(Worker, state_).detach();
}

void ExplorerTracker::Navigate(const std::wstring& folder) {
    DWORD id = state_->threadId;
    if (!id) return;
    auto* copy = new std::wstring(folder);
    if (!PostThreadMessageW(id, kNavigateMessage, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
}

ExplorerTracker::~ExplorerTracker() {
    state_->stop = true;
    if (DWORD id = state_->threadId) PostThreadMessageW(id, WM_QUIT, 0, 0);
    WaitForSingleObject(state_->done, 2000);
}

}  // namespace dirsize::ui
