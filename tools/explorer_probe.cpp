// Phase 5 feasibility probe (measurement only, not shipped). See
// docs/DESIGN-phase5.md section 8. No elevation needed.
//
//   explorer-probe windows
//     Lists every IShellWindows item: top-level window, the IShellBrowser
//     window, the shell view window (class, visible or not) and the current
//     folder. Run it with several Explorer windows and tabs open.
//   explorer-probe events [seconds]
//     Logs window events (create, destroy, show, hide, name change) of
//     Explorer windows, and the folders each window reports after the event.
//     Shows whether navigating or switching tabs produces any event.
//   explorer-probe dock owned|events [--subscribe] [--no-hooks] [--seconds N]
//                       [--hang-after S --hang-for S]
//     A small panel beside the Explorer window that is in front, showing the
//     subfolders of the folder the user navigated to or selected (rules in
//     the dock section below). "owned": the panel is created as an owned
//     window of that Explorer window (owner in another process). "events":
//     not owned; follows it with SetWinEventHook. --no-hooks: no WinEvent
//     hooks at all. --subscribe: also subscribe to SelectionChanged (events
//     ignored). --hang makes the panel's thread stop responding for a while,
//     to see whether Explorer is affected; the three options tell which part
//     makes Explorer wait.
//   explorer-probe verb install|remove
//     Adds or removes a classic registry verb under HKCU (Directory and
//     Directory\Background), to see where it appears in the context menu.

#include <windows.h>
#include <ole2.h>  // WIN32_LEAN_AND_MEAN leaves out OLE, which exdisp.h needs
#include <dwmapi.h>
#include <exdisp.h>
#include <shdispid.h>
#include <shldisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

using Clock = std::chrono::steady_clock;

double Since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

void Write(std::wstring_view text) {
    if (text.empty()) return;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                    nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr,
                        nullptr);
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
    std::fflush(stdout);
}

std::wstring Format(const wchar_t* format, ...) {
    wchar_t buffer[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

void Print(const wchar_t* format, ...) {
    wchar_t buffer[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    Write(buffer);
}

std::uint64_t FileTimeTo100ns(const FILETIME& t) {
    return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

double ProcessCpuSeconds(HANDLE process) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(process, &c, &e, &k, &u)) return -1;
    return (FileTimeTo100ns(k) + FileTimeTo100ns(u)) / 1e7;
}

std::wstring ClassOf(HWND h) {
    if (!h) return L"-";
    wchar_t name[256] = {};
    GetClassNameW(h, name, 256);
    return name;
}

std::wstring TitleOf(HWND h) {
    wchar_t text[512] = {};
    GetWindowTextW(h, text, 512);
    return text;
}

HWND Root(HWND h) { return h ? GetAncestor(h, GA_ROOT) : nullptr; }

// ---- reading Explorer windows ----------------------------------------------------------------

struct ShellWin {
    HWND top = nullptr;      // IWebBrowser2::get_HWND
    HWND browser = nullptr;  // IShellBrowser (IOleWindow::GetWindow)
    HWND view = nullptr;     // IShellView (IOleWindow::GetWindow)
    std::wstring folder;
    const wchar_t* failedStep = nullptr;
    HRESULT hr = S_OK;
    const wchar_t* service = L"";  // which QueryService id gave the IShellBrowser
    ComPtr<IShellView> shellView;
};

bool Fail(ShellWin& w, const wchar_t* step, HRESULT hr) {
    w.failedStep = step;
    w.hr = hr;
    return false;
}

bool ReadOne(IDispatch* disp, ShellWin& w) {
    ComPtr<IWebBrowser2> web;
    HRESULT hr = disp->QueryInterface(IID_PPV_ARGS(&web));
    if (FAILED(hr)) return Fail(w, L"QueryInterface(IWebBrowser2)", hr);
    SHANDLE_PTR handle = 0;
    hr = web->get_HWND(&handle);
    if (FAILED(hr)) return Fail(w, L"get_HWND", hr);
    w.top = reinterpret_cast<HWND>(handle);

    ComPtr<IServiceProvider> provider;
    hr = web.As(&provider);
    if (FAILED(hr)) return Fail(w, L"QueryInterface(IServiceProvider)", hr);
    ComPtr<IShellBrowser> browser;
    hr = provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
    w.service = L"SID_STopLevelBrowser";
    if (FAILED(hr)) {
        hr = provider->QueryService(SID_SShellBrowser, IID_PPV_ARGS(&browser));
        w.service = L"SID_SShellBrowser";
    }
    if (FAILED(hr)) return Fail(w, L"QueryService(IShellBrowser)", hr);
    browser->GetWindow(&w.browser);

    ComPtr<IShellView> view;
    hr = browser->QueryActiveShellView(&view);
    if (FAILED(hr) || !view) return Fail(w, L"QueryActiveShellView", hr);
    view->GetWindow(&w.view);
    w.shellView = view;
    ComPtr<IFolderView> folderView;
    hr = view.As(&folderView);
    if (FAILED(hr)) return Fail(w, L"QueryInterface(IFolderView)", hr);
    ComPtr<IPersistFolder2> persist;
    hr = folderView->GetFolder(IID_PPV_ARGS(&persist));
    if (FAILED(hr)) return Fail(w, L"IFolderView::GetFolder", hr);
    PIDLIST_ABSOLUTE pidl = nullptr;
    hr = persist->GetCurFolder(&pidl);
    if (FAILED(hr) || !pidl) return Fail(w, L"GetCurFolder", hr);
    PWSTR name = nullptr;
    hr = SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &name);
    ILFree(pidl);
    if (FAILED(hr)) return Fail(w, L"SHGetNameFromIDList", hr);
    w.folder = name;
    CoTaskMemFree(name);
    return true;
}

// onlyTop: when set, items whose top-level window is a different window are
// skipped after get_HWND (cheaper than reading their folder).
std::vector<ShellWin> ReadAll(IShellWindows* windows, HWND onlyTop = nullptr) {
    std::vector<ShellWin> result;
    long count = 0;
    if (FAILED(windows->get_Count(&count))) return result;
    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> disp;
        if (windows->Item(index, &disp) != S_OK || !disp) continue;
        if (onlyTop) {
            ComPtr<IWebBrowser2> web;
            SHANDLE_PTR handle = 0;
            if (FAILED(disp.As(&web)) || FAILED(web->get_HWND(&handle)) ||
                reinterpret_cast<HWND>(handle) != onlyTop) {
                continue;
            }
        }
        ShellWin w;
        ReadOne(disp.Get(), w);
        result.push_back(std::move(w));
    }
    return result;
}

ComPtr<IShellWindows> OpenShellWindows() {
    ComPtr<IShellWindows> windows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows));
    if (FAILED(hr)) Print(L"CoCreateInstance(CLSID_ShellWindows) failed: 0x%08lx\n", hr);
    return windows;
}

std::wstring Describe(const ShellWin& w) {
    std::wstring s = Format(L"top 0x%llx %s | browser 0x%llx %s visible %d | view 0x%llx %s visible %d",
                            reinterpret_cast<std::uint64_t>(w.top), ClassOf(w.top).c_str(),
                            reinterpret_cast<std::uint64_t>(w.browser), ClassOf(w.browser).c_str(),
                            w.browser ? IsWindowVisible(w.browser) : 0, reinterpret_cast<std::uint64_t>(w.view),
                            ClassOf(w.view).c_str(), w.view ? IsWindowVisible(w.view) : 0);
    if (w.failedStep) {
        s += Format(L"\n      FAILED at %s: 0x%08lx", w.failedStep, w.hr);
    } else {
        s += Format(L"\n      folder: %s", w.folder.c_str());
    }
    return s;
}

// The folder of the top-level window `top`: the only item for that window,
// or, if several items share it (tabs?), the only one whose view window is
// visible. Otherwise unknown. Raw counts are returned so the output can be
// checked against what is on the screen.
struct Pick {
    int candidates = 0;
    int visible = 0;
    bool known = false;
    std::wstring folder;
};

// Windows 11 tabs, measured on build 26300 (not documented): every tab of a
// window reports its view window as visible, and the tab in front is the one
// whose IShellBrowser window comes first in its parent's child z-order (3 of
// 3 tab switches). Returns the index into `browsers`, or -1 when they do not
// share one parent or none is found.
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

Pick PickFolder(const std::vector<ShellWin>& all, HWND top) {
    Pick p;
    const ShellWin* single = nullptr;
    const ShellWin* shown = nullptr;
    for (const ShellWin& w : all) {
        if (w.top != top || w.failedStep) continue;
        ++p.candidates;
        single = &w;
        if (w.view && IsWindowVisible(w.view)) {
            ++p.visible;
            shown = &w;
        }
    }
    if (p.candidates == 1) {
        p.known = true;
        p.folder = single->folder;
    } else if (p.candidates > 1 && p.visible == 1) {
        p.known = true;
        p.folder = shown->folder;
    } else if (p.visible > 1) {
        std::vector<const ShellWin*> tabs;
        std::vector<HWND> browsers;
        for (const ShellWin& w : all) {
            if (w.top != top || w.failedStep || !w.view || !IsWindowVisible(w.view)) continue;
            tabs.push_back(&w);
            browsers.push_back(w.browser);
        }
        int front = FrontmostTab(browsers);
        if (front >= 0) {
            p.known = true;
            p.folder = tabs[front]->folder + L"  (front tab by z-order)";
        }
    }
    return p;
}

bool IsExplorerTop(HWND h) { return h && ClassOf(h) == L"CabinetWClass"; }

// ---- windows -----------------------------------------------------------------------------------

// Accepts any event and ignores it; only used to see whether Advise works.
class IgnoreSink final : public IDispatch {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == DIID_DShellFolderViewEvents) {
            *ppv = static_cast<IDispatch*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* n) override {
        *n = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
        return S_OK;
    }
} g_ignoreSink;

// The selection count, and whether SelectionChanged can be subscribed to.
std::wstring CheckSelectionApis(IShellView* view) {
    std::wstring s;
    ComPtr<IFolderView2> fv;
    ComPtr<IShellItemArray> items;
    DWORD count = 0;
    HRESULT hr = view->QueryInterface(IID_PPV_ARGS(&fv));
    if (SUCCEEDED(hr)) hr = fv->GetSelection(FALSE, &items);
    if (items) items->GetCount(&count);
    s = Format(L"GetSelection 0x%08lx, %lu selected", hr, count);
    ComPtr<IDispatch> folderView;
    ComPtr<IConnectionPointContainer> container;
    ComPtr<IConnectionPoint> point;
    DWORD cookie = 0;
    hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&folderView));
    const wchar_t* step = L"GetItemObject";
    if (SUCCEEDED(hr)) {
        hr = folderView.As(&container);
        step = L"IConnectionPointContainer";
    }
    if (SUCCEEDED(hr)) {
        hr = container->FindConnectionPoint(DIID_DShellFolderViewEvents, &point);
        step = L"FindConnectionPoint";
    }
    if (SUCCEEDED(hr)) {
        hr = point->Advise(&g_ignoreSink, &cookie);
        step = L"Advise";
    }
    if (SUCCEEDED(hr)) {
        point->Unadvise(cookie);
        s += L"; SelectionChanged subscription: ok";
    } else {
        s += Format(L"; SelectionChanged subscription FAILED at %s: 0x%08lx", step, hr);
    }
    return s;
}

int RunWindows() {
    ComPtr<IShellWindows> windows = OpenShellWindows();
    if (!windows) return 1;
    long count = 0;
    windows->get_Count(&count);
    Print(L"IShellWindows count: %ld\n", count);
    Clock::time_point t0 = Clock::now();
    std::vector<ShellWin> all = ReadAll(windows.Get());
    double took = Since(t0);
    std::set<HWND> tops;
    for (size_t i = 0; i < all.size(); ++i) {
        Print(L"[%zu] %s\n", i, Describe(all[i]).c_str());
        Print(L"      via %s\n", all[i].service);
        if (all[i].shellView) Print(L"      %s\n", CheckSelectionApis(all[i].shellView.Get()).c_str());
        tops.insert(all[i].top);
    }
    Print(L"read %zu items in %.1f ms\n", all.size(), took * 1000);
    // Tab diagnostics: properties that might tell the tab in front apart.
    for (size_t i = 0; i < all.size(); ++i) {
        const ShellWin& w = all[i];
        if (!w.browser) continue;
        RECT br{}, vr{};
        GetWindowRect(w.browser, &br);
        if (w.view) GetWindowRect(w.view, &vr);
        int z = 0;  // position among the top-level window's children, 0 = topmost
        for (HWND c = GetWindow(GetParent(w.browser), GW_CHILD); c && c != w.browser; c = GetWindow(c, GW_HWNDNEXT)) ++z;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(w.browser, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        Print(L"  tab [%zu] browser rect %ld,%ld-%ld,%ld view rect %ld,%ld-%ld,%ld z %d enabled %d style 0x%08lx "
              L"cloaked %d parent 0x%llx\n",
              i, br.left, br.top, br.right, br.bottom, vr.left, vr.top, vr.right, vr.bottom, z,
              IsWindowEnabled(w.browser), static_cast<unsigned long>(GetWindowLongPtrW(w.browser, GWL_STYLE)),
              cloaked, reinterpret_cast<std::uint64_t>(GetParent(w.browser)));
        DWORD tid = GetWindowThreadProcessId(w.top, nullptr);
        GUITHREADINFO gti{sizeof(gti)};
        if (GetGUIThreadInfo(tid, &gti) && gti.hwndFocus) {
            bool inside = IsChild(w.browser, gti.hwndFocus) || gti.hwndFocus == w.browser;
            Print(L"      focus of Explorer's thread: 0x%llx %s, inside this tab: %d\n",
                  reinterpret_cast<std::uint64_t>(gti.hwndFocus), ClassOf(gti.hwndFocus).c_str(), inside);
        }
    }
    HWND fg = Root(GetForegroundWindow());
    Print(L"foreground: 0x%llx %s\n", reinterpret_cast<std::uint64_t>(fg), ClassOf(fg).c_str());
    for (HWND top : tops) {
        Pick p = PickFolder(all, top);
        Print(L"window 0x%llx \"%s\": items %d, view visible %d -> %s\n", reinterpret_cast<std::uint64_t>(top),
              TitleOf(top).c_str(), p.candidates, p.visible, p.known ? p.folder.c_str() : L"(cannot tell)");
    }
    return 0;
}

// ---- events ------------------------------------------------------------------------------------

struct QueuedEvent {
    double at;
    DWORD event;
    HWND hwnd;
};

struct EventsState {
    Clock::time_point start;
    std::deque<QueuedEvent> queue;
    DWORD thread = 0;
    std::map<DWORD, unsigned> counts;
    bool draining = false;
} g_events;

const wchar_t* EventName(DWORD e) {
    switch (e) {
        case EVENT_OBJECT_CREATE: return L"CREATE";
        case EVENT_OBJECT_DESTROY: return L"DESTROY";
        case EVENT_OBJECT_SHOW: return L"SHOW";
        case EVENT_OBJECT_HIDE: return L"HIDE";
        case EVENT_OBJECT_NAMECHANGE: return L"NAMECHANGE";
        case EVENT_SYSTEM_FOREGROUND: return L"FOREGROUND";
        default: return L"?";
    }
}

void CALLBACK OnEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    // DESTROY is skipped: the window may already be gone, so its root
    // cannot be checked.
    if (event == EVENT_OBJECT_DESTROY || !IsExplorerTop(Root(hwnd))) return;
    g_events.queue.push_back({Since(g_events.start), event, hwnd});
    PostThreadMessageW(g_events.thread, WM_APP, 0, 0);
}

void DrainEvents(IShellWindows* windows) {
    if (g_events.draining) return;  // COM calls below pump messages
    g_events.draining = true;
    while (!g_events.queue.empty()) {
        QueuedEvent e = g_events.queue.front();
        g_events.queue.pop_front();
        HWND root = Root(e.hwnd);
        std::wstring cls = ClassOf(e.hwnd);
        ++g_events.counts[e.event];
        std::wstring extra;
        if (e.event == EVENT_OBJECT_NAMECHANGE) extra = L" title \"" + TitleOf(e.hwnd) + L"\"";
        Print(L"[%7.2f s] %-10s 0x%llx %s (root 0x%llx)%s\n", e.at, EventName(e.event),
              reinterpret_cast<std::uint64_t>(e.hwnd), cls.c_str(), reinterpret_cast<std::uint64_t>(root),
              extra.c_str());
        // Folder list of that window only for events of the top-level
        // window or of the tab / view windows, not every child control.
        if (e.hwnd == root || cls == L"ShellTabWindowClass" || cls == L"SHELLDLL_DefView") {
            for (const ShellWin& w : ReadAll(windows, root)) {
                Print(L"             view 0x%llx visible %d: %s\n", reinterpret_cast<std::uint64_t>(w.view),
                      w.view ? IsWindowVisible(w.view) : 0, w.failedStep ? L"(failed)" : w.folder.c_str());
            }
        }
    }
    g_events.draining = false;
}

int RunEvents(unsigned seconds) {
    ComPtr<IShellWindows> windows = OpenShellWindows();
    if (!windows) return 1;
    g_events.start = Clock::now();
    g_events.thread = GetCurrentThreadId();
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    HWINEVENTHOOK h1 = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr, OnEvent, 0, 0, flags);
    HWINEVENTHOOK h2 =
        SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, OnEvent, 0, 0, flags);
    HWINEVENTHOOK h3 =
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnEvent, 0, 0, flags);
    if (!h1 || !h2 || !h3) {
        Print(L"SetWinEventHook failed: %lu\n", GetLastError());
        return 1;
    }
    Print(L"Logging Explorer window events for %u s. Navigate folders, switch tabs, open and close tabs,\n"
          L"and write down what you did at which second.\n",
          seconds);
    UINT_PTR timer = SetTimer(nullptr, 0, seconds * 1000, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.wParam == timer) break;
        if (msg.message == WM_APP) {
            DrainEvents(windows.Get());
            continue;
        }
        DispatchMessageW(&msg);
    }
    UnhookWinEvent(h1);
    UnhookWinEvent(h2);
    UnhookWinEvent(h3);
    DrainEvents(windows.Get());  // a posted WM_APP can be consumed by a COM wait
    Print(L"\ncounts:");
    for (auto& [event, n] : g_events.counts) Print(L" %s %u", EventName(event), n);
    Print(L"\n");
    return 0;
}

// ---- dock --------------------------------------------------------------------------------------
//
// The panel beside the Explorer window that is in front shows the
// subfolders of (rules confirmed by the user, docs/DESIGN-phase5.md 7.5):
//   - after a navigation (navigation pane, double-click, Back): the current
//     folder; hidden if it is not a file-system folder;
//   - when the selection in the view changes to exactly one folder: that
//     folder;
//   - when it changes to anything else (a file, nothing, several): hidden.
// The selection is read every 250 ms while Explorer is in front
// (IFolderView2::GetSelection). The VM test showed that the SelectionChanged
// event hardly ever arrives on Windows 10, so it is not used; --subscribe
// still subscribes to it (events are ignored), only to see whether the
// subscription is what makes Explorer wait when the panel hangs.
// The active view is cached: the full IShellWindows walk runs only when the
// cached view window is gone or hidden, and every 2 s as a safety net.

struct DockState {
    bool owned = false;
    bool subscribe = false;
    bool hooks = true;
    unsigned hangAfter = 0, hangFor = 0;
    HWND target = nullptr;
    DWORD targetPid = 0;
    HWND panel = nullptr;
    HWINEVENTHOOK locationHook = nullptr;
    IShellWindows* windows = nullptr;
    Clock::time_point start;

    // The cached active view, its folder and the selection last seen in it.
    HWND viewHwnd = nullptr;
    ComPtr<IShellView> view;
    std::wstring viewFolder;
    std::wstring selectionKey;
    ComPtr<IConnectionPoint> point;
    DWORD cookie = 0;
    unsigned subscribes = 0, subscribeFailures = 0;

    std::wstring shown;      // folder shown in the panel, empty = panel hidden
    std::wstring lastState;  // for logging changes only
    std::vector<std::wstring> lines;
    std::vector<HWND> tabBrowsers;  // IShellBrowser windows of the visible tabs, last full check
    HWND browserHwnd = nullptr;     // the front tab's
    unsigned tabSwitches = 0;
    std::wstring placement;
    bool inside = false;
    unsigned raises = 0;  // ticks that found Explorer above the inside panel
    bool busy = false;
    unsigned ticks = 0, fullChecks = 0, quickChecks = 0, navigations = 0;
    unsigned locationEvents = 0, placements = 0;
    double sumFullMs = 0, maxFullMs = 0, sumQuickMs = 0, maxQuickMs = 0;
} g_dock;

constexpr UINT_PTR kViewTimer = 1;
constexpr UINT_PTR kHangTimer = 2;
constexpr UINT_PTR kEndTimer = 3;
constexpr unsigned kViewMs = 250;
constexpr unsigned kFullCheckEvery = 8;  // ticks: 2 s

bool VisibleFrame(HWND h, RECT* r) {
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof(*r)));
}

void PlacePanel(bool bringAboveTarget) {
    if (!g_dock.panel || !IsWindow(g_dock.target) || !IsWindowVisible(g_dock.panel)) return;
    if (IsIconic(g_dock.target)) return;
    RECT frame;
    if (!VisibleFrame(g_dock.target, &frame)) return;
    // The panel's own invisible borders, so that visible edges line up.
    RECT outer{}, inner{};
    GetWindowRect(g_dock.panel, &outer);
    int l = 0, t = 0, r = 0, b = 0;
    if (VisibleFrame(g_dock.panel, &inner)) {
        l = inner.left - outer.left;
        t = inner.top - outer.top;
        r = outer.right - inner.right;
        b = outer.bottom - inner.bottom;
    }
    const int width = MulDiv(320, GetDpiForWindow(g_dock.target), 96);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g_dock.target, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    // Order confirmed by the user: outside right, outside left, otherwise
    // inside on the right edge, always within the monitor's work area.
    int x = 0;
    const wchar_t* placement = nullptr;
    if (frame.right + width <= work.right) {
        x = frame.right;
        placement = L"outside, right";
    } else if (frame.left - width >= work.left) {
        x = frame.left - width;
        placement = L"outside, left";
    } else {
        x = std::min(frame.right, static_cast<LONG>(work.right)) - width;
        x = std::max(x, static_cast<int>(work.left));
        placement = L"inside, right";
    }
    g_dock.inside = placement[0] == L'i';
    const int top = std::max(frame.top, work.top);
    const int bottom = std::min(frame.bottom, work.bottom);
    if (g_dock.placement != placement) {
        g_dock.placement = placement;
        Print(L"[%7.2f s] placement: %s\n", Since(g_dock.start), placement);
    }
    // Inside, the panel overlaps Explorer, so it must stay above it.
    if (g_dock.inside) bringAboveTarget = true;
    HWND after = nullptr;
    UINT flags = SWP_NOACTIVATE;
    if (g_dock.owned || !bringAboveTarget) {
        flags |= SWP_NOZORDER;
    } else {
        // Directly above the target: below the window that is above it.
        after = GetWindow(g_dock.target, GW_HWNDPREV);
        if (after == g_dock.panel) {
            flags |= SWP_NOZORDER;
        } else if (!after) {
            after = HWND_TOP;
        }
    }
    SetWindowPos(g_dock.panel, after, x - l, top - t, width + l + r, (bottom - top) + t + b, flags);
    ++g_dock.placements;
}

void CALLBACK OnLocation(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (hwnd != g_dock.target || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    ++g_dock.locationEvents;
    PlacePanel(false);
}

void HookTargetLocation() {
    if (!g_dock.hooks) return;
    if (g_dock.locationHook) UnhookWinEvent(g_dock.locationHook);
    GetWindowThreadProcessId(g_dock.target, &g_dock.targetPid);
    // Only the target's process: location changes of the whole system
    // (carets, cursor) would wake this thread all the time.
    g_dock.locationHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                                          OnLocation, g_dock.targetPid, 0, WINEVENT_OUTOFCONTEXT);
}

void ForgetView() {
    if (g_dock.point && g_dock.cookie) g_dock.point->Unadvise(g_dock.cookie);
    g_dock.point.Reset();
    g_dock.cookie = 0;
    g_dock.view.Reset();
    g_dock.viewHwnd = nullptr;
    g_dock.viewFolder.clear();
    g_dock.selectionKey.clear();
}

// --subscribe only: the events are ignored (g_ignoreSink).
void SubscribeIgnored(IShellView* view) {
    ++g_dock.subscribes;
    ComPtr<IDispatch> folderView;
    ComPtr<IConnectionPointContainer> container;
    HRESULT hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&folderView));
    if (SUCCEEDED(hr)) hr = folderView.As(&container);
    if (SUCCEEDED(hr)) hr = container->FindConnectionPoint(DIID_DShellFolderViewEvents, &g_dock.point);
    if (SUCCEEDED(hr)) hr = g_dock.point->Advise(&g_ignoreSink, &g_dock.cookie);
    if (FAILED(hr)) {
        ++g_dock.subscribeFailures;
        g_dock.point.Reset();
        g_dock.cookie = 0;
    }
}

// Subfolder names of `folder` (display only; sizes come from the service
// in the real front end).
std::vector<std::wstring> ListSubfolders(const std::wstring& folder, double* ms) {
    Clock::time_point t = Clock::now();
    std::vector<std::wstring> names;
    std::wstring pattern = folder;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            names.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    *ms = Since(t) * 1000;
    return names;
}

bool IsFileSystemFolder(const std::wstring& path) {
    if (path.empty() || path[0] == L':') return false;  // "::{GUID}": This PC, Quick access, ...
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Shows `folder` in the panel, or hides the panel when it is empty.
void Show(const std::wstring& state, const std::wstring& folder) {
    if (state != g_dock.lastState) {
        g_dock.lastState = state;
        Print(L"[%7.2f s] %s\n", Since(g_dock.start), state.c_str());
    }
    if (folder == g_dock.shown) return;
    g_dock.shown = folder;
    if (folder.empty()) {
        ShowWindow(g_dock.panel, SW_HIDE);
        return;
    }
    double listMs = 0;
    std::vector<std::wstring> names = ListSubfolders(folder, &listMs);
    g_dock.lines.clear();
    g_dock.lines.push_back(folder);
    g_dock.lines.push_back(Format(L"%zu subfolders (listed in %.1f ms)", names.size(), listMs));
    g_dock.lines.push_back(L"");
    for (size_t i = 0; i < names.size() && i < 30; ++i) g_dock.lines.push_back(names[i]);
    if (names.size() > 30) g_dock.lines.push_back(L"...");
    ShowWindow(g_dock.panel, SW_SHOWNOACTIVATE);
    PlacePanel(true);
    InvalidateRect(g_dock.panel, nullptr, TRUE);
}

struct Selection {
    bool ok = false;
    DWORD count = 0;
    std::wstring path;  // when count == 1 and it is a file-system item
    std::wstring Key() const { return ok ? Format(L"%lu|", count) + path : L"?"; }
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
    if (FAILED(hr)) return s;
    s.ok = true;
    if (items) items->GetCount(&s.count);
    if (s.count == 1) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(0, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            s.path = path;
            CoTaskMemFree(path);
        }
    }
    return s;
}

// A new active view (navigation or tab switch): show its current folder.
// The selection it starts with (e.g. the folder Explorer selects again after
// Back) becomes the baseline, so only later changes count as a click.
void OnNewView(const ShellWin& w) {
    ForgetView();
    ++g_dock.navigations;
    g_dock.view = w.shellView;
    g_dock.viewHwnd = w.view;
    g_dock.viewFolder = w.folder;
    g_dock.selectionKey = ReadSelection(w.shellView.Get()).Key();
    if (g_dock.subscribe) SubscribeIgnored(w.shellView.Get());
    if (IsFileSystemFolder(w.folder)) {
        Show(L"navigated: current folder " + w.folder, w.folder);
    } else {
        Show(L"navigated: not a file-system folder (" + w.folder + L") -> hide", L"");
    }
}

// Every 250 ms while the target is in front.
void Tick() {
    if (g_dock.busy) return;  // COM calls pump messages; no nested ticks
    HWND fg = Root(GetForegroundWindow());
    if (fg != g_dock.target) return;  // also skipped while the panel itself is in front
    // Inside placement overlaps Explorer: if Explorer got above the panel
    // (e.g. it was activated), put the panel back on top of it.
    if (!g_dock.owned && g_dock.inside && IsWindowVisible(g_dock.panel) &&
        GetWindow(g_dock.target, GW_HWNDPREV) != g_dock.panel) {
        ++g_dock.raises;
        PlacePanel(true);
    }
    g_dock.busy = true;
    ++g_dock.ticks;
    Clock::time_point t = Clock::now();
    // A tab switch keeps the cached view visible, so also check (without COM)
    // that the cached tab is still the front one.
    bool tabSwitched = g_dock.tabBrowsers.size() > 1 &&
                       [] {
                           int front = FrontmostTab(g_dock.tabBrowsers);
                           return front < 0 || g_dock.tabBrowsers[front] != g_dock.browserHwnd;
                       }();
    bool full = !g_dock.view || !IsWindow(g_dock.viewHwnd) || !IsWindowVisible(g_dock.viewHwnd) || tabSwitched ||
                g_dock.ticks % kFullCheckEvery == 0;
    if (full) {
        std::vector<ShellWin> all = ReadAll(g_dock.windows, g_dock.target);
        std::vector<const ShellWin*> visible;
        std::vector<HWND> browsers;
        for (const ShellWin& w : all) {
            if (w.failedStep || !w.view || !IsWindowVisible(w.view)) continue;
            visible.push_back(&w);
            browsers.push_back(w.browser);
        }
        const ShellWin* active = nullptr;
        if (visible.size() == 1) {
            active = visible[0];
        } else if (visible.size() > 1) {
            int front = FrontmostTab(browsers);
            if (front >= 0) active = visible[front];
        }
        double ms = Since(t) * 1000;
        ++g_dock.fullChecks;
        g_dock.sumFullMs += ms;
        g_dock.maxFullMs = std::max(g_dock.maxFullMs, ms);
        if (!active) {
            ForgetView();
            g_dock.tabBrowsers.clear();
            Show(Format(L"cannot tell the active view (%zu items, %zu visible) -> hide", all.size(), visible.size()),
                 L"");
            g_dock.busy = false;
            return;
        }
        g_dock.tabBrowsers = browsers;
        if (active->browser != g_dock.browserHwnd && g_dock.browserHwnd) {
            ++g_dock.tabSwitches;
            Print(L"[%7.2f s] front tab: %s (%zu tabs)\n", Since(g_dock.start), active->folder.c_str(),
                  visible.size());
        }
        g_dock.browserHwnd = active->browser;
        if (active->view != g_dock.viewHwnd || active->folder != g_dock.viewFolder) {
            OnNewView(*active);
            g_dock.busy = false;
            return;
        }
    }
    Clock::time_point q = Clock::now();
    Selection s = ReadSelection(g_dock.view.Get());
    double qms = Since(q) * 1000;
    ++g_dock.quickChecks;
    g_dock.sumQuickMs += qms;
    g_dock.maxQuickMs = std::max(g_dock.maxQuickMs, qms);
    std::wstring key = s.Key();
    if (s.ok && key != g_dock.selectionKey) {
        g_dock.selectionKey = key;
        if (s.count == 1 && IsFileSystemFolder(s.path)) {
            Show(L"selected folder " + s.path, s.path);
        } else if (s.count == 1) {
            Show(L"selected a file or non-file-system item (" + s.path + L") -> hide", L"");
        } else {
            Show(Format(L"selection: %lu items -> hide", s.count), L"");
        }
    }
    g_dock.busy = false;
}

void CALLBACK OnSystem(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    double at = Since(g_dock.start);
    if (event == EVENT_SYSTEM_FOREGROUND) {
        HWND root = Root(hwnd);
        if (root == g_dock.target) {
            PlacePanel(true);
        } else if (!g_dock.owned && IsExplorerTop(root)) {
            g_dock.target = root;
            HookTargetLocation();
            ForgetView();
            g_dock.tabBrowsers.clear();
            g_dock.browserHwnd = nullptr;
            Print(L"[%7.2f s] retarget to Explorer window 0x%llx\n", at, reinterpret_cast<std::uint64_t>(root));
            PostMessageW(g_dock.panel, WM_TIMER, kViewTimer, 0);  // check its view outside this callback
        }
    } else if (hwnd == g_dock.target && !g_dock.owned) {
        if (event == EVENT_SYSTEM_MINIMIZESTART) {
            ShowWindow(g_dock.panel, SW_HIDE);
            Print(L"[%7.2f s] target minimized: panel hidden\n", at);
        } else if (event == EVENT_SYSTEM_MINIMIZEEND && !g_dock.shown.empty()) {
            ShowWindow(g_dock.panel, SW_SHOWNOACTIVATE);
            PlacePanel(true);
            Print(L"[%7.2f s] target restored: panel shown\n", at);
        }
    }
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kViewTimer) {
                if (!IsWindow(g_dock.target)) {
                    Print(L"[%7.2f s] target window closed\n", Since(g_dock.start));
                    DestroyWindow(hwnd);
                    return 0;
                }
                Tick();
            } else if (wp == kEndTimer) {
                DestroyWindow(hwnd);
            } else if (wp == kHangTimer) {
                KillTimer(hwnd, kHangTimer);
                Print(L"[%7.2f s] HANG START: the panel stops responding for %u s. Click, type and drag in "
                      L"Explorer now.\n",
                      Since(g_dock.start), g_dock.hangFor);
                Sleep(g_dock.hangFor * 1000);
                Print(L"[%7.2f s] HANG END\n", Since(g_dock.start));
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            InflateRect(&rc, -8, -8);
            std::wstring text = Format(L"DirSize probe (%s%s%s)\n%s\n\n", g_dock.owned ? L"owned" : L"not owned",
                                       g_dock.hooks ? L"" : L", no hooks", g_dock.subscribe ? L", subscribed" : L"",
                                       g_dock.placement.c_str());
            for (const std::wstring& line : g_dock.lines) text += line + L"\n";
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, text.c_str(), -1, &rc, DT_WORDBREAK | DT_NOPREFIX);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL WINAPI OnConsoleCtrl(DWORD) {
    // Ctrl+C in the console: close the panel so the results are printed.
    if (g_dock.panel) PostMessageW(g_dock.panel, WM_CLOSE, 0, 0);
    return TRUE;
}

struct DockOptions {
    bool owned = false;
    bool subscribe = false;
    bool hooks = true;
    unsigned hangAfter = 0, hangFor = 0, seconds = 0;
};

int RunDock(const DockOptions& o) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ComPtr<IShellWindows> windows = OpenShellWindows();
    if (!windows) return 1;
    g_dock.owned = o.owned;
    g_dock.subscribe = o.subscribe;
    g_dock.hooks = o.hooks;
    g_dock.hangAfter = o.hangAfter;
    g_dock.hangFor = o.hangFor;
    g_dock.windows = windows.Get();
    g_dock.start = Clock::now();
    Print(L"mode: %s, WinEvent hooks %s, SelectionChanged subscription %s\n", o.owned ? L"owned" : L"not owned",
          o.hooks ? L"on" : L"off", o.subscribe ? L"on (events ignored)" : L"off");

    Print(L"Bring an Explorer window to the front within 5 s...\n");
    for (int i = 0; i < 50 && !IsExplorerTop(Root(GetForegroundWindow())); ++i) Sleep(100);
    g_dock.target = Root(GetForegroundWindow());
    if (!IsExplorerTop(g_dock.target)) {
        Print(L"No Explorer window in front.\n");
        return 1;
    }
    Print(L"target: 0x%llx\n", reinterpret_cast<std::uint64_t>(g_dock.target));

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DirSizeExplorerProbe";
    RegisterClassExW(&wc);
    g_dock.panel = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"DirSize probe",
                                   WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, 0, 0, 320, 400,
                                   o.owned ? g_dock.target : nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_dock.panel) {
        Print(L"CreateWindowExW failed: %lu\n", GetLastError());
        return 1;
    }
    Print(L"owner of the panel: 0x%llx\n", reinterpret_cast<std::uint64_t>(GetWindow(g_dock.panel, GW_OWNER)));
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    HookTargetLocation();
    HWINEVENTHOOK fgHook = nullptr, minHook = nullptr;
    if (o.hooks) {
        fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnSystem, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        minHook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnSystem, 0, 0,
                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    SetTimer(g_dock.panel, kViewTimer, kViewMs, nullptr);
    if (o.hangFor) SetTimer(g_dock.panel, kHangTimer, o.hangAfter * 1000, nullptr);
    if (o.seconds) SetTimer(g_dock.panel, kEndTimer, o.seconds * 1000, nullptr);
    Print(L"Click folders in the navigation pane and in the file list, files, empty space; open folders;\n"
          L"use Back; move, resize, minimize and restore the window.\n"
          L"Ends after --seconds, or press Ctrl+C here.\n");

    double cpu0 = ProcessCpuSeconds(GetCurrentProcess());
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    double wall = Since(g_dock.start);
    double cpu = ProcessCpuSeconds(GetCurrentProcess()) - cpu0;
    ForgetView();
    if (fgHook) UnhookWinEvent(fgHook);
    if (minHook) UnhookWinEvent(minHook);
    if (g_dock.locationHook) UnhookWinEvent(g_dock.locationHook);
    Print(L"\nresult: %.1f s, CPU %.3f s (%.2f%% of one core)\n", wall, cpu, cpu / wall * 100);
    Print(L"  ticks %u (every %u ms while Explorer was in front), navigations %u\n", g_dock.ticks, kViewMs,
          g_dock.navigations);
    Print(L"  full view checks %u, avg %.2f ms, max %.2f ms\n", g_dock.fullChecks,
          g_dock.fullChecks ? g_dock.sumFullMs / g_dock.fullChecks : 0.0, g_dock.maxFullMs);
    Print(L"  selection reads %u, avg %.2f ms, max %.2f ms\n", g_dock.quickChecks,
          g_dock.quickChecks ? g_dock.sumQuickMs / g_dock.quickChecks : 0.0, g_dock.maxQuickMs);
    if (o.subscribe) Print(L"  subscriptions %u (failed %u)\n", g_dock.subscribes, g_dock.subscribeFailures);
    Print(L"  location events %u, placements %u\n", g_dock.locationEvents, g_dock.placements);
    Print(L"  inside-panel raises %u, last placement: %s\n", g_dock.raises, g_dock.placement.c_str());
    Print(L"  front-tab changes %u\n", g_dock.tabSwitches);
    return 0;
}

// ---- verb --------------------------------------------------------------------------------------

const wchar_t* const kVerbKeys[] = {
    L"Software\\Classes\\Directory\\shell\\DirSizeProbe",
    L"Software\\Classes\\Directory\\Background\\shell\\DirSizeProbe",
};

bool SetDefault(const std::wstring& key, const std::wstring& value) {
    HKEY h = nullptr;
    LSTATUS s = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr);
    if (s != ERROR_SUCCESS) {
        Print(L"RegCreateKeyExW %s failed: %ld\n", key.c_str(), s);
        return false;
    }
    s = RegSetValueExW(h, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    if (s != ERROR_SUCCESS) Print(L"RegSetValueExW %s failed: %ld\n", key.c_str(), s);
    return s == ERROR_SUCCESS;
}

int RunVerb(bool install) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    bool ok = true;
    for (int i = 0; i < 2; ++i) {
        std::wstring key = kVerbKeys[i];
        if (install) {
            // %1 for a folder, %V for the folder background.
            std::wstring command = std::wstring(L"\"") + exe + L"\" verb-invoked \"" + (i == 0 ? L"%1" : L"%V") + L"\"";
            ok &= SetDefault(key, L"DirSize probe");
            ok &= SetDefault(key + L"\\command", command);
        } else {
            LSTATUS s = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
            if (s != ERROR_SUCCESS && s != ERROR_FILE_NOT_FOUND) {
                Print(L"RegDeleteTreeW %s failed: %ld\n", key.c_str(), s);
                ok = false;
            }
            RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    Print(install ? L"verb installed under HKCU (Directory, Directory\\Background): \"DirSize probe\"\n"
                  : L"verb removed\n");
    return ok ? 0 : 1;
}

int RunVerbInvoked(const wchar_t* path) {
    Print(L"verb invoked with: %s\nPress Enter to close.\n", path);
    std::getchar();
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        Print(L"CoInitializeEx failed\n");
        return 1;
    }
    auto arg = [&](int i) { return i < argc ? std::wstring_view(argv[i]) : std::wstring_view(); };
    auto num = [&](int i, unsigned def) { return i < argc ? static_cast<unsigned>(std::wcstoul(argv[i], nullptr, 10)) : def; };
    int rc = 2;
    if (arg(1) == L"windows") {
        rc = RunWindows();
    } else if (arg(1) == L"events") {
        rc = RunEvents(std::max(1u, num(2, 60)));
    } else if (arg(1) == L"dock" && (arg(2) == L"owned" || arg(2) == L"events")) {
        DockOptions o;
        o.owned = arg(2) == L"owned";
        for (int i = 3; i < argc; ++i) {
            if (arg(i) == L"--subscribe") o.subscribe = true;
            if (arg(i) == L"--no-hooks") o.hooks = false;
            if (arg(i) == L"--hang-after") o.hangAfter = num(i + 1, 0);
            if (arg(i) == L"--hang-for") o.hangFor = num(i + 1, 0);
            if (arg(i) == L"--seconds") o.seconds = num(i + 1, 0);
        }
        rc = RunDock(o);
    } else if (arg(1) == L"verb" && (arg(2) == L"install" || arg(2) == L"remove")) {
        rc = RunVerb(arg(2) == L"install");
    } else if (arg(1) == L"verb-invoked" && argc >= 3) {
        rc = RunVerbInvoked(argv[2]);
    } else {
        Print(L"usage:\n"
              L"  explorer-probe windows\n"
              L"  explorer-probe events [seconds=60]\n"
              L"  explorer-probe dock owned|events [--subscribe] [--no-hooks] [--seconds N] [--hang-after S --hang-for S]\n"
              L"  explorer-probe verb install|remove\n");
    }
    CoUninitialize();
    return rc;
}
