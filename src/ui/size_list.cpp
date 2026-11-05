#include "ui/size_list.h"

#include <windowsx.h>

#include <algorithm>

#include "ui/format.h"
#include "ui/strings.h"

namespace dirsize::ui {

namespace {

constexpr wchar_t kClassName[] = L"DirSizeSizeList";

// Column widths in DIPs; Name takes the rest.
int ColumnWidthDip(Column c, bool compact) {
    switch (c) {
        case Column::Size: return compact ? 76 : 90;
        case Column::Allocated: return compact ? 80 : 96;
        case Column::Share: return compact ? 112 : 150;
        case Column::Files: return compact ? 70 : 84;
        case Column::Name: break;
    }
    return 0;
}

UINT TitleId(Column c) {
    switch (c) {
        case Column::Name: return IDS_COL_NAME;
        case Column::Size: return IDS_COL_SIZE;
        case Column::Allocated: return IDS_COL_ALLOCATED;
        case Column::Share: return IDS_COL_SHARE;
        case Column::Files: return IDS_COL_FILES;
    }
    return IDS_COL_NAME;
}

void Fill(HDC dc, const RECT& r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Text(HDC dc, const std::wstring& s, RECT r, UINT format, COLORREF color) {
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

SizeList* SizeList::Create(HWND parent, HINSTANCE instance) {
    if (!RegisterOnce(instance, Proc)) return nullptr;
    auto* list = new SizeList();
    list->parent_ = parent;
    HWND hwnd = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
                                parent, nullptr, instance, list);
    if (!hwnd) {
        delete list;
        return nullptr;
    }
    return list;
}

SizeList::~SizeList() {
    if (font_) DeleteObject(font_);
}

void SizeList::SetColumns(const std::vector<Column>& columns) {
    columns_ = columns;
    if (std::find(columns_.begin(), columns_.end(), sortColumn_) == columns_.end()) {
        sortColumn_ = columns_.size() > 1 ? columns_[1] : Column::Name;
        sortDescending_ = DefaultDescending(sortColumn_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SizeList::SetItems(std::vector<ListItem> items, std::uint64_t shareTotal, bool keepPosition) {
    std::wstring selectedName = keepPosition && selected_ >= 0 && selected_ < static_cast<int>(items_.size())
                                    ? items_[selected_].name
                                    : std::wstring();
    int oldTop = top_;
    items_ = std::move(items);
    shareTotal_ = shareTotal;
    Sort();
    selected_ = -1;
    top_ = 0;
    if (keepPosition) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].name == selectedName) selected_ = static_cast<int>(i);
        }
        top_ = oldTop;
    }
    UpdateScroll();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SizeList::SetMessage(const std::wstring& message) {
    message_ = message;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SizeList::SetPalette(const Palette& palette) {
    palette_ = palette;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SizeList::SetDpi(UINT dpi) {
    dpi_ = dpi ? dpi : 96;
    scroll_.SetDpi(dpi_);
    UpdateMetrics();
    UpdateScroll();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SizeList::SetCompact(bool compact) {
    compact_ = compact;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SizeList::UpdateMetrics() {
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
    rowHeight_ = tm.tmHeight + Scale(10);
    headerHeight_ = tm.tmHeight + Scale(12);
}

int SizeList::VisibleRows() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int h = rc.bottom - headerHeight_;
    return h > 0 ? std::max(1, h / rowHeight_) : 1;
}

void SizeList::UpdateScroll() {
    const int count = static_cast<int>(items_.size());
    const int page = VisibleRows();
    top_ = std::clamp(top_, 0, std::max(0, count - page));
    RECT rc;
    GetClientRect(hwnd_, &rc);
    // The bar runs below the column header.
    scroll_.SetRect(RECT{rc.right - scroll_.Width(), headerHeight_, rc.right, rc.bottom});
    scroll_.SetRange(count, page, top_);
}

RECT SizeList::ContentRect() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (scroll_.Visible()) rc.right = std::max(rc.left, rc.right - scroll_.Width());
    return rc;
}

void SizeList::ScrollTo(int top) {
    int old = top_;
    top_ = top;
    UpdateScroll();
    if (top_ != old) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SizeList::Select(int index, bool ensureVisible) {
    const int count = static_cast<int>(items_.size());
    if (count == 0) return;
    index = std::clamp(index, 0, count - 1);
    selected_ = index;
    if (ensureVisible) {
        const int page = VisibleRows();
        if (index < top_) ScrollTo(index);
        else if (index >= top_ + page) ScrollTo(index - page + 1);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SizeList::Sort() {
    std::wstring selectedName =
        selected_ >= 0 && selected_ < static_cast<int>(items_.size()) ? items_[selected_].name : std::wstring();
    SortItems(items_, sortColumn_, sortDescending_);
    if (!selectedName.empty()) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].name == selectedName) selected_ = static_cast<int>(i);
        }
    }
}

void SizeList::ColumnRects(const RECT& client, std::vector<RECT>& out) const {
    out.assign(columns_.size(), RECT{});
    int fixed = 0;
    for (Column c : columns_) fixed += Scale(ColumnWidthDip(c, compact_));
    const int pad = Scale(8);
    int nameWidth = std::max(Scale(compact_ ? 60 : 120), static_cast<int>(client.right - client.left) - fixed - pad);
    int x = client.left + pad;
    for (size_t i = 0; i < columns_.size(); ++i) {
        int w = columns_[i] == Column::Name ? nameWidth : Scale(ColumnWidthDip(columns_[i], compact_));
        out[i] = RECT{x, client.top, x + w, client.bottom};
        x += w;
    }
}

int SizeList::HitRow(int y) const {
    if (y < headerHeight_) return -1;
    int index = top_ + (y - headerHeight_) / rowHeight_;
    return index < static_cast<int>(items_.size()) ? index : -1;
}

int SizeList::HitHeader(int x, int y) const {
    if (y >= headerHeight_) return -1;
    RECT rc = ContentRect();
    std::vector<RECT> cols;
    ColumnRects(rc, cols);
    for (size_t i = 0; i < cols.size(); ++i) {
        if (x >= cols[i].left && x < cols[i].right) return static_cast<int>(i);
    }
    return -1;
}

void SizeList::PaintRow(HDC dc, int index, const RECT& row) {
    const ListItem& item = items_[index];
    const bool selected = index == selected_;
    if (selected) Fill(dc, row, palette_.selection);
    const COLORREF text = selected ? palette_.selectionText : palette_.text;
    const COLORREF secondary = selected ? palette_.selectionText : palette_.secondaryText;
    const int gap = Scale(8);

    std::vector<RECT> cols;
    ColumnRects(row, cols);
    for (size_t c = 0; c < columns_.size(); ++c) {
        RECT r = cols[c];
        r.right -= gap;
        switch (columns_[c]) {
            case Column::Name: {
                // A small outline icon: folder (with a tab) or page.
                const int iw = Scale(14), ih = Scale(11);
                const int ix = r.left, iy = (row.top + row.bottom - ih) / 2;
                HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(1)), secondary);
                HGDIOBJ oldPen = SelectObject(dc, pen);
                HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                if (item.isFolder) {
                    Rectangle(dc, ix, iy + Scale(2), ix + iw, iy + ih);
                    Rectangle(dc, ix, iy, ix + iw / 2, iy + Scale(3));
                } else {
                    Rectangle(dc, ix + Scale(2), iy - Scale(1), ix + iw - Scale(2), iy + ih + Scale(1));
                }
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(pen);
                r.left += iw + Scale(8);
                std::wstring tag = item.accessible ? std::wstring() : L"  " + ui::Text(IDS_NO_ACCESS);
                if (!tag.empty()) {
                    SIZE ts{};
                    GetTextExtentPoint32W(dc, tag.c_str(), static_cast<int>(tag.size()), &ts);
                    RECT tr = r;
                    tr.left = std::max(r.left, r.right - ts.cx);
                    Text(dc, tag, tr, DT_RIGHT, selected ? text : palette_.warning);
                    r.right = tr.left;
                }
                Text(dc, item.name, r, DT_LEFT | DT_END_ELLIPSIS, text);
                break;
            }
            case Column::Size:
                Text(dc, FormatBytes(item.logicalBytes), r, DT_RIGHT, text);
                break;
            case Column::Allocated:
                Text(dc, FormatBytes(item.allocatedBytes), r, DT_RIGHT, text);
                break;
            case Column::Files:
                Text(dc, item.isFolder ? FormatCount(item.files) : std::wstring(), r, DT_RIGHT, secondary);
                break;
            case Column::Share: {
                // Bar and percentage side by side: no text drawn over the bar
                // (the high contrast guidance).
                const int textWidth = Scale(compact_ ? 44 : 52);
                RECT bar = r;
                bar.right = std::max(bar.left, r.right - textWidth - gap);
                const int bh = Scale(8);
                bar.top = (row.top + row.bottom - bh) / 2;
                bar.bottom = bar.top + bh;
                Fill(dc, bar, palette_.barTrack);
                if (shareTotal_ > 0 && bar.right > bar.left) {
                    double share = std::min(1.0, static_cast<double>(item.allocatedBytes) / shareTotal_);
                    RECT fill = bar;
                    fill.right = bar.left + static_cast<int>((bar.right - bar.left) * share + 0.5);
                    if (fill.right > fill.left) Fill(dc, fill, palette_.bar);
                }
                if (palette_.highContrast) {
                    // Outline so the empty part of the bar is visible too.
                    HPEN pen = CreatePen(PS_SOLID, 1, palette_.text);
                    HGDIOBJ op = SelectObject(dc, pen);
                    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                    Rectangle(dc, bar.left, bar.top, bar.right, bar.bottom);
                    SelectObject(dc, ob);
                    SelectObject(dc, op);
                    DeleteObject(pen);
                }
                RECT tr = r;
                tr.left = r.right - textWidth;
                Text(dc, FormatPercent(item.allocatedBytes, shareTotal_), tr, DT_RIGHT, secondary);
                break;
            }
        }
    }
}

void SizeList::Paint(HDC dc, const RECT& fullClient) {
    Fill(dc, fullClient, palette_.background);
    HGDIOBJ oldFont = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    // Rows and columns stop at the scroll bar; the header background and
    // its separator run the full width.
    RECT client = fullClient;
    if (scroll_.Visible()) client.right = std::max(client.left, client.right - scroll_.Width());

    // Header.
    RECT header{client.left, client.top, client.right, client.top + headerHeight_};
    RECT headerFull{fullClient.left, fullClient.top, fullClient.right, fullClient.top + headerHeight_};
    Fill(dc, headerFull, palette_.headerBackground);
    std::vector<RECT> cols;
    ColumnRects(header, cols);
    for (size_t c = 0; c < columns_.size(); ++c) {
        RECT r = cols[c];
        r.right -= Scale(8);
        std::wstring title = ui::Text(TitleId(columns_[c]));
        if (columns_[c] == sortColumn_) title += sortDescending_ ? L" ▾" : L" ▴";
        Text(dc, title, r, columns_[c] == Column::Name ? DT_LEFT : DT_RIGHT, palette_.secondaryText);
    }
    RECT sep{fullClient.left, header.bottom - std::max(1, Scale(1)), fullClient.right, header.bottom};
    Fill(dc, sep, palette_.line);

    if (items_.empty()) {
        RECT r{client.left, header.bottom, client.right, client.bottom};
        r.top += Scale(24);
        r.bottom = r.top + rowHeight_;
        Text(dc, message_.empty() ? ui::Text(IDS_EMPTY) : message_, r, DT_CENTER | DT_END_ELLIPSIS,
             palette_.secondaryText);
    } else {
        const int page = VisibleRows() + 1;
        for (int i = top_; i < static_cast<int>(items_.size()) && i < top_ + page; ++i) {
            int y = header.bottom + (i - top_) * rowHeight_;
            RECT row{client.left, y, client.right, y + rowHeight_};
            PaintRow(dc, i, row);
        }
    }
    scroll_.Paint(dc, palette_);
    SelectObject(dc, oldFont);
}

LRESULT CALLBACK SizeList::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SizeList* self = reinterpret_cast<SizeList*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<SizeList*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
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

LRESULT SizeList::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            dpi_ = GetDpiForWindow(hwnd_);
            scroll_.SetDpi(dpi_);
            UpdateMetrics();
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
                // Double buffer: draw into a memory bitmap, then copy once.
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
        case WM_TIMER: {
            int pos;
            if (scroll_.Timer(hwnd_, wp, pos)) {
                if (pos >= 0) ScrollTo(pos);
                return 0;
            }
            break;
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
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            int pos;
            if (scroll_.ButtonDown(hwnd_, POINT{x, y}, pos)) {
                if (pos >= 0) ScrollTo(pos);
                return 0;
            }
            int h = HitHeader(x, y);
            if (h >= 0) {
                Column c = columns_[h];
                if (c == sortColumn_) {
                    sortDescending_ = !sortDescending_;
                } else {
                    sortColumn_ = c;
                    sortDescending_ = DefaultDescending(c);
                }
                Sort();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            int row = HitRow(y);
            if (row >= 0) Select(row, true);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            // On the scroll bar, the second click of a fast pair is a click.
            int pos;
            if (scroll_.ButtonDown(hwnd_, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, pos)) {
                if (pos >= 0) ScrollTo(pos);
                return 0;
            }
            int row = HitRow(GET_Y_LPARAM(lp));
            if (row >= 0) PostMessageW(parent_, kListOpenItem, static_cast<WPARAM>(row), 0);
            return 0;
        }
        case WM_NCHITTEST: {
            // Let the parent's resizable edges (the Explorer panel) work over
            // the list too.
            LRESULT parent = SendMessageW(parent_, WM_NCHITTEST, 0, lp);
            if (parent != HTCLIENT && parent != HTNOWHERE && parent != HTERROR) return HTTRANSPARENT;
            break;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN: {
            const int count = static_cast<int>(items_.size());
            switch (wp) {
                case VK_UP: Select(selected_ < 0 ? 0 : selected_ - 1, true); return 0;
                case VK_DOWN: Select(selected_ + 1, true); return 0;
                case VK_PRIOR: Select(std::max(0, selected_) - VisibleRows(), true); return 0;
                case VK_NEXT: Select(std::max(0, selected_) + VisibleRows(), true); return 0;
                case VK_HOME: Select(0, true); return 0;
                case VK_END: Select(count - 1, true); return 0;
                case VK_RETURN:
                    if (selected_ >= 0) PostMessageW(parent_, kListOpenItem, static_cast<WPARAM>(selected_), 0);
                    return 0;
                case VK_BACK: PostMessageW(parent_, kListGoUp, 0, 0); return 0;
            }
            break;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace dirsize::ui
