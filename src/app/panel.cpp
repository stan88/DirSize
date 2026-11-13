#include "app/panel.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <string>

#include "ui/explorer_tracker.h"
#include "ui/folder_source.h"
#include "ui/format.h"
#include "ui/placement.h"
#include "ui/settings.h"
#include "ui/size_list.h"
#include "ui/strings.h"
#include "ui/theme.h"

using namespace dirsize::ui;

namespace {

constexpr wchar_t kPanelClass[] = L"DirSizePanel";
constexpr UINT kThemeChanged = WM_APP + 1;
constexpr UINT kFolderLoaded = WM_APP + 2;
constexpr UINT kExplorerChanged = WM_APP + 3;
constexpr UINT_PTR kZOrderTimer = 1;
constexpr int kDefaultWidthDip = 340;  // with the share column (list_model.h)
constexpr int kMinHeightDip = 160;
// Drag strips at the resizable edges (the side away from Explorer, and the
// bottom). The list stays out of them, so they do not overlap its scroll
// bar, and a small handle in each shows where to drag (user request).
constexpr int kGripDip = 8;
constexpr int kHandleDip = 32;  // length of the handle drawn in a strip
constexpr int kHeaderDip = 54;
constexpr wchar_t kSettingsKey[] = L"Software\\DirSize";

// The panel size the user chose, in DIPs (HKCU\Software\DirSize). Height 0
// = as tall as Explorer.
void LoadPanelSize(int& widthDip, int& heightDip) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"PanelWidth", RRF_RT_REG_DWORD, nullptr, &value, &size) ==
        ERROR_SUCCESS) {
        widthDip = std::clamp(static_cast<int>(value), kPanelMinWidthDip, kPanelMaxWidthDip);
    }
    size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"PanelHeight", RRF_RT_REG_DWORD, nullptr, &value, &size) ==
        ERROR_SUCCESS) {
        heightDip = value == 0 ? 0 : std::max(static_cast<int>(value), kMinHeightDip);
    }
}

void SavePanelSize(int widthDip, int heightDip) {
    DWORD w = static_cast<DWORD>(widthDip), h = static_cast<DWORD>(heightDip);
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"PanelWidth", REG_DWORD, &w, sizeof(w));
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"PanelHeight", REG_DWORD, &h, sizeof(h));
}

std::wstring Join(const std::wstring& folder, const std::wstring& name) {
    return !folder.empty() && folder.back() == L'\\' ? folder + name : folder + L"\\" + name;
}

std::wstring NoteText(ServiceNote note) {
    switch (note) {
        case ServiceNote::Updating: return Text(IDS_NOTE_UPDATING);
        case ServiceNote::Starting: return Text(IDS_NOTE_STARTING);
        case ServiceNote::NotRunning: return Text(IDS_NOTE_NOT_RUNNING);
        case ServiceNote::NotInstalled: return Text(IDS_NOTE_NOT_INSTALLED);
        case ServiceNote::NotIndexed: return Text(IDS_NOTE_NOT_INDEXED);
        case ServiceNote::Indexing: return Text(IDS_NOTE_INDEXING);
        case ServiceNote::NotLocal: return Text(IDS_NOTE_NOT_LOCAL);
        case ServiceNote::None: break;
    }
    return L"";
}

// "C:\Users\x" -> "x"; "C:\" stays.
std::wstring LastName(const std::wstring& path) {
    if (path.size() <= 3) return path;
    size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

class Panel;
Panel* g_panel = nullptr;  // for the WinEvent callbacks (they have no context pointer)

class Panel {
public:
    bool Create(HINSTANCE instance);

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                                    DWORD);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void OnExplorer(std::unique_ptr<ExplorerUpdate> u);
    void OnLoaded(std::unique_ptr<FolderResult> r);
    void HookTarget();
    void Unhook();
    void UpdateVisibility();
    void Place(bool raise);
    void ApplyTheme();
    void UpdateFonts();
    void Layout();
    void PaintHeader(HDC dc, const RECT& client);
    RECT CloseBox(const RECT& client) const;
    LRESULT HitTest(LPARAM lp) const;
    void OnSizeEnd();
    void ShowMenu(POINT screen);
    int ShareDip() const;  // width of the share column (the user's or the default)
    RECT WidthStrip(const RECT& client) const;
    RECT BottomStrip(const RECT& client) const;
    void PaintStrips(HDC dc, const RECT& client);
    void SetEdgeHover(bool hover);
    int Border() const { return rounded_ ? 0 : std::max(1, Scale(1)); }
    int Scale(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    SizeList* list_ = nullptr;
    ThemeWatcher theme_;
    Palette palette_;
    std::unique_ptr<FolderSource> source_;
    std::unique_ptr<ExplorerTracker> tracker_;
    HWND target_ = nullptr;
    HWINEVENTHOOK locationHook_ = nullptr;
    HWINEVENTHOOK systemHook_ = nullptr;
    HWINEVENTHOOK minimizeHook_ = nullptr;
    std::wstring folder_;
    std::wstring status_;
    // The last loaded folder's totals, to rebuild the status line when the
    // size unit changes.
    bool haveTotals_ = false;
    std::uint64_t totalAllocated_ = 0;
    bool fromService_ = false;
    ServiceNote note_ = ServiceNote::None;
    SizeUnit unit_ = SizeUnit::Auto;
    void BuildStatus();
    std::uint64_t request_ = 0;
    PanelSide side_ = PanelSide::OutsideRight;
    UINT dpi_ = 96;
    HFONT font_ = nullptr;
    HFONT titleFont_ = nullptr;
    int widthDip_ = kDefaultWidthDip;  // saved width: with the share column
    int heightDip_ = 0;     // 0 = as tall as Explorer
    bool showShare_ = true; // settings.h; without it the panel is narrower
    bool edgeHover_ = false;  // the mouse is over a drag strip
    bool sizing_ = false;   // the user is dragging an edge
    bool rounded_ = false;  // Windows 11 draws rounded corners and the border
};

bool Panel::Create(HINSTANCE instance) {
    instance_ = instance;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPanelClass;
    if (!RegisterClassExW(&wc)) return false;
    // Not an owned window of Explorer: in the VM test an owned window made
    // Explorer freeze whenever the panel stopped responding.
    // WS_EX_NOACTIVATE: clicking the panel leaves Explorer in front.
    // WS_THICKFRAME: resizable (and tells DWM the window has a frame it can
    // round); WM_NCCALCSIZE removes the visible frame again.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kPanelClass, Text(IDS_APP_TITLE).c_str(),
                            WS_POPUP | WS_THICKFRAME, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    return hwnd_ != nullptr;
}

LRESULT CALLBACK Panel::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

void CALLBACK Panel::OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    Panel* p = g_panel;
    if (!p || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    switch (event) {
        case EVENT_OBJECT_LOCATIONCHANGE:
            if (hwnd == p->target_) p->Place(false);
            break;
        case EVENT_SYSTEM_FOREGROUND:
            if (GetAncestor(hwnd, GA_ROOT) == p->target_) p->Place(true);
            break;
        case EVENT_SYSTEM_MINIMIZESTART:
        case EVENT_SYSTEM_MINIMIZEEND:
            if (hwnd == p->target_) p->UpdateVisibility();
            break;
    }
}

void Panel::OnCreate() {
    dpi_ = GetDpiForWindow(hwnd_);
    LoadPanelSize(widthDip_, heightDip_);
    // Rounded corners: DWMWA_WINDOW_CORNER_PREFERENCE, Windows 11 build 22000
    // and later. On Windows 10 the call fails and the panel draws its own
    // square border. (Per the documentation, windows in a VM are not rounded.)
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    rounded_ = SUCCEEDED(DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner)));
    UpdateFonts();
    showShare_ = ShowShareColumn();
    unit_ = SizeUnitSetting();
    list_ = SizeList::Create(hwnd_, instance_);
    if (list_) {
        list_->SetUnit(unit_);
        list_->SetColumns(PanelColumns(showShare_));
        list_->SetCompact(true);
        list_->SetColumnWidths(ParseColumnWidths(ColumnWidthsSetting(true)));
        list_->SetGrowParent(true);  // a resized column changes the panel width
        list_->SetDpi(dpi_);
    }
    theme_.Watch(hwnd_, kThemeChanged);
    ApplyTheme();
    source_ = std::make_unique<FolderSource>(hwnd_, kFolderLoaded);
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    systemHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0, flags);
    minimizeHook_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnWinEvent, 0, 0,
                                    flags);
    std::wstring tracePath;
    if (wcsstr(GetCommandLineW(), L"--trace")) {
        // Diagnostics: %TEMP%\DirSize-panel-trace.txt (local only).
        wchar_t temp[MAX_PATH + 1] = {};
        DWORD n = GetTempPathW(MAX_PATH + 1, temp);
        if (n > 0 && n <= MAX_PATH) tracePath = std::wstring(temp) + L"DirSize-panel-trace.txt";
    }
    tracker_ = std::make_unique<ExplorerTracker>(hwnd_, kExplorerChanged, tracePath);
    SetTimer(hwnd_, kZOrderTimer, 250, nullptr);
}

void Panel::HookTarget() {
    if (locationHook_) UnhookWinEvent(locationHook_);
    locationHook_ = nullptr;
    DWORD pid = 0;
    if (target_ && GetWindowThreadProcessId(target_, &pid)) {
        // Only Explorer's process: location changes of the whole system
        // (caret, cursor) would wake this thread all the time.
        locationHook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnWinEvent,
                                        pid, 0, WINEVENT_OUTOFCONTEXT);
    }
}

void Panel::Unhook() {
    for (HWINEVENTHOOK* h : {&locationHook_, &systemHook_, &minimizeHook_}) {
        if (*h) UnhookWinEvent(*h);
        *h = nullptr;
    }
}

void Panel::OnExplorer(std::unique_ptr<ExplorerUpdate> u) {
    if (u->explorer != target_) {
        target_ = u->explorer;
        HookTarget();
    }
    if (u->folder != folder_) {
        folder_ = u->folder;
        if (!folder_.empty() && list_) {
            status_ = Text(IDS_LOADING);
            haveTotals_ = false;
            list_->SetItems({}, 0, false);
            list_->SetMessage(Text(IDS_LOADING));
            request_ = source_->Request(folder_, true);  // files too (user request 2026-11-13)
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    UpdateVisibility();
}

void Panel::OnLoaded(std::unique_ptr<FolderResult> r) {
    if (r->request != request_ || !list_) return;
    if (r->walking) {  // interim: why the service did not answer; the result follows
        std::wstring note = NoteText(r->note);
        list_->SetMessage(Text(IDS_WALKING));
        status_ = note.empty() ? Text(IDS_WALKING) : note;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    switch (r->status) {
        case LoadStatus::NotFound:
            list_->SetMessage(Text(IDS_ERR_NOT_FOUND));
            status_.clear();
            break;
        case LoadStatus::AccessDenied:
            list_->SetMessage(Text(IDS_ERR_ACCESS));
            status_.clear();
            break;
        case LoadStatus::Failed:
            list_->SetMessage(Format(IDS_ERR_FAILED, {std::to_wstring(r->error)}));
            status_.clear();
            break;
        case LoadStatus::Ok: {
            list_->SetMessage(L"");
            list_->SetItems(std::move(r->items), r->totalAllocated, false);
            haveTotals_ = true;
            totalAllocated_ = r->totalAllocated;
            fromService_ = r->fromService;
            note_ = r->note;
            BuildStatus();
            break;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Panel::BuildStatus() {
    if (!haveTotals_) return;
    status_ = Text(IDS_COL_ALLOCATED) + L" " + FormatSize(totalAllocated_, unit_) + L"  \u00B7  " +
              Text(fromService_ ? IDS_SRC_SERVICE : IDS_SRC_WALK);
    std::wstring note = NoteText(note_);
    if (!note.empty()) status_ += L"  \u00B7  " + note;
}

void Panel::UpdateVisibility() {
    const bool show = !folder_.empty() && target_ && IsWindow(target_) && !IsIconic(target_);
    if (show) {
        if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        Place(true);
    } else if (IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void Panel::Place(bool raise) {
    if (sizing_) return;  // the user is dragging an edge
    if (!IsWindowVisible(hwnd_) || !target_ || !IsWindow(target_) || IsIconic(target_)) return;
    RECT frame;
    if (FAILED(DwmGetWindowAttribute(target_, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) return;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(target_, MONITOR_DEFAULTTONEAREST), &mi);
    const int targetDpi = static_cast<int>(GetDpiForWindow(target_));
    const int width = MulDiv(PanelShownWidthDip(widthDip_, showShare_, ShareDip()), targetDpi, 96);
    const int height = heightDip_ > 0 ? MulDiv(heightDip_, targetDpi, 96) : 0;
    PanelPlacement p = PlacePanel(frame, mi.rcWork, width, height);
    const bool sideChanged = WidthEdgeIsRight(p.side) != WidthEdgeIsRight(side_);
    side_ = p.side;
    if (sideChanged) {  // the drag strip moves to the other side
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    // Inside, the panel overlaps Explorer, so it must stay above it.
    if (side_ == PanelSide::InsideRight) raise = true;
    HWND after = nullptr;
    UINT flags = SWP_NOACTIVATE;
    if (raise) {
        // Directly above Explorer: below the window that is above it.
        after = GetWindow(target_, GW_HWNDPREV);
        if (after == hwnd_) flags |= SWP_NOZORDER;
        else if (!after) after = HWND_TOP;
    } else {
        flags |= SWP_NOZORDER;
    }
    SetWindowPos(hwnd_, after, p.x, p.top, width, p.bottom - p.top, flags);
}

void Panel::ApplyTheme() {
    palette_ = theme_.Current();
    if (rounded_) {
        COLORREF border = palette_.line;
        DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    }
    if (list_) list_->SetPalette(palette_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Panel::UpdateFonts() {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_)) return;
    if (font_) DeleteObject(font_);
    if (titleFont_) DeleteObject(titleFont_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW title = ncm.lfMessageFont;
    title.lfWeight = FW_SEMIBOLD;
    titleFont_ = CreateFontIndirectW(&title);
}

int Panel::ShareDip() const {
    return list_ ? list_->ColumnDip(Column::Share) : ColumnWidthDip(Column::Share, true);
}

RECT Panel::WidthStrip(const RECT& client) const {
    const int border = Border(), grip = Scale(kGripDip);
    const int left = WidthEdgeIsRight(side_) ? client.right - border - grip : client.left + border;
    return RECT{left, Scale(kHeaderDip), left + grip, client.bottom - border - grip};
}

RECT Panel::BottomStrip(const RECT& client) const {
    const int border = Border(), grip = Scale(kGripDip);
    return RECT{client.left + border, client.bottom - border - grip, client.right - border, client.bottom - border};
}

void Panel::Layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (list_) {
        // Between the header, the drag strips and the border.
        const RECT strip = WidthStrip(rc);
        const int border = Border(), grip = Scale(kGripDip);
        const int left = WidthEdgeIsRight(side_) ? border : strip.right;
        const int right = WidthEdgeIsRight(side_) ? strip.left : rc.right - border;
        MoveWindow(list_->Window(), left, Scale(kHeaderDip), std::max(0, right - left),
                   std::max(0, static_cast<int>(rc.bottom) - Scale(kHeaderDip) - border - grip), TRUE);
    }
}

// The strips with a short handle in the middle of each: the line color
// normally, the accent color while the mouse is over a strip.
void Panel::PaintStrips(HDC dc, const RECT& client) {
    const RECT side = WidthStrip(client), bottom = BottomStrip(client);
    SetDCBrushColor(dc, palette_.background);
    FillRect(dc, &side, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    FillRect(dc, &bottom, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const int thick = std::max(2, Scale(2)), length = Scale(kHandleDip);
    SetDCBrushColor(dc, edgeHover_ ? palette_.bar : palette_.line);
    const int cx = (side.left + side.right) / 2, cy = (side.top + side.bottom) / 2;
    RECT v{cx - thick / 2, cy - length / 2, cx - thick / 2 + thick, cy + length / 2};
    FillRect(dc, &v, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const int bx = (bottom.left + bottom.right) / 2, by = (bottom.top + bottom.bottom) / 2;
    RECT h{bx - length / 2, by - thick / 2, bx + length / 2, by - thick / 2 + thick};
    FillRect(dc, &h, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Panel::SetEdgeHover(bool hover) {
    if (hover == edgeHover_) return;
    edgeHover_ = hover;
    if (hover) {
        // WM_NCMOUSELEAVE when the mouse leaves the strips (they are
        // "non-client": WM_NCHITTEST returns the sizing codes there).
        TRACKMOUSEEVENT tme{sizeof(tme)};
        tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
        tme.hwndTrack = hwnd_;
        TrackMouseEvent(&tme);
    }
    RECT rc;
    GetClientRect(hwnd_, &rc);
    RECT side = WidthStrip(rc), bottom = BottomStrip(rc);
    InvalidateRect(hwnd_, &side, FALSE);
    InvalidateRect(hwnd_, &bottom, FALSE);
}

RECT Panel::CloseBox(const RECT& client) const {
    const int size = Scale(28);
    return RECT{client.right - size - Scale(4), Scale(4), client.right - Scale(4), Scale(4) + size};
}

void Panel::PaintHeader(HDC dc, const RECT& client) {
    // Windows 10: our own 1 px border around everything. Windows 11: DWM
    // draws the rounded border (DWMWA_BORDER_COLOR).
    SetDCBrushColor(dc, palette_.line);
    if (!rounded_) FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const int border = Border();
    RECT header{border, border, client.right - border, Scale(kHeaderDip)};
    SetDCBrushColor(dc, palette_.background);
    FillRect(dc, &header, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    RECT close = CloseBox(client);
    const int pad = Scale(10);
    RECT line1{pad, Scale(6), close.left - Scale(4), Scale(28)};
    HGDIOBJ old = SelectObject(dc, titleFont_);
    SetTextColor(dc, palette_.text);
    std::wstring name = LastName(folder_);
    DrawTextW(dc, name.c_str(), -1, &line1, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, font_);
    SetTextColor(dc, palette_.secondaryText);
    DrawTextW(dc, L"\u2715", -1, &close, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT line2{pad, Scale(28), client.right - pad, Scale(50)};
    DrawTextW(dc, status_.c_str(), -1, &line2, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

LRESULT Panel::HitTest(LPARAM lp) const {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd_, &pt);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int grip = Scale(kGripDip);
    const bool right = WidthEdgeIsRight(side_);
    const bool widthEdge = right ? pt.x >= rc.right - grip : pt.x < grip;
    const bool bottom = pt.y >= rc.bottom - grip;
    if (bottom && widthEdge) return right ? HTBOTTOMRIGHT : HTBOTTOMLEFT;
    if (bottom) return HTBOTTOM;
    if (widthEdge) return right ? HTRIGHT : HTLEFT;
    return HTCLIENT;
}

void Panel::OnSizeEnd() {
    sizing_ = false;
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    const int targetDpi = target_ ? static_cast<int>(GetDpiForWindow(target_)) : static_cast<int>(dpi_);
    widthDip_ = PanelSavedWidthDip(MulDiv(wr.right - wr.left, 96, targetDpi), showShare_, ShareDip());
    // As tall as Explorer (or taller): follow Explorer's height again.
    RECT frame{};
    int full = 0;
    if (target_ && SUCCEEDED(DwmGetWindowAttribute(target_, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(target_, MONITOR_DEFAULTTONEAREST), &mi);
        full = std::min(frame.bottom, mi.rcWork.bottom) - std::max(frame.top, mi.rcWork.top);
    }
    const int h = wr.bottom - wr.top;
    heightDip_ = full > 0 && h >= full - Scale(2) ? 0 : std::max(MulDiv(h, 96, targetDpi), kMinHeightDip);
    SavePanelSize(widthDip_, heightDip_);
    Place(true);
}

// Right-click on the panel: its settings (share column, size unit).
// The panel never takes the focus (WS_EX_NOACTIVATE), but a popup menu
// needs its owner in the foreground to close when the user clicks
// elsewhere (TrackPopupMenu remarks), so the panel is brought forward for
// the menu and Explorer afterwards.
void Panel::ShowMenu(POINT screen) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const bool showShare = ShowShareColumn();
    AppendMenuW(menu, MF_STRING | (showShare ? MF_CHECKED : MF_UNCHECKED), 1, Text(IDS_MENU_SHOW_SHARE).c_str());
    // Size unit: commands 10 + SizeUnit.
    const SizeUnit unit = SizeUnitSetting();
    if (HMENU unitMenu = CreatePopupMenu()) {
        const struct { SizeUnit unit; UINT text; } units[] = {
            {SizeUnit::Auto, IDS_MENU_UNIT_AUTO}, {SizeUnit::KB, IDS_MENU_UNIT_KB}, {SizeUnit::GB, IDS_MENU_UNIT_GB}};
        for (const auto& u : units) {
            AppendMenuW(unitMenu, MF_STRING | (u.unit == unit ? MF_CHECKED : MF_UNCHECKED),
                        10 + static_cast<UINT>(u.unit), Text(u.text).c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(unitMenu), Text(IDS_MENU_UNIT).c_str());
    }
    SetForegroundWindow(hwnd_);
    UINT cmd = static_cast<UINT>(
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr));
    DestroyMenu(menu);
    if (target_ && IsWindow(target_)) SetForegroundWindow(target_);
    if (cmd == 1 && SetShowShareColumn(!showShare) == ERROR_SUCCESS) NotifySettingsChanged();
    if (cmd >= 10 && cmd <= 12) {
        const SizeUnit chosen = static_cast<SizeUnit>(cmd - 10);
        if (chosen != unit && SetSizeUnitSetting(chosen) == ERROR_SUCCESS) NotifySettingsChanged();
    }
}

LRESULT Panel::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    // A registered message: not a constant, so not a case label.
    if (msg == SettingsChangedMessage() && msg != 0) {
        const bool show = ShowShareColumn();
        if (show != showShare_) {
            showShare_ = show;
            if (list_) list_->SetColumns(PanelColumns(showShare_));
            Place(false);  // narrower or wider by the share column
        }
        const SizeUnit unit = SizeUnitSetting();
        if (unit != unit_) {
            unit_ = unit;
            if (list_) list_->SetUnit(unit_);
            BuildStatus();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    switch (msg) {
        case WM_NCMOUSEMOVE:
            SetEdgeHover(wp == HTLEFT || wp == HTRIGHT || wp == HTBOTTOM || wp == HTBOTTOMLEFT ||
                         wp == HTBOTTOMRIGHT);
            break;
        case WM_NCMOUSELEAVE:
            SetEdgeHover(false);
            return 0;
        case WM_MOUSEMOVE:
            SetEdgeHover(false);
            return 0;
        case kListGrowParent:
            // A column got wider or narrower by wParam DIPs: so does the
            // panel, and the other columns keep their widths. At the panel's
            // limits the name column takes the rest.
            widthDip_ = std::clamp(widthDip_ + static_cast<int>(wp), kPanelMinWidthDip, kPanelMaxWidthDip);
            Place(false);
            return 0;
        case kListColumnsResized:  // the drag ended: keep the widths
            if (list_) SetColumnWidthsSetting(true, FormatColumnWidths(list_->ColumnWidths()));
            SavePanelSize(widthDip_, heightDip_);
            return 0;
        case kListContextMenu:  // right-click or the menu key on the list
            ShowMenu(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_CONTEXTMENU: {  // right-click on the panel's own header
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);  // keyboard
            ShowMenu(pt);
            return 0;
        }
        case WM_CREATE:
            OnCreate();
            return 0;
        case WM_NCCALCSIZE:
            if (wp) return 0;  // no visible frame: the client area is the whole window
            break;
        case WM_NCACTIVATE:
            // The panel becomes active only for its right-click menu. Left to
            // DefWindowProc, the change drew the thick sizing frame over the
            // panel on Windows 10 (VM screenshot). The panel draws everything
            // itself, so nothing is drawn here; TRUE completes the change of
            // active window, as the WM_NCACTIVATE documentation requires.
            return TRUE;
        case WM_NCHITTEST:
            return HitTest(lp);
        case WM_ENTERSIZEMOVE:
            sizing_ = true;
            return 0;
        case WM_EXITSIZEMOVE:
            OnSizeEnd();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = Scale(PanelMinWidthDip(showShare_));
            mmi->ptMinTrackSize.y = Scale(kMinHeightDip);
            mmi->ptMaxTrackSize.x = Scale(PanelMaxWidthDip(showShare_, ShareDip()));
            return 0;
        }
        case kListOpenItem: {
            // Double-click: Explorer browses to that folder; the panel then
            // follows Explorer (the tracker sees the new view).
            size_t index = wp;
            if (list_ && tracker_ && index < list_->Items().size() && list_->Items()[index].isFolder) {
                tracker_->Navigate(Join(folder_, list_->Items()[index].name));
            }
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SIZE:
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rc;
            GetClientRect(hwnd_, &rc);
            PaintHeader(dc, rc);
            PaintStrips(dc, rc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_LBUTTONUP: {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            RECT close = CloseBox(rc);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&close, pt)) DestroyWindow(hwnd_);
            return 0;
        }
        case WM_TIMER:
            if (wp == kZOrderTimer) {
                if (target_ && !IsWindow(target_)) {
                    target_ = nullptr;
                    folder_.clear();
                    UpdateVisibility();
                } else if (IsWindowVisible(hwnd_) && side_ == PanelSide::InsideRight &&
                           GetWindow(target_, GW_HWNDPREV) != hwnd_) {
                    Place(true);  // Explorer got above the overlapping panel
                }
            }
            return 0;
        case kExplorerChanged:
            OnExplorer(std::unique_ptr<ExplorerUpdate>(reinterpret_cast<ExplorerUpdate*>(lp)));
            return 0;
        case kFolderLoaded:
            OnLoaded(std::unique_ptr<FolderResult>(reinterpret_cast<FolderResult*>(lp)));
            return 0;
        case kThemeChanged:
        case WM_SYSCOLORCHANGE:
            ApplyTheme();
            return 0;
        case WM_SETTINGCHANGE:
            ApplyTheme();
            break;
        case WM_DPICHANGED:
            // The panel positions itself next to Explorer; only the metrics change.
            dpi_ = HIWORD(wp);
            UpdateFonts();
            if (list_) list_->SetDpi(dpi_);
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kZOrderTimer);
            Unhook();
            tracker_.reset();
            source_.reset();
            if (font_) DeleteObject(font_);
            if (titleFont_) DeleteObject(titleFont_);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace

int RunPanel(HINSTANCE instance) {
    Panel panel;
    g_panel = &panel;
    if (!panel.Create(instance)) return 1;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_panel = nullptr;
    return 0;
}
