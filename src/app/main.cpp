// DirSize front end (Phase 5): the standalone window (a folder tree on the
// left, the selected folder's subfolders and files with their sizes on the
// right, following the system theme) and, with --panel, the Explorer panel
// (panel.cpp). docs/DESIGN-phase5.md sections 3 and 4.
//
//   dirsize.exe [folder]
//   dirsize.exe --panel [--trace]
//   dirsize.exe --check <folder> <output file>   (verification scripts)
//   dirsize.exe --setting <autostart|share> <on|off>   (installer, per user)

#include <windows.h>
#include <unknwn.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <winrt/base.h>
#include <wrl/client.h>

#include <shlobj.h>

#include <cstdio>
#include <memory>
#include <share.h>
#include <string>

#include "app/panel.h"
#include "ui/folder_source.h"
#include "ui/folder_tree.h"
#include "ui/format.h"
#include "ui/language.h"
#include "ui/settings.h"
#include "ui/shell_verb.h"
#include "ui/size_list.h"
#include "ui/strings.h"
#include "ui/theme.h"
#include "ui/verb_command.h"

using Microsoft::WRL::ComPtr;
using namespace dirsize::ui;

namespace {

constexpr wchar_t kWindowClass[] = L"DirSizeMainWindow";
constexpr WORD kAppIcon = 1;  // the ICON resource in dirsize.rc
constexpr UINT kThemeChanged = WM_APP + 1;
constexpr UINT kFolderLoaded = WM_APP + 2;
constexpr WORD kCmdRefresh = 1;
constexpr WORD kCmdPick = 2;
constexpr WORD kCmdUp = 3;
constexpr WORD kCmdNextPane = 4;  // Tab, Shift+Tab
// Popup menu items (TrackPopupMenu with TPM_RETURNCMD).
constexpr UINT kMenuVerb = 100;
constexpr UINT kMenuPick = 101;
constexpr UINT kMenuOpen = 102;
constexpr UINT kMenuExplore = 103;
constexpr UINT kMenuRefresh = 104;
constexpr UINT kMenuAutostart = 105;
constexpr UINT kMenuShowShare = 106;
constexpr UINT kMenuLangAuto = 199;  // then kMenuLangFirst + index into Languages()
constexpr UINT kMenuLangFirst = 200;
constexpr int kTreeWidthDip = 260;
constexpr int kSplitterDip = 6;

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
    void Navigate(const std::wstring& path, bool refresh, bool fromTree = false);
    void OnTreeSelect();
    void NextPane();
    RECT SplitterRect() const;
    void SetTreeWidth(int x);
    void OnLoaded(std::unique_ptr<FolderResult> r);
    void PickFolder();
    RECT MoreButtonRect() const;  // the "..." button at the right of the path
    void ShowSettingsMenu();
    void ShowListMenu(int index, POINT screen);
    void SetMoreHover(bool hover);
    int Scale(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }
    int HeaderHeight() const { return Scale(40); }  // the path and the "..." button
    int StatusHeight() const { return Scale(28); }  // the status bar at the bottom
    RECT StatusRect() const;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    SizeList* list_ = nullptr;
    FolderTree* tree_ = nullptr;
    HWND lastFocus_ = nullptr;  // the pane that gets the focus back on activation
    int treeWidthDip_ = kTreeWidthDip;
    bool dragging_ = false;     // moving the splitter
    bool moreHover_ = false;    // mouse over the "..." button
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
    // The icon from dirsize.rc at the system's large and small sizes. The
    // class lives as long as the process, so the icons are not destroyed.
    // On failure the handles stay null and Windows uses its default icon.
    // Not LoadIconMetric: it is imported from comctl32 by ordinal 380, which
    // only version 6 has, and this program does not ask for version 6 in its
    // manifest; the program then failed to start (DEVLOG 2026-11-12).
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kAppIcon), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kAppIcon), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
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
    tree_ = FolderTree::Create(hwnd_, instance_);
    if (tree_) tree_->SetDpi(dpi_);
    list_ = SizeList::Create(hwnd_, instance_);
    if (list_) {
        list_->SetColumns(WindowColumns(ShowShareColumn()));
        list_->SetColumnWidths(ParseColumnWidths(ColumnWidthsSetting(false)));
        list_->SetDpi(dpi_);
    }
    source_ = std::make_unique<FolderSource>(hwnd_, kFolderLoaded);
    theme_.Watch(hwnd_, kThemeChanged);
    ApplyTheme();
    Navigate(path_, false);
}

void MainWindow::ApplyTheme() {
    palette_ = theme_.Current();
    ThemeWatcher::ApplyTitleBar(hwnd_, palette_.dark && !palette_.highContrast);
    if (list_) list_->SetPalette(palette_);
    if (tree_) tree_->SetPalette(palette_);
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

RECT MainWindow::SplitterRect() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    // The tree keeps at least 120 DIPs, the list at least 240 when possible.
    int width = std::min(Scale(treeWidthDip_), static_cast<int>(rc.right) - Scale(240 + kSplitterDip));
    width = std::max(width, Scale(120));
    return RECT{width, HeaderHeight(), width + Scale(kSplitterDip), StatusRect().top};
}

RECT MainWindow::StatusRect() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int top = std::max(static_cast<int>(rc.top) + HeaderHeight(), static_cast<int>(rc.bottom) - StatusHeight());
    return RECT{rc.left, top, rc.right, rc.bottom};
}

void MainWindow::SetTreeWidth(int x) {
    treeWidthDip_ = std::max(120, MulDiv(x, 96, static_cast<int>(dpi_)));
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::Layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const RECT split = SplitterRect();
    const int height = std::max(0, static_cast<int>(StatusRect().top) - HeaderHeight());
    if (tree_) MoveWindow(tree_->Window(), 0, HeaderHeight(), split.left, height, TRUE);
    if (list_) {
        MoveWindow(list_->Window(), split.right, HeaderHeight(), std::max(0, static_cast<int>(rc.right - split.right)),
                   height, TRUE);
    }
}

void MainWindow::PaintHeader(HDC dc, const RECT& client) {
    RECT header{0, 0, client.right, HeaderHeight()};
    SetDCBrushColor(dc, palette_.background);
    FillRect(dc, &header, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    const int pad = Scale(12);
    const RECT more = MoreButtonRect();
    RECT line1{pad, Scale(6), more.left - pad, Scale(32)};
    HGDIOBJ old = SelectObject(dc, titleFont_);
    SetTextColor(dc, palette_.text);
    DrawTextW(dc, path_.c_str(), -1, &line1, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);

    // The "..." button (settings menu): drawn with the palette, a hover
    // background, and an outline in high contrast so it reads as a button.
    if (moreHover_) {
        SetDCBrushColor(dc, palette_.highContrast ? palette_.selection : palette_.headerBackground);
        FillRect(dc, &more, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    if (palette_.highContrast) {
        HPEN pen = CreatePen(PS_SOLID, 1, palette_.text);
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, more.left, more.top, more.right, more.bottom);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    SetTextColor(dc, moreHover_ && palette_.highContrast ? palette_.selectionText : palette_.text);
    RECT moreText = more;
    DrawTextW(dc, L"⋯", -1, &moreText, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    // The status bar at the bottom (the user asked for it there): the
    // totals and the source on the left, the key hint on the right when
    // there is room. Header background with a separator line on top.
    SelectObject(dc, font_);
    RECT bar = StatusRect();
    SetDCBrushColor(dc, palette_.headerBackground);
    FillRect(dc, &bar, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    RECT sep{bar.left, bar.top, bar.right, bar.top + std::max(1, Scale(1))};
    SetDCBrushColor(dc, palette_.line);
    FillRect(dc, &sep, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    RECT line2{pad, bar.top + std::max(1, Scale(1)), client.right - pad, bar.bottom};
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

    // The splitter: background with a one-pixel line in the middle.
    RECT split = SplitterRect();
    SetDCBrushColor(dc, palette_.background);
    FillRect(dc, &split, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const int mid = (split.left + split.right) / 2;
    RECT line{mid, split.top, mid + std::max(1, Scale(1)), split.bottom};
    SetDCBrushColor(dc, palette_.line);
    FillRect(dc, &line, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

RECT MainWindow::MoreButtonRect() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int size = Scale(30), pad = Scale(8);
    return RECT{rc.right - pad - size, Scale(4), rc.right - pad, Scale(4) + size};
}

void MainWindow::SetMoreHover(bool hover) {
    if (hover == moreHover_) return;
    moreHover_ = hover;
    if (hover) {
        TRACKMOUSEEVENT tme{sizeof(tme)};
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        TrackMouseEvent(&tme);
    }
    RECT r = MoreButtonRect();
    InvalidateRect(hwnd_, &r, FALSE);
}

// The settings menu of the "..." button. System popup menus stay light in
// dark mode (only an undocumented way changes that); accepted for menus.
void MainWindow::ShowSettingsMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const bool installed = IsShellVerbInstalled();
    // Registered for all users by the installer: shown, but not switched here.
    const bool byInstaller = IsShellVerbSetByInstaller();
    if (byInstaller) {
        AppendMenuW(menu, MF_STRING | MF_CHECKED | MF_GRAYED, kMenuVerb, Text(IDS_MENU_VERB_INSTALLER).c_str());
    } else {
        AppendMenuW(menu, MF_STRING | (installed ? MF_CHECKED : MF_UNCHECKED), kMenuVerb, Text(IDS_MENU_VERB).c_str());
    }
    const bool autostart = IsPanelAutostart();
    AppendMenuW(menu, MF_STRING | (autostart ? MF_CHECKED : MF_UNCHECKED), kMenuAutostart,
                Text(IDS_MENU_AUTOSTART).c_str());
    const bool showShare = ShowShareColumn();
    AppendMenuW(menu, MF_STRING | (showShare ? MF_CHECKED : MF_UNCHECKED), kMenuShowShare,
                Text(IDS_MENU_SHOW_SHARE).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuPick, Text(IDS_MENU_PICK).c_str());
    // Language: follow Windows, or one of the built-in tables.
    const std::wstring saved = SavedLanguage();
    const auto languages = Languages();
    const size_t savedIndex = FindLanguage(saved, languages);
    HMENU langMenu = CreatePopupMenu();
    if (langMenu) {
        AppendMenuW(langMenu, MF_STRING | (savedIndex >= languages.size() ? MF_CHECKED : MF_UNCHECKED), kMenuLangAuto,
                    Text(IDS_MENU_LANG_AUTO).c_str());
        AppendMenuW(langMenu, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < languages.size(); ++i) {
            AppendMenuW(langMenu, MF_STRING | (i == savedIndex ? MF_CHECKED : MF_UNCHECKED),
                        kMenuLangFirst + static_cast<UINT>(i), LanguageDisplayName(languages[i].locale).c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(langMenu), Text(IDS_MENU_LANGUAGE).c_str());
    }
    RECT more = MoreButtonRect();
    POINT pt{more.right, more.bottom};
    ClientToScreen(hwnd_, &pt);
    UINT cmd = static_cast<UINT>(
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_,
                       nullptr));
    DestroyMenu(menu);
    SetMoreHover(false);
    if (cmd == kMenuAutostart) {
        DWORD error = SetPanelAutostart(!autostart);
        if (error != ERROR_SUCCESS) {
            status_ = Format(IDS_ERR_SETTING, {std::to_wstring(error)});
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    } else if (cmd == kMenuShowShare) {
        DWORD error = SetShowShareColumn(!showShare);
        if (error != ERROR_SUCCESS) {
            status_ = Format(IDS_ERR_SETTING, {std::to_wstring(error)});
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            NotifySettingsChanged();  // this window and an open panel follow
        }
    } else if (cmd == kMenuVerb && !byInstaller) {
        DWORD error = installed ? RemoveShellVerb() : InstallShellVerb();
        if (error != ERROR_SUCCESS) {
            status_ = Format(IDS_ERR_VERB, {std::to_wstring(error)});
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    } else if (cmd == kMenuPick) {
        PickFolder();
    } else if (cmd == kMenuLangAuto || (cmd >= kMenuLangFirst && cmd < kMenuLangFirst + languages.size())) {
        std::wstring choice = cmd == kMenuLangAuto ? L"" : languages[cmd - kMenuLangFirst].locale;
        if (choice != saved) {
            DWORD error = SaveLanguage(choice);
            if (error != ERROR_SUCCESS) {
                status_ = Format(IDS_ERR_SETTING, {std::to_wstring(error)});
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            // Switch now: most text is loaded when painted; the title, the
            // status line, the list message and the drive labels are rebuilt
            // by loading the folder and the drives again.
            ApplyUiLanguage();
            Navigate(path_, true);
        }
    }
}

// Right-click on the list: open / show in Explorer / refresh.
void MainWindow::ShowListMenu(int index, POINT screen) {
    if (!list_) return;
    const auto& items = list_->Items();
    const ListItem* item = index >= 0 && index < static_cast<int>(items.size()) ? &items[index] : nullptr;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    if (item && item->isFolder) {
        AppendMenuW(menu, MF_STRING, kMenuOpen, Text(IDS_MENU_OPEN).c_str());
        SetMenuDefaultItem(menu, kMenuOpen, FALSE);  // as double-click
        AppendMenuW(menu, MF_STRING, kMenuExplore, Text(IDS_MENU_EXPLORE).c_str());
    } else if (item) {
        AppendMenuW(menu, MF_STRING, kMenuExplore, Text(IDS_MENU_SHOW_FILE).c_str());
    } else {
        AppendMenuW(menu, MF_STRING, kMenuExplore, Text(IDS_MENU_EXPLORE).c_str());  // the current folder
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuRefresh, Text(IDS_MENU_REFRESH).c_str());
    // Copied before the menu runs: its modal loop dispatches messages, and
    // a load finishing then replaces the list's items.
    const std::wstring target = item ? Join(path_, item->name) : path_;
    const bool targetIsFolder = !item || item->isFolder;
    item = nullptr;
    UINT cmd = static_cast<UINT>(
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr));
    DestroyMenu(menu);
    switch (cmd) {
        case kMenuOpen:
            Navigate(target, false);
            break;
        case kMenuExplore:
            if (targetIsFolder) {
                ShellExecuteW(hwnd_, L"explore", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(target.c_str())) {
                // Opens the folder with the file selected.
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
            break;
        case kMenuRefresh:
            Navigate(path_, true);
            break;
    }
}

void MainWindow::Navigate(const std::wstring& path, bool refresh, bool fromTree) {
    path_ = Normalize(path);
    // The tree follows the list; a selection made in the tree is already
    // there. Reveal never reports back, so this does not loop.
    if (tree_ && !fromTree) {
        if (refresh) tree_->Refresh(path_);
        tree_->Reveal(path_);
    }
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
    if (r->walking) {
        // Interim: the service did not answer and a walk is under way. Say
        // why now (design section 2); the rows stay on a refresh.
        std::wstring note = NoteText(r->note);
        status_ = note.empty() ? Text(IDS_WALKING) : note + L"  ·  " + Text(IDS_WALKING);
        list_->SetMessage(Text(IDS_WALKING));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
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
            list_->SetMessage(Format(IDS_ERR_FAILED, {std::to_wstring(r->error)}));
            status_.clear();
            break;
        case LoadStatus::Ok: {
            const bool keep = true;  // a refresh of the same folder keeps the position
            list_->SetMessage(L"");
            list_->SetItems(std::move(r->items), r->totalAllocated, keep);
            status_ = Format(IDS_TOTAL, {FormatBytes(r->totalLogical)}) + L"  \u00B7  " +
                      Format(IDS_STATUS_ALLOCATED, {FormatBytes(r->totalAllocated)}) + L"  \u00B7  " +
                      Format(IDS_FILE_COUNT, {FormatCount(r->totalFiles)}) + L"  \u00B7  " +
                      Text(r->fromService ? IDS_SRC_SERVICE : IDS_SRC_WALK);
            std::wstring note = NoteText(r->note);
            if (!note.empty()) status_ += L"  \u00B7  " + note;
            break;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnTreeSelect() {
    if (!tree_) return;
    std::wstring path = tree_->SelectedPath();
    if (path.empty()) return;
    path = Normalize(path);
    if (CompareStringOrdinal(path.c_str(), -1, path_.c_str(), -1, TRUE) == CSTR_EQUAL) return;
    Navigate(path, false, true);
}

void MainWindow::NextPane() {
    if (!tree_ || !list_) return;
    HWND next = GetFocus() == tree_->Window() ? list_->Window() : tree_->Window();
    SetFocus(next);
    lastFocus_ = next;
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
    // A registered message: not a constant, so not a case label.
    if (msg == SettingsChangedMessage() && msg != 0) {
        if (list_) list_->SetColumns(WindowColumns(ShowShareColumn()));
        return 0;
    }
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
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            RECT split = SplitterRect();
            if (LOWORD(lp) == HTCLIENT && PtInRect(&split, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT more = MoreButtonRect();
            if (PtInRect(&more, pt)) {
                ShowSettingsMenu();
                return 0;
            }
            RECT split = SplitterRect();
            if (PtInRect(&split, pt)) {
                dragging_ = true;
                SetCapture(hwnd_);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (dragging_) {
                SetTreeWidth(GET_X_LPARAM(lp) - Scale(kSplitterDip) / 2);
                return 0;
            }
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT more = MoreButtonRect();
            SetMoreHover(PtInRect(&more, pt) != FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            SetMoreHover(false);
            return 0;
        case WM_CAPTURECHANGED:
            dragging_ = false;
            return 0;
        case WM_LBUTTONUP: {
            if (dragging_) {
                ReleaseCapture();  // WM_CAPTURECHANGED clears dragging_
                return 0;
            }
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT more = MoreButtonRect();
            if (pt.y < Scale(32) && pt.x < more.left) PickFolder();  // click on the path
            return 0;
        }
        case kListColumnsResized:
            if (list_) SetColumnWidthsSetting(false, FormatColumnWidths(list_->ColumnWidths()));
            return 0;
        case kListContextMenu:
            ShowListMenu(static_cast<int>(wp), POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                HWND focus = GetFocus();
                if ((tree_ && focus == tree_->Window()) || (list_ && focus == list_->Window())) lastFocus_ = focus;
            }
            break;
        case WM_SETFOCUS:
            if (lastFocus_ && IsWindow(lastFocus_)) SetFocus(lastFocus_);
            else if (list_) SetFocus(list_->Window());
            return 0;
        case kTreeSelect:
            OnTreeSelect();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kCmdRefresh: Navigate(path_, true); return 0;
                case kCmdPick: PickFolder(); return 0;
                case kCmdUp: Navigate(ParentOf(path_), false); return 0;
                case kCmdNextPane: NextPane(); return 0;
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
            if (tree_) tree_->SetDpi(dpi_);
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
    // From the Explorer verb a drive arrives as C:" (verb_command.h).
    if (argv && argc >= 2) folder = PathFromArgument(argv[1]);
    if (argv) LocalFree(argv);
    if (folder.empty()) {
        wchar_t windows[MAX_PATH] = {};
        if (GetWindowsDirectoryW(windows, MAX_PATH) >= 3) folder.assign(windows, 3);  // the system drive
        else folder = L"C:\\";
    }
    return folder;
}

const wchar_t* NoteName(ServiceNote note) {
    switch (note) {
        case ServiceNote::None: return L"none";
        case ServiceNote::Updating: return L"updating";
        case ServiceNote::Starting: return L"starting";
        case ServiceNote::NotRunning: return L"not-running";
        case ServiceNote::NotInstalled: return L"not-installed";
        case ServiceNote::NotIndexed: return L"not-indexed";
        case ServiceNote::Indexing: return L"indexing";
        case ServiceNote::NotLocal: return L"not-local";
    }
    return L"?";
}

const wchar_t* StatusName(LoadStatus s) {
    switch (s) {
        case LoadStatus::Ok: return L"ok";
        case LoadStatus::NotFound: return L"not-found";
        case LoadStatus::AccessDenied: return L"access-denied";
        case LoadStatus::Failed: return L"failed";
    }
    return L"?";
}

// dirsize.exe --check <folder> <output file>
// For the verification scripts (scripts/frontend-test.ps1): loads the folder
// through the same FolderSource as the window, without a window, and
// appends one line to the output file (UTF-8) with what the status line
// would say: where the sizes came from and why. Exit code 0 when loaded.
int g_checkWalkNotices = 0;  // --check: interim "walking" notices seen

int RunCheck() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 4) {
        if (argv) LocalFree(argv);
        return 2;
    }
    const std::wstring folder = Normalize(PathFromArgument(argv[2]));
    const std::wstring output = argv[3];
    LocalFree(argv);

    // A message-only window receives the result (FolderSource posts it).
    struct Receiver {
        static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
            if (m == kFolderLoaded) {
                auto* slot = reinterpret_cast<std::unique_ptr<FolderResult>*>(GetWindowLongPtrW(h, GWLP_USERDATA));
                std::unique_ptr<FolderResult> r(reinterpret_cast<FolderResult*>(l));
                if (r->walking) {
                    g_checkWalkNotices++;  // interim notice; the result follows
                    return 0;
                }
                *slot = std::move(r);
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(h, m, w, l);
        }
    };
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Receiver::Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DirSizeCheck";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 3;
    std::unique_ptr<FolderResult> result;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&result));
    const ULONGLONG start = GetTickCount64();
    {
        FolderSource source(hwnd, kFolderLoaded);
        source.Request(folder, false);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    }
    const ULONGLONG elapsed = GetTickCount64() - start;
    DestroyWindow(hwnd);
    if (!result) return 4;

    wchar_t when[32] = {};
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(when, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring line = std::wstring(when) + L" status=" + StatusName(result->status) +
                        L" source=" + (result->fromService ? L"service" : L"walk") +
                        L" note=" + NoteName(result->note) + L" folders=" + std::to_wstring(result->items.size()) +
                        L" allocated=" + std::to_wstring(result->totalAllocated) +
                        L" files=" + std::to_wstring(result->totalFiles) + L" ms=" + std::to_wstring(elapsed) +
                        L" walk-notice=" + std::to_wstring(g_checkWalkNotices) +
                        (result->status == LoadStatus::Failed ? L" error=" + std::to_wstring(result->error) : L"") +
                        L"\n";
    FILE* f = _wfsopen(output.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    if (!f) return 5;
    fputws(line.c_str(), f);
    fclose(f);
    return result->status == LoadStatus::Ok ? 0 : 1;
}

// dirsize.exe --setting <autostart|share> <on|off>
// For the installer: sets one per-user setting (HKCU) for the user running
// this process and exits without a window. The installer runs it with the
// credentials of the user who started Setup. Returns -1 when the command
// line is not this form; otherwise the Win32 error (0 = done).
int RunSetting() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return -1;
    int result = -1;
    // The first argument exactly (not a substring search: a folder name may
    // contain the same text).
    if (argc == 4 && _wcsicmp(argv[1], L"--setting") == 0) {
        const bool on = _wcsicmp(argv[3], L"on") == 0;
        const bool off = _wcsicmp(argv[3], L"off") == 0;
        result = ERROR_INVALID_PARAMETER;
        if (on || off) {
            if (_wcsicmp(argv[2], L"autostart") == 0) result = static_cast<int>(SetPanelAutostart(on));
            else if (_wcsicmp(argv[2], L"share") == 0) result = static_cast<int>(SetShowShareColumn(on));
        }
    }
    LocalFree(argv);
    return result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // Single-threaded apartment: UISettings (Windows Runtime) and the folder
    // picker (COM) are used on this thread.
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    // The installer's per-user settings: no window.
    if (int setting = RunSetting(); setting >= 0) return setting;
    // Before any window or thread: worker threads load strings too.
    ApplyUiLanguage();
    if (wcsstr(GetCommandLineW(), L"--check")) return RunCheck();
    if (wcsstr(GetCommandLineW(), L"--panel")) return RunPanel(instance);
    if (!MainWindow::Register(instance)) return 1;
    HWND hwnd = MainWindow::Create(instance, StartFolder());
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);

    ACCEL keys[] = {
        {FVIRTKEY, VK_F5, kCmdRefresh},
        {FVIRTKEY | FCONTROL, 'O', kCmdPick},
        {FVIRTKEY | FALT, VK_UP, kCmdUp},
        {FVIRTKEY, VK_TAB, kCmdNextPane},
        {FVIRTKEY | FSHIFT, VK_TAB, kCmdNextPane},
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
