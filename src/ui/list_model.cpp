#include "ui/list_model.h"

#include <windows.h>

#include <algorithm>

namespace dirsize::ui {

int CompareNames(const std::wstring& a, const std::wstring& b) {
    int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                            static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0);
    if (r == 0) return a.compare(b);  // CompareStringEx failed: ordinal order
    return r - CSTR_EQUAL;            // CSTR_LESS_THAN / EQUAL / GREATER_THAN -> -1 / 0 / 1
}

namespace {

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

std::vector<Column> WindowColumns(bool showShare) {
    if (showShare) return {Column::Name, Column::Size, Column::Allocated, Column::Share, Column::Files};
    return {Column::Name, Column::Size, Column::Allocated, Column::Files};
}

std::vector<Column> PanelColumns(bool showShare) {
    if (showShare) return {Column::Name, Column::Allocated, Column::Share};
    return {Column::Name, Column::Allocated};
}

int ColumnWidthDip(Column column, bool compact) {
    switch (column) {
        case Column::Size: return compact ? 76 : 90;
        case Column::Allocated: return compact ? 80 : 96;
        case Column::Share: return compact ? 112 : 150;
        case Column::Files: return compact ? 70 : 84;
        case Column::Name: break;
    }
    return 0;
}

namespace {

struct ColumnName {
    Column column;
    const wchar_t* name;
};
constexpr ColumnName kColumnNames[] = {{Column::Name, L"Name"},
                                       {Column::Size, L"Size"},
                                       {Column::Allocated, L"Allocated"},
                                       {Column::Share, L"Share"},
                                       {Column::Files, L"Files"}};

int HiddenShareDip(bool showShare, int shareDip) {
    return showShare ? 0 : std::clamp(shareDip, kColumnMinDip, kColumnMaxDip);
}

}  // namespace

std::vector<ColumnWidth> ParseColumnWidths(const std::wstring& text) {
    std::vector<ColumnWidth> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L';', start);
        if (end == std::wstring::npos) end = text.size();
        const std::wstring part = text.substr(start, end - start);
        const size_t eq = part.find(L'=');
        if (eq != std::wstring::npos && eq + 1 < part.size() && part.size() - eq - 1 <= 4) {
            const std::wstring name = part.substr(0, eq), number = part.substr(eq + 1);
            int dip = 0;
            bool digits = true;
            for (wchar_t ch : number) {
                if (ch < L'0' || ch > L'9') digits = false;
                else dip = dip * 10 + (ch - L'0');
            }
            for (const ColumnName& c : kColumnNames) {
                if (digits && name == c.name && dip >= kColumnMinDip && dip <= ColumnMaxDip(c.column)) {
                    // A later entry for the same column replaces an earlier one.
                    auto same = std::find_if(out.begin(), out.end(),
                                             [&](const ColumnWidth& w) { return w.column == c.column; });
                    if (same != out.end()) same->dip = dip;
                    else out.push_back({c.column, dip});
                }
            }
        }
        start = end + 1;
    }
    return out;
}

int ColumnMaxDip(Column column) { return column == Column::Name ? kNameColumnMaxDip : kColumnMaxDip; }

std::vector<int> LayoutColumns(const std::vector<int>& dips, unsigned dpi, int available, int pad, int fillMinPx) {
    const int d = dpi ? static_cast<int>(dpi) : 96;
    std::vector<int> px(dips.size(), 0);
    int fixed = 0;
    for (size_t i = 0; i < dips.size(); ++i) {
        if (dips[i] >= 0) {
            px[i] = MulDiv(dips[i], d, 96);
            fixed += px[i];
        }
    }
    for (size_t i = 0; i < dips.size(); ++i) {
        if (dips[i] < 0) px[i] = std::max(fillMinPx, available - pad - fixed);
    }
    return px;
}

int DraggedWidthDip(int startDip, int dxPixels, unsigned dpi, bool clamp, int maxDip) {
    // Rounded to the nearest DIP, the same both ways (MulDiv rounds half
    // away from zero).
    const int dip = startDip + MulDiv(dxPixels, 96, dpi ? static_cast<int>(dpi) : 96);
    return clamp ? std::clamp(dip, kColumnMinDip, maxDip) : dip;
}

std::wstring FormatColumnWidths(const std::vector<ColumnWidth>& widths) {
    std::wstring out;
    for (const ColumnWidth& w : widths) {
        for (const ColumnName& c : kColumnNames) {
            if (c.column != w.column) continue;
            if (!out.empty()) out += L';';
            out += std::wstring(c.name) + L"=" + std::to_wstring(std::clamp(w.dip, kColumnMinDip, ColumnMaxDip(c.column)));
        }
    }
    return out;
}

int PanelMinWidthDip(bool showShare) { return showShare ? kPanelMinWidthDip : kPanelMinWidthNoShareDip; }

int PanelMaxWidthDip(bool showShare, int shareDip) {
    return kPanelMaxWidthDip - HiddenShareDip(showShare, shareDip);
}

int PanelShownWidthDip(int savedDip, bool showShare, int shareDip) {
    const int saved = std::clamp(savedDip, kPanelMinWidthDip, kPanelMaxWidthDip);
    return std::clamp(saved - HiddenShareDip(showShare, shareDip), PanelMinWidthDip(showShare),
                      PanelMaxWidthDip(showShare, shareDip));
}

int PanelSavedWidthDip(int shownDip, bool showShare, int shareDip) {
    return std::clamp(shownDip + HiddenShareDip(showShare, shareDip), kPanelMinWidthDip, kPanelMaxWidthDip);
}

Column FallbackSortColumn(Column current, const std::vector<Column>& columns) {
    auto has = [&](Column c) { return std::find(columns.begin(), columns.end(), c) != columns.end(); };
    if (has(current)) return current;
    if (current == Column::Share && has(Column::Allocated)) return Column::Allocated;
    return columns.size() > 1 ? columns[1] : Column::Name;
}

}  // namespace dirsize::ui
