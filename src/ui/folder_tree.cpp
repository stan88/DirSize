#include "ui/folder_tree.h"

#include <windowsx.h>

#include <algorithm>

#include "ui/icon_key.h"
#include "ui/shell_icons.h"
#include "ui/strings.h"

namespace dirsize::ui {

namespace {

constexpr wchar_t kClassName[] = L"DirSizeFolderTree";
constexpr UINT kListingDone = WM_APP + 220;
constexpr UINT kDrivesDone = WM_APP + 221;
constexpr UINT_PTR kKeyTimer = 1;
constexpr UINT kKeyDelayMs = 250;  // keyboard selection settles before the list loads

void Fill(HDC dc, const RECT& r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void DrawLabel(HDC dc, const std::wstring& s, RECT r, UINT format, COLORREF color) {
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

bool RegisterOnce(HINSTANCE instance, WNDPROC proc) {
    static bool done = false;
    if (done) return true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    done = RegisterClassExW(&wc) != 0;
    return done;
}

}  // namespace

FolderTree* FolderTree::Create(HWND parent, HINSTANCE instance) {
    if (!RegisterOnce(instance, Proc)) return nullptr;
    auto* tree = new FolderTree();
    tree->parent_ = parent;
    HWND hwnd = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
                                parent, nullptr, instance, tree);
    if (!hwnd) {
        delete tree;
        return nullptr;
    }
    return tree;
}

FolderTree::~FolderTree() {
    if (font_) DeleteObject(font_);
}

void FolderTree::SetPalette(const Palette& palette) {
    palette_ = palette;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void FolderTree::SetDpi(UINT dpi) {
    dpi_ = dpi ? dpi : 96;
    scroll_.SetDpi(dpi_);
    UpdateMetrics();
    AfterChange();
}

void FolderTree::Reveal(const std::wstring& path) {
    KillTimer(hwnd_, kKeyTimer);  // a pending keyboard selection is replaced
    Load(model_.Reveal(path));
    AfterChange();
}

void FolderTree::Refresh(const std::wstring& path) {
    if (loader_) loader_->ListDrives();
    Load(model_.Refresh(path));
    AfterChange();
}

void FolderTree::Load(const std::vector<std::wstring>& paths) {
    if (!loader_) return;
    for (const std::wstring& p : paths) loader_->List(p);
}

void FolderTree::Apply(const TreeAction& action, bool fromKeyboard) {
    Load(action.loads);
    AfterChange();
    if (!action.selectionChanged) return;
    EnsureVisible(model_.RowOf(model_.Selected()));
    if (fromKeyboard) {
        SetTimer(hwnd_, kKeyTimer, kKeyDelayMs, nullptr);
    } else {
        KillTimer(hwnd_, kKeyTimer);
        PostMessageW(parent_, kTreeSelect, 0, 0);
    }
}

void FolderTree::AfterChange() {
    UpdateScroll();
    if (model_.TakeScrollRequest()) {
        // A reveal: a row out of view is brought to the middle, so the
        // folders around it show too.
        const int row = model_.RowOf(model_.Selected());
        const int page = VisibleRows();
        if (row >= 0 && (row < top_ || row >= top_ + page)) ScrollTo(row - page / 2);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void FolderTree::UpdateMetrics() {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_)) {
        if (font_) DeleteObject(font_);
        font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    rowHeight_ = tm.tmHeight + Scale(10);  // as SizeList
}

int FolderTree::VisibleRows() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    return std::max(1, static_cast<int>(rc.bottom) / std::max(1, rowHeight_));
}

void FolderTree::UpdateScroll() {
    const int count = static_cast<int>(model_.Rows().size());
    const int page = VisibleRows();
    top_ = std::clamp(top_, 0, std::max(0, count - page));
    RECT rc;
    GetClientRect(hwnd_, &rc);
    scroll_.SetRect(RECT{rc.right - scroll_.Width(), rc.top, rc.right, rc.bottom});
    scroll_.SetRange(count, page, top_);
}

void FolderTree::ScrollTo(int top) {
    int old = top_;
    top_ = top;
    UpdateScroll();
    if (top_ != old) InvalidateRect(hwnd_, nullptr, FALSE);
}

void FolderTree::EnsureVisible(int row) {
    if (row < 0) return;
    const int page = VisibleRows();
    if (row < top_) ScrollTo(row);
    else if (row >= top_ + page) ScrollTo(row - page + 1);
}

int FolderTree::HitRow(int y) {
    if (y < 0) return -1;
    int index = top_ + y / std::max(1, rowHeight_);
    return index < static_cast<int>(model_.Rows().size()) ? index : -1;
}

bool FolderTree::HitExpander(int row, int x) {
    const TreeRow& r = model_.Rows()[row];
    const int left = Indent(r.depth);
    return r.node->HasExpander() && x >= left && x < left + Scale(16);
}

void FolderTree::PaintRow(HDC dc, const TreeRow& row, const RECT& r, bool selected) {
    if (selected) {
        Fill(dc, r, palette_.selection);
        if (focused_) {
            // Focus outline, so Tab between the tree and the list shows
            // where the keys go (also visible in high contrast).
            HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(1)), palette_.selectionText);
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
    }
    const COLORREF text = selected ? palette_.selectionText : palette_.text;
    const COLORREF secondary = selected ? palette_.selectionText : palette_.secondaryText;
    const TreeNode* node = row.node;
    int x = r.left + Indent(row.depth);
    const int cy = (r.top + r.bottom) / 2;

    HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(1)), secondary);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (node->HasExpander()) {
        // A chevron: ">" collapsed, "v" expanded, drawn in the palette's
        // color (no images, as the high contrast guidance asks).
        const int c = x + Scale(8), h = Scale(3);
        POINT pts[3];
        if (node->expanded) {
            pts[0] = {c - h * 4 / 3, cy - h / 2};
            pts[1] = {c, cy + h * 2 / 3};
            pts[2] = {c + h * 4 / 3 + 1, cy - h / 2 - 1};
        } else {
            pts[0] = {c - h / 2, cy - h * 4 / 3};
            pts[1] = {c + h * 2 / 3, cy};
            pts[2] = {c - h / 2 - 1, cy + h * 4 / 3 + 1};
        }
        Polyline(dc, pts, 3);
    }
    x += Scale(18);
    // The system icon (shell_icons.h); outlines in high contrast or when
    // there is none.
    const int is = Scale(16);
    const bool system = !palette_.highContrast &&
                        DrawShellIcon(dc, node->parent ? IconKey(node->name, true) : DriveIconKey(node->name), x,
                                      cy - is / 2, is);
    if (system) {
    } else if (node->parent) {
        // Folder outline, as in SizeList.
        const int iw = Scale(14), ih = Scale(11), iy = cy - ih / 2;
        Rectangle(dc, x, iy + Scale(2), x + iw, iy + ih);
        Rectangle(dc, x, iy, x + iw / 2, iy + Scale(3));
    } else {
        // Drive outline: a flat box with a light.
        const int iw = Scale(15), ih = Scale(8), iy = cy - ih / 2;
        Rectangle(dc, x, iy, x + iw, iy + ih);
        Rectangle(dc, x + iw - Scale(5), iy + ih / 2 - Scale(1), x + iw - Scale(3), iy + ih / 2 + Scale(1));
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    x += Scale(22);

    RECT tr{x, r.top, r.right - Scale(6), r.bottom};
    const std::wstring& label = node->label.empty() ? node->name : node->label;
    if (node->state == ChildState::Loading && node->expanded) {
        std::wstring loading = L"  " + ui::Text(IDS_LOADING);
        SIZE ls{};
        GetTextExtentPoint32W(dc, loading.c_str(), static_cast<int>(loading.size()), &ls);
        SIZE ns{};
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &ns);
        RECT lr = tr;
        lr.left = std::min(tr.right, tr.left + ns.cx);
        if (lr.right - lr.left >= ls.cx) {
            DrawLabel(dc, loading, lr, DT_LEFT, secondary);
            tr.right = lr.left;
        }
    }
    DrawLabel(dc, label, tr, DT_LEFT | DT_END_ELLIPSIS, text);
}

void FolderTree::Paint(HDC dc, const RECT& fullClient) {
    Fill(dc, fullClient, palette_.background);
    RECT client = fullClient;  // rows stop at the scroll bar
    if (scroll_.Visible()) client.right = std::max(client.left, client.right - scroll_.Width());
    HGDIOBJ oldFont = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    const auto& rows = model_.Rows();
    const TreeNode* selected = model_.Selected();
    const int page = VisibleRows() + 1;
    for (int i = top_; i < static_cast<int>(rows.size()) && i < top_ + page; ++i) {
        int y = (i - top_) * rowHeight_;
        RECT r{client.left, y, client.right, y + rowHeight_};
        PaintRow(dc, rows[i], r, rows[i].node == selected);
    }
    scroll_.Paint(dc, palette_);
    SelectObject(dc, oldFont);
}

LRESULT CALLBACK FolderTree::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<FolderTree*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<FolderTree*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
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

LRESULT FolderTree::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            dpi_ = GetDpiForWindow(hwnd_);
            scroll_.SetDpi(dpi_);
            UpdateMetrics();
            loader_ =std::make_unique<TreeLoader>(hwnd_, kListingDone, kDrivesDone);
            loader_->ListDrives();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kKeyTimer);
            loader_.reset();
            return 0;
        case kListingDone: {
            std::unique_ptr<TreeListing> r(reinterpret_cast<TreeListing*>(lp));
            Load(r->ok ? model_.SetChildren(r->path, std::move(r->names)) : model_.SetFailed(r->path));
            AfterChange();
            return 0;
        }
        case kDrivesDone: {
            std::unique_ptr<DriveList> r(reinterpret_cast<DriveList*>(lp));
            Load(model_.SetRoots(r->roots));
            AfterChange();
            return 0;
        }
        case WM_TIMER: {
            if (wp == kKeyTimer) {
                KillTimer(hwnd_, kKeyTimer);
                PostMessageW(parent_, kTreeSelect, 0, 0);
                return 0;
            }
            int pos;
            if (scroll_.Timer(hwnd_, wp, pos)) {
                if (pos >= 0) ScrollTo(pos);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            int pos;
            if (scroll_.MouseMove(hwnd_, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, pos) && pos >= 0) ScrollTo(pos);
            return 0;
        }
        case WM_LBUTTONUP:
            scroll_.ButtonUp(hwnd_);
            return 0;
        case WM_CAPTURECHANGED:
            scroll_.CaptureLost(hwnd_);
            return 0;
        case WM_MOUSELEAVE:
            scroll_.MouseLeave(hwnd_);
            return 0;
        case WM_SIZE:
            UpdateScroll();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rc;
            GetClientRect(hwnd_, &rc);
            if (rc.right > rc.left && rc.bottom > rc.top) {
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                HGDIOBJ old = SelectObject(mem, bmp);
                Paint(mem, rc);
                BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            if (lines == WHEEL_PAGESCROLL) lines = static_cast<UINT>(VisibleRows());
            wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(wp);
            int steps = wheelRemainder_ / WHEEL_DELTA;
            wheelRemainder_ -= steps * WHEEL_DELTA;
            if (steps) ScrollTo(top_ - steps * static_cast<int>(lines));
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            int pos;
            if (scroll_.ButtonDown(hwnd_, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, pos)) {
                if (pos >= 0) ScrollTo(pos);
                return 0;
            }
            int row = HitRow(GET_Y_LPARAM(lp));
            if (row < 0) return 0;
            TreeNode* node = model_.Rows()[row].node;
            if (HitExpander(row, GET_X_LPARAM(lp))) Apply(model_.Toggle(node), false);
            else Apply(model_.Click(node), false);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            int pos;
            if (scroll_.ButtonDown(hwnd_, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, pos)) {
                if (pos >= 0) ScrollTo(pos);  // the second click of a fast pair
                return 0;
            }
            int row = HitRow(GET_Y_LPARAM(lp));
            if (row < 0) return 0;
            // On the label: expand or collapse (the first click selected).
            // On the expander: this is the second click of a fast pair,
            // which arrives instead of WM_LBUTTONDOWN, so it toggles again.
            Apply(model_.Toggle(model_.Rows()[row].node), false);
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN: {
            TreeKey key;
            switch (wp) {
                case VK_UP: key = TreeKey::Up; break;
                case VK_DOWN: key = TreeKey::Down; break;
                case VK_LEFT: key = TreeKey::Left; break;
                case VK_RIGHT: key = TreeKey::Right; break;
                case VK_HOME: key = TreeKey::Home; break;
                case VK_END: key = TreeKey::End; break;
                case VK_PRIOR: key = TreeKey::PageUp; break;
                case VK_NEXT: key = TreeKey::PageDown; break;
                case VK_RETURN:
                    // Load the list now instead of after the pause.
                    if (model_.Selected()) {
                        KillTimer(hwnd_, kKeyTimer);
                        PostMessageW(parent_, kTreeSelect, 0, 0);
                    }
                    return 0;
                default: return DefWindowProcW(hwnd_, msg, wp, lp);
            }
            Apply(model_.Key(key, VisibleRows()), true);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            focused_ = msg == WM_SETFOCUS;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace dirsize::ui
