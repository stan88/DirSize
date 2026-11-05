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

namespace dirsize::ui {

// Posted to the parent window.
constexpr UINT kListOpenItem = WM_APP + 200;  // wParam: index into Items() (double-click, Enter)
constexpr UINT kListGoUp = WM_APP + 201;      // Backspace

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
    void Select(int index, bool ensureVisible);
    void ScrollTo(int top);
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
};

}  // namespace dirsize::ui
