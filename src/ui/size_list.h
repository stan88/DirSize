#pragma once
// Self-drawn list of folders/files with sizes and a share bar (design
// section 5: the built-in ListView has no documented dark mode, so the list
// draws itself with the palette). Double-buffered, per-monitor DPI aware,
// keyboard and mouse wheel, click a header to sort.

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui/list_model.h"
#include "ui/palette.h"
#include "ui/scroll_strip.h"
#include "ui/tip_window.h"

namespace dirsize::ui {

// Posted to the parent window.
constexpr UINT kListOpenItem = WM_APP + 200;  // wParam: index into Items() (double-click, Enter)
constexpr UINT kListGoUp = WM_APP + 201;      // Backspace
// Right-click or the menu key: wParam = row index or (WPARAM)-1 for none
// (the row is selected first); lParam = screen point (GET_X/Y_LPARAM).
constexpr UINT kListContextMenu = WM_APP + 202;
// The user dragged a header separator (or reset one by double-clicking it);
// the parent saves ColumnWidths().
constexpr UINT kListColumnsResized = WM_APP + 203;
// Sent while a separator is dragged when SetGrowParent(true): wParam is the
// change in DIPs (signed, cast to int) the parent should add to its width,
// so the other columns keep theirs.
constexpr UINT kListGrowParent = WM_APP + 204;

class SizeList {
public:
    // Creates the child window; the object belongs to the window and is
    // deleted with it.
    static SizeList* Create(HWND parent, HINSTANCE instance);

    HWND Window() const { return hwnd_; }
    void SetColumns(const std::vector<Column>& columns);
    // Replaces the rows (sorted by the current column). `shareTotal` is the
    // allocated size the share bar is relative to (the folder's total).
    // `keepPosition`: keep the selection and scroll position by name (a
    // refresh of the same folder) instead of starting at the top.
    void SetItems(std::vector<ListItem> items, std::uint64_t shareTotal, bool keepPosition);
    // Text shown instead of rows when there are none (loading, errors).
    void SetMessage(const std::wstring& message);
    void SetPalette(const Palette& palette);
    void SetDpi(UINT dpi);
    // Narrower number and share columns, for the Explorer panel.
    void SetCompact(bool compact);
    // The parent (the Explorer panel) changes its width when a column is
    // resized (kListGrowParent), so the other columns keep their widths.
    // Otherwise the name column takes the difference.
    void SetGrowParent(bool grow) { growParent_ = grow; }
    // Widths the user set by dragging the separator at the right of a column
    // (the name column takes the rest); a column not listed has its default.
    void SetColumnWidths(const std::vector<ColumnWidth>& widths);
    const std::vector<ColumnWidth>& ColumnWidths() const { return widths_; }
    // The user's width or the default; -1 for a name column that fills the
    // rest (always in the panel; in the window until the user sets one).
    int ColumnDip(Column c) const;

    const std::vector<ListItem>& Items() const { return items_; }
    int Selected() const { return selected_; }

private:
    SizeList() = default;
    ~SizeList();
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void UpdateMetrics();
    void UpdateScroll();
    int VisibleRows() const;
    void Paint(HDC dc, const RECT& client);
    void PaintRow(HDC dc, int index, const RECT& row);
    void ColumnRects(const RECT& client, std::vector<RECT>& out) const;
    int HitRow(int y) const;
    int HitHeader(int x, int y) const;
    // The header separator under (x, y): the index of the column at its
    // right (its width changes), or -1.
    int HitSeparator(int x, int y) const;
    void SetColumnDip(Column c, int dip);
    void ApplyDrag(int dip);
    std::vector<int> ColumnPixels(int available) const;  // LayoutColumns for this list
    int TotalWidth(int available) const;                  // pad + all columns, pixels
    void ScrollXTo(int x);
    // Where the row's name text is drawn (after the icon and before the
    // "no access" tag), and whether the name is cut off there.
    RECT NameTextRect(HDC dc, int index, const RECT& row) const;
    bool NameCutOff(int index, RECT* textRect) const;
    void UpdateTip(POINT pt);  // mouse moved: hide the tip when the row changed
    void Select(int index, bool ensureVisible);
    void ScrollTo(int top);
    RECT ContentRect() const;  // the client area left of the scroll bar
    void Sort();
    int Scale(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    std::vector<Column> columns_ = {Column::Name, Column::Size, Column::Allocated, Column::Share, Column::Files};
    std::vector<ListItem> items_;
    std::uint64_t shareTotal_ = 0;
    std::wstring message_;
    Palette palette_;
    UINT dpi_ = 96;
    HFONT font_ = nullptr;
    int rowHeight_ = 24;
    int headerHeight_ = 26;
    int top_ = 0;  // first visible row
    int selected_ = -1;
    Column sortColumn_ = Column::Allocated;
    bool sortDescending_ = true;
    int wheelRemainder_ = 0;
    bool compact_ = false;
    ScrollStrip scroll_;
    ScrollStrip hscroll_;              // sideways, when the columns are wider than the list
    int scrollX_ = 0;                  // pixels scrolled sideways
    int hbar_ = 0;                     // height of the horizontal bar when shown, else 0
    std::vector<ColumnWidth> widths_;  // user widths (DIPs)
    int resizing_ = -1;                // column whose separator is dragged
    int resizeStartX_ = 0;             // screen x where the drag started
    int resizeStartDip_ = 0;
    int resizeLastDip_ = 0;            // width applied by the last drag step
    bool growParent_ = false;
    int tipRow_ = -1;                  // row under the mouse for the tooltip
    bool tracking_ = false;            // TrackMouseEvent leave tracking is on
    TipWindow tip_;
};

}  // namespace dirsize::ui
