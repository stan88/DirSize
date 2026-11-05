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

// Sorts in place. Share sorts like Allocated (the bar is the allocated
// share). Ties, and Name, use the user's locale order with digits compared
// as numbers (CompareStringEx, NORM_IGNORECASE | SORT_DIGITSASNUMBERS), so
// "file2" comes before "file10" as in Explorer.
void SortItems(std::vector<ListItem>& items, Column column, bool descending);

// The default order of a column the first time it is clicked: names
// ascending, numbers descending (largest first).
bool DefaultDescending(Column column);

}  // namespace dirsize::ui
