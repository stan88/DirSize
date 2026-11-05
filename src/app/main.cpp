// DirSize front end (Phase 5). This first version is the standalone window:
// a folder's subfolders and files with their sizes, following the system
// theme. The Explorer panel comes next (docs/DESIGN-phase5.md section 8).
//
//   dirsize.exe [folder]

#include <windows.h>
#include <unknwn.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <winrt/base.h>
#include <wrl/client.h>

#include <memory>
#include <string>

#include "app/panel.h"
#include "ui/folder_source.h"
#include "ui/format.h"
#include "ui/size_list.h"
#include "ui/strings.h"
#include "ui/theme.h"

using Microsoft::WRL::ComPtr;
using namespace dirsize::ui;

namespace {

constexpr wchar_t kWindowClass[] = L"DirSizeMainWindow";
constexpr UINT kThemeChanged = WM_APP + 1;
constexpr UINT kFolderLoaded = WM_APP + 2;
constexpr WORD kCmdRefresh = 1;
constexpr WORD kCmdPick = 2;
constexpr WORD kCmdUp = 3;

std::wstring ParentOf(const std::wstring& path) {
    if (path.size() <= 3) return path;  // "C:\" stays
    size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return path;
    if (slash <= 2) return path.substr(0, 3);
    return path.substr(0, slash);
}

std::wstring Join(const std::wstring& folder, const std::wstring& name) {
    return folder.back() == L'\\' ? folder + name : folder + L"\\" + name;
}

// "C:" -> "C:\", "C:\x\" -> "C:\x".
std::wstring Normalize(std::wstring path) {
    if (path.size() == 2 && path[1] == L':') path += L'\\';
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
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

class MainWindow {
public:
    static bool Register(HINSTANCE instance);
    static HWND Create(HINSTANCE instance, const std::wstring& folder);

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void OnCreate();
    void ApplyTheme();
    void UpdateFonts();
    void Layout();
    void PaintHeader(HDC dc, const RECT& client);
    void Navigate(const std::wstring& path, bool refresh);
    void OnLoaded(std::unique_ptr<FolderResult> r);
    void PickFolder();
    int Scale(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }
    int HeaderHeight() const { return Scale(58); }

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    SizeList* list_ = nullptr;
    ThemeWatcher theme_;
    Palette palette_;
    std::unique_ptr<FolderSource> source_;
    std::wstring path_;
    std::uint64_t request_ = 0;
    std::wstring status_;
    UINT dpi_ = 96;
    HFONT titleFont_ = nullptr;
    HFONT font_ = nullptr;
};

bool MainWindow::Register(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    return RegisterClassExW(&wc) != 0;
}

HWND MainWindow::Create(HINSTANCE instance, const std::wstring& folder) {
    auto* self = new MainWindow();
    self->instance_ = instance;
    self->path_ = folder;
    HWND hwnd = CreateWindowExW(0, kWindowClass, Text(IDS_APP_TITLE).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 900, 640, nullptr, nullptr, instance, self);
    if (!hwnd) {
        delete self;
        return nullptr;
    }
    // Initial size in DIPs on the window's monitor.
    UINT dpi = GetDpiForWindow(hwnd);
    SetWindowPos(hwnd, nullptr, 0, 0, MulDiv(960, dpi, 96), MulDiv(680, dpi, 96), SWP_NOMOVE | SWP_NOZORDER);
    return hwnd;
}

LRESULT CALLBACK MainWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    LRESULT r = self->Handle(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete self;
    }
    return r;
}

void MainWindow::OnCreate() {
    dpi_ = GetDpiForWindow(hwnd_);
    UpdateFonts();
    list_ = SizeList::Create(hwnd_, instance_);
    if (list_) list_->SetDpi(dpi_);
    source_ = std::make_unique<FolderSource>(hwnd_, kFolderLoaded);
    theme_.Watch(hwnd_, kThemeChanged);
    ApplyTheme();
    Navigate(path_, false);
}

void MainWindow::ApplyTheme() {
    palette_ = theme_.Current();
    ThemeWatcher::ApplyTitleBar(hwnd_, palette_.dark && !palette_.highContrast);
    if (list_) list_->SetPalette(palette_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::UpdateFonts() {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_)) return;
    if (font_) DeleteObject(font_);
    if (titleFont_) DeleteObject(titleFont_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW title = ncm.lfMessageFont;
    title.lfHeight = title.lfHeight * 6 / 5;
    title.lfWeight = FW_SEMIBOLD;
    titleFont_ = CreateFontIndirectW(&title);
}

void MainWindow::Layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (list_) {
        MoveWindow(list_->Window(), 0, HeaderHeight(), rc.right, std::max(0L, rc.bottom - HeaderHeight()), TRUE);
    }
}

void MainWindow::PaintHeader(HDC dc, const RECT& client) {
    RECT header{0, 0, client.right, HeaderHeight()};
    SetDCBrushColor(dc, palette_.background);
    FillRect(dc, &header, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    const int pad = Scale(12);
    RECT line1{pad, Scale(6), client.right - pad, Scale(32)};
    HGDIOBJ old = SelectObject(dc, titleFont_);
    SetTextColor(dc, palette_.text);
    DrawTextW(dc, path_.c_str(), -1, &line1, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, font_);
    RECT line2{pad, Scale(32), client.right - pad, Scale(54)};
    SetTextColor(dc, palette_.secondaryText);
    std::wstring hint = Text(IDS_HINT);
    SIZE hs{};
    GetTextExtentPoint32W(dc, hint.c_str(), static_cast<int>(hint.size()), &hs);
    RECT hintRect = line2;
    if (hs.cx < (line2.right - line2.left) / 2) {
        DrawTextW(dc, hint.c_str(), -1, &hintRect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        line2.right -= hs.cx + pad;
    }
    DrawTextW(dc, status_.c_str(), -1, &line2, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

void MainWindow::Navigate(const std::wstring& path, bool refresh) {
    path_ = Normalize(path);
    SetWindowTextW(hwnd_, (path_ + L" - " + Text(IDS_APP_TITLE)).c_str());
    status_ = Text(IDS_LOADING);
    if (list_ && !refresh) {
        list_->SetItems({}, 0, false);
        list_->SetMessage(Text(IDS_LOADING));
    }
    request_ = source_->Request(path_, true);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnLoaded(std::unique_ptr<FolderResult> r) {
    if (r->request != request_ || !list_) return;  // an older request
    switch (r->status) {
        case LoadStatus::NotFound:
            list_->SetItems({}, 0, false);
            list_->SetMessage(Text(IDS_ERR_NOT_FOUND));
            status_.clear();
            break;
        case LoadStatus::AccessDenied:
            list_->SetItems({}, 0, false);
            list_->SetMessage(Text(IDS_ERR_ACCESS));
            status_.clear();
            break;
        case LoadStatus::Failed:
            list_->SetItems({}, 0, false);
            list_->SetMessage(Text(IDS_ERR_FAILED) + std::to_wstring(r->error));
            status_.clear();
            break;
        case LoadStatus::Ok: {
            const bool keep = true;  // a refresh of the same folder keeps the position
            list_->SetMessage(L"");
            list_->SetItems(std::move(r->items), r->totalAllocated, keep);
            status_ = Text(IDS_TOTAL) + FormatBytes(r->totalLogical) + L"  \u00B7  " + Text(IDS_COL_ALLOCATED) + L" " +
                      FormatBytes(r->totalAllocated) + L"  \u00B7  " + FormatCount(r->totalFiles) +
                      Text(IDS_FILES_SUFFIX) + L"  \u00B7  " + Text(r->fromService ? IDS_SRC_SERVICE : IDS_SRC_WALK);
            std::wstring note = NoteText(r->note);
            if (!note.empty()) status_ += L"  \u00B7  " + note;
            break;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::PickFolder() {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(Text(IDS_PICK_FOLDER).c_str());
    ComPtr<IShellItem> current;
    if (SUCCEEDED(SHCreateItemFromParsingName(path_.c_str(), nullptr, IID_PPV_ARGS(&current)))) {
        dialog->SetFolder(current.Get());
    }
    if (FAILED(dialog->Show(hwnd_))) return;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        Navigate(path, false);
        CoTaskMemFree(path);
    }
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            OnCreate();
            return 0;
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
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_LBUTTONUP:
            if (GET_Y_LPARAM(lp) < Scale(32)) PickFolder();  // click on the path
            return 0;
        case WM_SETFOCUS:
            if (list_) SetFocus(list_->Window());
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kCmdRefresh: Navigate(path_, true); return 0;
                case kCmdPick: PickFolder(); return 0;
                case kCmdUp: Navigate(ParentOf(path_), false); return 0;
            }
            break;
        case kListGoUp:
            Navigate(ParentOf(path_), false);
            return 0;
        case kListOpenItem: {
            if (!list_) return 0;
            size_t index = wp;
            const auto& items = list_->Items();
            if (index < items.size() && items[index].isFolder) Navigate(Join(path_, items[index].name), false);
            return 0;
        }
        case kFolderLoaded:
            OnLoaded(std::unique_ptr<FolderResult>(reinterpret_cast<FolderResult*>(lp)));
            return 0;
        case kThemeChanged:
        case WM_SYSCOLORCHANGE:
            ApplyTheme();
            return 0;
        case WM_SETTINGCHANGE:
            ApplyTheme();  // high contrast and color changes
            break;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            UpdateFonts();
            if (list_) list_->SetDpi(dpi_);
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_DESTROY:
            source_.reset();
            if (font_) DeleteObject(font_);
            if (titleFont_) DeleteObject(titleFont_);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

std::wstring StartFolder() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring folder;
    if (argv && argc >= 2) folder = argv[1];
    if (argv) LocalFree(argv);
    if (folder.empty()) {
        wchar_t windows[MAX_PATH] = {};
        if (GetWindowsDirectoryW(windows, MAX_PATH) >= 3) folder.assign(windows, 3);  // the system drive
        else folder = L"C:\\";
    }
    return folder;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // Single-threaded apartment: UISettings (Windows Runtime) and the folder
    // picker (COM) are used on this thread.
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    if (wcsstr(GetCommandLineW(), L"--panel")) return RunPanel(instance);
    if (!MainWindow::Register(instance)) return 1;
    HWND hwnd = MainWindow::Create(instance, StartFolder());
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);

    ACCEL keys[] = {
        {FVIRTKEY, VK_F5, kCmdRefresh},
        {FVIRTKEY | FCONTROL, 'O', kCmdPick},
        {FVIRTKEY | FALT, VK_UP, kCmdUp},
    };
    HACCEL accel = CreateAcceleratorTableW(keys, static_cast<int>(std::size(keys)));
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (accel && TranslateAcceleratorW(hwnd, accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (accel) DestroyAcceleratorTable(accel);
    return 0;
}
