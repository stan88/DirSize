#pragma once
// Rows of the size list and how they sort. No window code here, so the
// sorting is unit-tested.

#include <cstdint>
#include <string>
#include <vector>

namespace dirsize::ui {

struct ListItem {
    std::wstring name;
    bool isFolder = false;
    bool accessible = true;  // false: shown as "no access" (sizes still shown, see design 8.1)
    std::uint64_t logicalBytes = 0;
    std::uint64_t allocatedBytes = 0;
    std::uint64_t files = 0;  // files under a folder; 1 for a file
};

enum class Column { Name, Size, Allocated, Share, Files };

// Sorts in place. Folders always come before files, in both directions,
// and each group is sorted by the column. Share sorts like Allocated (the bar is the allocated
// share). Ties, and Name, use the user's locale order with digits compared
// as numbers (CompareStringEx, NORM_IGNORECASE | SORT_DIGITSASNUMBERS), so
// "file2" comes before "file10" as in Explorer.
void SortItems(std::vector<ListItem>& items, Column column, bool descending);

// Name order of the list and the folder tree: the user's locale with digits
// compared as numbers, ignoring case. Negative, zero or positive.
int CompareNames(const std::wstring& a, const std::wstring& b);

// The list starts sorted by name, ascending (user request 2026-11-13).
constexpr Column kDefaultSortColumn = Column::Name;

// The default order of a column the first time it is clicked: names
// ascending, numbers descending (largest first).
bool DefaultDescending(Column column);

// The columns of the standalone window and of the Explorer panel. The share
// column (bar and percentage) is a setting (settings.h, ShowShareColumn).
std::vector<Column> WindowColumns(bool showShare);
std::vector<Column> PanelColumns(bool showShare);

// Default column widths in DIPs (Name takes the rest); `compact` for the
// panel. The user can change them by dragging the header separators.
int ColumnWidthDip(Column column, bool compact);

// Column widths the user set, saved as text: "Name=300;Allocated=96".
// Unknown names, bad numbers and widths outside the limits are skipped on
// reading, so a damaged value only loses those entries. A Name width is
// used only in the standalone window (without one the name column fills
// the rest, as it always does in the panel).
constexpr int kColumnMinDip = 40;
constexpr int kColumnMaxDip = 400;
constexpr int kNameColumnMaxDip = 1200;
int ColumnMaxDip(Column column);

// Pixel widths of the columns, laid out from the left. `dips[i]` is the
// width of column i in DIPs, or negative for a column that fills what is
// left of `available` after `pad` and the others (at least `fillMinPx`).
// The widths may add up to more than `available`: the list then scrolls
// sideways.
std::vector<int> LayoutColumns(const std::vector<int>& dips, unsigned dpi, int available, int pad, int fillMinPx);
struct ColumnWidth {
    Column column;
    int dip;
};
std::vector<ColumnWidth> ParseColumnWidths(const std::wstring& text);

// A separator drag: the column's width after the mouse moved `dxPixels`
// (right = wider) at `dpi`. `clamp`: keep it within the column limits (off
// for the name column in the panel, whose "width" is a change of the panel
// width, limited by the panel instead).
int DraggedWidthDip(int startDip, int dxPixels, unsigned dpi, bool clamp, int maxDip = kColumnMaxDip);
std::wstring FormatColumnWidths(const std::vector<ColumnWidth>& widths);

// The panel width. The saved width (HKCU PanelWidth) is the width with the
// share column; without it the panel is narrower by that column's width
// (`shareDip`, which the user may have changed), so turning the setting off
// and on returns to the same width.
constexpr int kPanelMinWidthDip = 240;        // with the share column
constexpr int kPanelMaxWidthDip = 900;
constexpr int kPanelMinWidthNoShareDip = 160; // name, size on disk, scroll bar
int PanelShownWidthDip(int savedDip, bool showShare, int shareDip);
int PanelSavedWidthDip(int shownDip, bool showShare, int shareDip);
int PanelMinWidthDip(bool showShare);
int PanelMaxWidthDip(bool showShare, int shareDip);

// The column to sort by when `current` is no longer shown: Share sorts like
// Allocated, so it falls back to Allocated when that is there; otherwise
// the second column (the first number), or Name.
Column FallbackSortColumn(Column current, const std::vector<Column>& columns);

}  // namespace dirsize::ui
