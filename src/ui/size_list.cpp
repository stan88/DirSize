#include "ui/size_list.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdlib>

#include "ui/format.h"
#include "ui/strings.h"

namespace dirsize::ui {

namespace {

constexpr wchar_t kClassName[] = L"DirSizeSizeList";

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
    const Column sort = FallbackSortColumn(sortColumn_, columns_);
    if (sort != sortColumn_) {
        // Share -> Allocated keeps the order (same key); others restart.
        if (!(sortColumn_ == Column::Share && sort == Column::Allocated)) sortDescending_ = DefaultDescending(sort);
        sortColumn_ = sort;
        Sort();
    }
    UpdateScroll();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SizeList::SetItems(std::vector<ListItem> items, std::uint64_t shareTotal, bool keepPosition) {
    std::wstring selectedName = keepPosition && selected_ >= 0 && selected_ < static_cast<int>(items_.size())
                                    ? items_[selected_].name
                                    : std::wstring();
    int oldTop = top_;
    tip_.Hide();
    tipRow_ = -1;
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
    hscroll_.SetDpi(dpi_);
    UpdateMetrics();
    UpdateScroll();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SizeList::SetCompact(bool compact) {
    compact_ = compact;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SizeList::SetColumnWidths(const std::vector<ColumnWidth>& widths) {
    widths_ = widths;
    UpdateScroll();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// The name column: its own width only in the window after the user set one;
// otherwise -1 (it fills the rest).
int SizeList::ColumnDip(Column c) const {
    if (c == Column::Name && growParent_) return -1;
    for (const ColumnWidth& w : widths_) {
        if (w.column == c) return w.dip;
    }
    return c == Column::Name ? -1 : ColumnWidthDip(c, compact_);
}

std::vector<int> SizeList::ColumnPixels(int available) const {
    std::vector<int> dips;
    for (Column c : columns_) dips.push_back(ColumnDip(c));
    return LayoutColumns(dips, dpi_, available, Scale(8), Scale(compact_ ? 60 : 120));
}

int SizeList::TotalWidth(int available) const {
    int total = Scale(8);
    for (int w : ColumnPixels(available)) total += w;
    return total;
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
    int h = rc.bottom - headerHeight_ - hbar_;
    return h > 0 ? std::max(1, h / rowHeight_) : 1;
}

// Both bars: the vertical one when the rows do not fit, the horizontal one
// when the columns are wider than the list. Each takes room from the other,
// so it is decided twice.
void SizeList::UpdateScroll() {
    const int count = static_cast<int>(items_.size());
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int bar = scroll_.Width();
    int width = rc.right;
    hbar_ = 0;
    for (int pass = 0; pass < 2; ++pass) {
        hbar_ = TotalWidth(width) > width ? bar : 0;
        const int rows = std::max(1, (static_cast<int>(rc.bottom) - headerHeight_ - hbar_) / rowHeight_);
        width = count > rows ? rc.right - bar : rc.right;
    }
    const int page = VisibleRows();
    top_ = std::clamp(top_, 0, std::max(0, count - page));
    // The vertical bar runs below the column header, above the other bar.
    scroll_.SetRect(RECT{rc.right - bar, headerHeight_, rc.right, rc.bottom - hbar_});
    scroll_.SetRange(count, page, top_);
    const int total = TotalWidth(width);
    scrollX_ = std::clamp(scrollX_, 0, std::max(0, total - width));
    hscroll_.SetRect(RECT{0, rc.bottom - bar, width, rc.bottom});
    hscroll_.SetRange(hbar_ ? total : 0, width, scrollX_);
}

RECT SizeList::ContentRect() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (scroll_.Visible()) rc.right = std::max(rc.left, rc.right - scroll_.Width());
    rc.bottom = std::max(rc.top, rc.bottom - hbar_);
    return rc;
}

void SizeList::ScrollXTo(int x) {
    tip_.Hide();
    const int old = scrollX_;
    scrollX_ = x;
    UpdateScroll();
    if (scrollX_ != old) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SizeList::ScrollTo(int top) {
    tip_.Hide();
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

// Laid out from the left and moved by the horizontal scroll position.
void SizeList::ColumnRects(const RECT& client, std::vector<RECT>& out) const {
    out.assign(columns_.size(), RECT{});
    const std::vector<int> px = ColumnPixels(client.right - client.left);
    int x = client.left + Scale(8) - scrollX_;
    for (size_t i = 0; i < columns_.size(); ++i) {
        out[i] = RECT{x, client.top, x + px[i], client.bottom};
        x += px[i];
    }
}

// A separator is a column's right edge and resizes that column (as in File
// Explorer); the columns to its right move with it.
int SizeList::HitSeparator(int x, int y) const {
    if (y < 0 || y >= headerHeight_) return -1;
    const RECT content = ContentRect();
    if (x >= content.right) return -1;
    std::vector<RECT> cols;
    ColumnRects(content, cols);
    const int slop = Scale(4);
    for (size_t i = 0; i < cols.size(); ++i) {
        if (std::abs(x - static_cast<int>(cols[i].right)) <= slop) return static_cast<int>(i);
    }
    return -1;
}

void SizeList::SetColumnDip(Column c, int dip) {
    auto same = std::find_if(widths_.begin(), widths_.end(), [&](const ColumnWidth& w) { return w.column == c; });
    if (same != widths_.end()) same->dip = dip;
    else widths_.push_back({c, dip});
}

// Drag step: the dragged column becomes `dip` wide. The other columns keep
// their widths: in the panel the parent grows or shrinks by the difference
// (kListGrowParent, sent so the layout follows at once); in the window the
// name column takes the difference.
void SizeList::ApplyDrag(int dip) {
    const int delta = dip - resizeLastDip_;
    if (delta == 0) return;
    resizeLastDip_ = dip;
    const Column c = columns_[resizing_];
    // In the panel the name column has no width of its own (it fills).
    if (c != Column::Name || !growParent_) SetColumnDip(c, dip);
    if (growParent_) SendMessageW(parent_, kListGrowParent, static_cast<WPARAM>(delta), 0);
    UpdateScroll();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// The same geometry as the Name case of PaintRow.
RECT SizeList::NameTextRect(HDC dc, int index, const RECT& row) const {
    std::vector<RECT> cols;
    ColumnRects(row, cols);
    RECT r = row;
    for (size_t c = 0; c < columns_.size(); ++c) {
        if (columns_[c] == Column::Name) r = cols[c];
    }
    r.right -= Scale(8);
    r.left += Scale(14) + Scale(8);  // the icon and its gap
    if (!items_[index].accessible) {
        std::wstring tag = L"  " + ui::Text(IDS_NO_ACCESS);
        SIZE ts{};
        GetTextExtentPoint32W(dc, tag.c_str(), static_cast<int>(tag.size()), &ts);
        r.right = std::max(r.left, r.right - ts.cx);
    }
    return r;
}

bool SizeList::NameCutOff(int index, RECT* textRect) const {
    if (index < top_ || index >= static_cast<int>(items_.size()) || index >= top_ + VisibleRows() + 1) return false;
    const RECT content = ContentRect();
    const int y = headerHeight_ + (index - top_) * rowHeight_;
    const RECT row{content.left, y, content.right, y + rowHeight_};
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    const RECT r = NameTextRect(dc, index, row);
    const std::wstring& name = items_[index].name;
    SIZE size{};
    GetTextExtentPoint32W(dc, name.c_str(), static_cast<int>(name.size()), &size);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    if (textRect) *textRect = RECT{r.left, row.top, r.right, row.bottom};
    return size.cx > r.right - r.left;
}

// The tooltip follows the row under the mouse: a new row hides it and asks
// for a new WM_MOUSEHOVER (it comes once per TrackMouseEvent call). The
// same row does nothing: showing the tip makes Windows send a WM_MOUSEMOVE,
// and hiding the tip then hid it again at once (seen in testing).
// `tracking_` is the leave tracking, which lasts until WM_MOUSELEAVE.
void SizeList::UpdateTip(POINT pt) {
    const int row = pt.x < ContentRect().right ? HitRow(pt.y) : -1;
    if (row == tipRow_ && tracking_) return;
    if (row != tipRow_) tip_.Hide();
    tipRow_ = row;
    TRACKMOUSEEVENT tme{sizeof(tme)};
    tme.dwFlags = TME_HOVER | TME_LEAVE;
    tme.hwndTrack = hwnd_;
    tme.dwHoverTime = HOVER_DEFAULT;
    tracking_ = TrackMouseEvent(&tme) != FALSE;
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
    RECT client = ContentRect();  // left of the vertical bar, above the horizontal one

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
        // The separator at the column's right edge, which can be dragged
        // (not drawn after the last column: that edge is the list's edge).
        if (c + 1 < columns_.size()) {
            const int inset = headerHeight_ / 4;
            const int x = cols[c].right - std::max(1, Scale(1));
            RECT line{x, header.top + inset, x + std::max(1, Scale(1)), header.bottom - inset};
            Fill(dc, line, resizing_ == static_cast<int>(c) ? palette_.bar : palette_.line);
        }
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
    hscroll_.Paint(dc, palette_);
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
            hscroll_.SetHorizontal(true);
            hscroll_.SetDpi(dpi_);
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
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (resizing_ >= 0) {
                // In screen coordinates: the panel moves while it grows
                // (it may grow to the left), so client x would drift.
                POINT screen = pt;
                ClientToScreen(hwnd_, &screen);
                // In the panel the name column's "width" is a change of the
                // panel width (no limits of its own; the panel has them).
                const Column c = columns_[resizing_];
                const bool panelName = c == Column::Name && growParent_;
                ApplyDrag(DraggedWidthDip(resizeStartDip_, screen.x - resizeStartX_, dpi_, !panelName, ColumnMaxDip(c)));
                return 0;
            }
            int pos;
            if (scroll_.MouseMove(hwnd_, pt, pos) && pos >= 0) ScrollTo(pos);
            if (hscroll_.MouseMove(hwnd_, pt, pos) && pos >= 0) ScrollXTo(pos);
            UpdateTip(pt);
            return 0;
        }
        case WM_MOUSEHOVER: {
            RECT text;
            if (tipRow_ >= 0 && NameCutOff(tipRow_, &text)) {
                // Just below the row, its text in line with the name (8 DIPs
                // of padding and a pixel of border). Not over the row: a tip
                // under the mouse made the list get WM_MOUSELEAVE, which hid
                // the tip again at once (seen in testing).
                POINT at{text.left - Scale(8) - 1, text.bottom + Scale(2)};
                ClientToScreen(hwnd_, &at);
                tip_.Show(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE)), at,
                          items_[tipRow_].name, font_, palette_, dpi_);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd_, &pt);
                if (resizing_ >= 0 || HitSeparator(pt.x, pt.y) >= 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONUP:
            if (resizing_ >= 0) {
                ReleaseCapture();  // WM_CAPTURECHANGED ends the drag
                return 0;
            }
            scroll_.ButtonUp(hwnd_);
            hscroll_.ButtonUp(hwnd_);
            return 0;
        case WM_CAPTURECHANGED:
            if (resizing_ >= 0) {
                resizing_ = -1;
                PostMessageW(parent_, kListColumnsResized, 0, 0);
                return 0;
            }
            scroll_.CaptureLost(hwnd_);
            hscroll_.CaptureLost(hwnd_);
            return 0;
        case WM_MOUSELEAVE:
            tip_.Hide();
            tipRow_ = -1;
            tracking_ = false;
            scroll_.MouseLeave(hwnd_);
            hscroll_.MouseLeave(hwnd_);
            return 0;
        case WM_TIMER: {
            int pos;
            if (scroll_.Timer(hwnd_, wp, pos)) {
                if (pos >= 0) ScrollTo(pos);
                return 0;
            }
            if (hscroll_.Timer(hwnd_, wp, pos)) {
                if (pos >= 0) ScrollXTo(pos);
                return 0;
            }
            break;
        }
        case WM_MOUSEHWHEEL:  // tilt wheel, touchpad: right is positive
            tip_.Hide();
            ScrollXTo(scrollX_ + MulDiv(GET_WHEEL_DELTA_WPARAM(wp), Scale(48), WHEEL_DELTA));
            return 0;
        case WM_MOUSEWHEEL: {
            if (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) {  // Shift+wheel: sideways (up = left)
                tip_.Hide();
                ScrollXTo(scrollX_ - MulDiv(GET_WHEEL_DELTA_WPARAM(wp), Scale(48), WHEEL_DELTA));
                return 0;
            }
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            if (lines == WHEEL_PAGESCROLL) lines = static_cast<UINT>(VisibleRows());
            tip_.Hide();
            tipRow_ = -1;
            wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(wp);
            int steps = wheelRemainder_ / WHEEL_DELTA;
            wheelRemainder_ -= steps * WHEEL_DELTA;
            if (steps) ScrollTo(top_ - steps * static_cast<int>(lines));
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            tip_.Hide();
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            int pos;
            if (scroll_.ButtonDown(hwnd_, POINT{x, y}, pos)) {
                if (pos >= 0) ScrollTo(pos);
                return 0;
            }
            if (hscroll_.ButtonDown(hwnd_, POINT{x, y}, pos)) {
                if (pos >= 0) ScrollXTo(pos);
                return 0;
            }
            if (int sep = HitSeparator(x, y); sep >= 0) {
                POINT screen{x, y};
                ClientToScreen(hwnd_, &screen);
                resizing_ = sep;
                resizeStartX_ = screen.x;
                // In the window, a name column that filled the rest keeps
                // the width it has now, so moving a column changes nothing
                // else (as in File Explorer). In the panel the name column
                // has no width of its own: its drag is a change of the panel
                // width, counted from 0.
                if (!growParent_ && ColumnDip(Column::Name) < 0) {
                    std::vector<RECT> cols;
                    ColumnRects(ContentRect(), cols);
                    for (size_t i = 0; i < columns_.size(); ++i) {
                        if (columns_[i] == Column::Name) {
                            SetColumnDip(Column::Name, std::clamp(MulDiv(cols[i].right - cols[i].left, 96,
                                                                         static_cast<int>(dpi_)),
                                                                  kColumnMinDip, kNameColumnMaxDip));
                        }
                    }
                }
                resizeStartDip_ = columns_[sep] == Column::Name && growParent_ ? 0 : ColumnDip(columns_[sep]);
                resizeLastDip_ = resizeStartDip_;
                SetCapture(hwnd_);
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
            if (hscroll_.ButtonDown(hwnd_, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, pos)) {
                if (pos >= 0) ScrollXTo(pos);
                return 0;
            }
            // On a separator: back to the default width. For the name column
            // in the window that is "fill the rest" again; in the panel it
            // has none. The panel shrinks or grows with the column.
            if (int sep = HitSeparator(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); sep >= 0) {
                const Column c = columns_[sep];
                if (c == Column::Name && growParent_) return 0;
                const int delta = c == Column::Name ? 0 : ColumnWidthDip(c, compact_) - ColumnDip(c);
                widths_.erase(std::remove_if(widths_.begin(), widths_.end(),
                                             [&](const ColumnWidth& w) { return w.column == c; }),
                              widths_.end());
                if (growParent_ && delta != 0) SendMessageW(parent_, kListGrowParent, static_cast<WPARAM>(delta), 0);
                UpdateScroll();
                InvalidateRect(hwnd_, nullptr, FALSE);
                PostMessageW(parent_, kListColumnsResized, 0, 0);
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
        case WM_CONTEXTMENU: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int index = -1;
            if (pt.x == -1 && pt.y == -1) {
                // From the keyboard (Shift+F10, menu key): at the selected
                // row, or the top of the list (WM_CONTEXTMENU documentation).
                index = selected_;
                int y = headerHeight_;
                if (index >= 0 && index >= top_ && index < top_ + VisibleRows()) y += (index - top_) * rowHeight_;
                pt = POINT{Scale(24), y + rowHeight_ / 2};
                ClientToScreen(hwnd_, &pt);
            } else {
                POINT client = pt;
                ScreenToClient(hwnd_, &client);
                RECT content = ContentRect();
                if (client.x >= content.right) return 0;  // over the scroll bar
                index = HitRow(client.y);
                if (index >= 0) Select(index, false);
            }
            PostMessageW(parent_, kListContextMenu, static_cast<WPARAM>(index), MAKELPARAM(pt.x, pt.y));
            return 0;
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
