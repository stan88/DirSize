#include "ui/list_model.h"

#include <windows.h>

#include <algorithm>

namespace dirsize::ui {

namespace {

int CompareNames(const std::wstring& a, const std::wstring& b) {
    int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                            static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0);
    if (r == 0) return a.compare(b);  // CompareStringEx failed: ordinal order
    return r - CSTR_EQUAL;            // CSTR_LESS_THAN / EQUAL / GREATER_THAN -> -1 / 0 / 1
}

std::uint64_t Key(const ListItem& item, Column column) {
    switch (column) {
        case Column::Size: return item.logicalBytes;
        case Column::Allocated:
        case Column::Share: return item.allocatedBytes;
        case Column::Files: return item.files;
        case Column::Name: break;
    }
    return 0;
}

}  // namespace

void SortItems(std::vector<ListItem>& items, Column column, bool descending) {
    std::stable_sort(items.begin(), items.end(), [&](const ListItem& a, const ListItem& b) {
        if (column != Column::Name) {
            std::uint64_t ka = Key(a, column), kb = Key(b, column);
            if (ka != kb) return descending ? ka > kb : ka < kb;
            return CompareNames(a.name, b.name) < 0;  // ties: by name, always ascending
        }
        int c = CompareNames(a.name, b.name);
        return descending ? c > 0 : c < 0;
    });
}

bool DefaultDescending(Column column) { return column != Column::Name; }

}  // namespace dirsize::ui
