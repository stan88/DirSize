// Unit tests for the front end's pure logic (Phase 5): size formatting,
// list sorting, palettes, panel placement and the panel rules. The panel
// rules and placement are ported from tools/explorer_probe.cpp, where they
// were settled in three VM rounds; these tests pin them.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "ui/format.h"
#include "ui/list_model.h"
#include "ui/palette.h"
#include "ui/panel_rules.h"
#include "ui/placement.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

using namespace dirsize::ui;

void TestFormat() {
    CHECK(FormatBytes(0) == L"0 B");
    CHECK(FormatBytes(1023) == L"1023 B");
    CHECK(FormatBytes(1024) == L"1.00 KB");
    CHECK(FormatBytes(1536) == L"1.50 KB");
    CHECK(FormatBytes(10 * 1024) == L"10.0 KB");
    CHECK(FormatBytes(100 * 1024) == L"100 KB");
    CHECK(FormatBytes(1023 * 1024) == L"0.999 MB" || FormatBytes(1023 * 1024) == L"1.00 MB");
    CHECK(FormatBytes(1048575) == L"1.00 MB");  // 1023.999 KB rounds into the next unit
    CHECK(FormatBytes(5ull * 1024 * 1024 * 1024) == L"5.00 GB");
    CHECK(FormatBytes(1234567890123ull) == L"1.12 TB");
    CHECK(FormatBytes(~0ull) == L"16.0 EB");

    CHECK(FormatCount(0) == L"0");
    CHECK(FormatCount(999) == L"999");
    CHECK(FormatCount(1000) == L"1,000");
    CHECK(FormatCount(1234567) == L"1,234,567");
    CHECK(FormatCount(100000) == L"100,000");
    // Lengths not a multiple of 3 (a bug seen in the first screenshot:
    // "3,2" and "3,9,724").
    CHECK(FormatCount(32) == L"32");
    CHECK(FormatCount(1234) == L"1,234");
    CHECK(FormatCount(39724) == L"39,724");
    CHECK(FormatCount(15517) == L"15,517");
    CHECK(FormatCount(12345678) == L"12,345,678");
    CHECK(FormatCount(18446744073709551615ull) == L"18,446,744,073,709,551,615");

    CHECK(FormatPercent(0, 0) == L"0.0%");
    CHECK(FormatPercent(1, 3) == L"33.3%");
    CHECK(FormatPercent(5, 5) == L"100.0%");
}

ListItem Item(const wchar_t* name, std::uint64_t logical, std::uint64_t allocated, std::uint64_t files) {
    ListItem i;
    i.name = name;
    i.logicalBytes = logical;
    i.allocatedBytes = allocated;
    i.files = files;
    return i;
}

std::wstring Names(const std::vector<ListItem>& v) {
    std::wstring s;
    for (const ListItem& i : v) s += i.name + L" ";
    return s;
}

void TestSort() {
    std::vector<ListItem> v = {Item(L"b", 10, 4096, 3), Item(L"A", 30, 8192, 1), Item(L"file10", 20, 4096, 2),
                               Item(L"file2", 20, 4096, 2)};
    SortItems(v, Column::Name, false);
    CHECK(Names(v) == L"A b file2 file10 ");  // case-insensitive, digits as numbers
    SortItems(v, Column::Name, true);
    CHECK(Names(v) == L"file10 file2 b A ");
    SortItems(v, Column::Allocated, true);
    CHECK(Names(v) == L"A b file2 file10 ");  // ties by name ascending
    SortItems(v, Column::Share, false);
    CHECK(Names(v) == L"b file2 file10 A ");
    SortItems(v, Column::Size, true);
    CHECK(Names(v) == L"A file2 file10 b ");
    SortItems(v, Column::Files, false);
    CHECK(Names(v) == L"A file2 file10 b ");
    CHECK(!DefaultDescending(Column::Name));
    CHECK(DefaultDescending(Column::Allocated));
}

void TestPalette() {
    // The formula from the Microsoft themes page: white is light, black is not.
    CHECK(IsColorLight(255, 255, 255));
    CHECK(!IsColorLight(0, 0, 0));
    CHECK(!IsColorLight(0x20, 0x20, 0x20));

    SystemColors sys{RGB(0, 0, 0), RGB(255, 255, 0), RGB(0, 255, 255), RGB(0, 0, 0), RGB(128, 128, 128)};
    Palette hc = MakePalette(false, true, RGB(1, 2, 3), sys);
    CHECK(hc.highContrast);
    CHECK(hc.dark);  // black window
    CHECK(hc.background == sys.window && hc.text == sys.windowText);
    CHECK(hc.selection == sys.highlight && hc.selectionText == sys.highlightText);
    CHECK(hc.bar == sys.highlight);  // accent ignored in high contrast

    Palette dark = MakePalette(true, false, RGB(1, 2, 3), sys);
    CHECK(dark.dark && !dark.highContrast);
    CHECK(dark.bar == RGB(1, 2, 3));
    CHECK(!IsColorLight(GetRValue(dark.background), GetGValue(dark.background), GetBValue(dark.background)));
    CHECK(IsColorLight(GetRValue(dark.text), GetGValue(dark.text), GetBValue(dark.text)));

    Palette light = MakePalette(false, false, RGB(1, 2, 3), sys);
    CHECK(!light.dark);
    CHECK(IsColorLight(GetRValue(light.background), GetGValue(light.background), GetBValue(light.background)));
}

void TestPlacement() {
    const RECT work{0, 0, 1920, 1040};
    // Room on the right.
    PanelPlacement p = PlacePanel(RECT{100, 100, 900, 700}, work, 320);
    CHECK(p.side == PanelSide::OutsideRight && p.x == 900 && p.top == 100 && p.bottom == 700);
    // Exactly fits on the right.
    p = PlacePanel(RECT{100, 100, 1600, 700}, work, 320);
    CHECK(p.side == PanelSide::OutsideRight && p.x == 1600);
    // Near the right edge: outside left.
    p = PlacePanel(RECT{1000, 100, 1800, 700}, work, 320);
    CHECK(p.side == PanelSide::OutsideLeft && p.x == 680);
    // No room on either side: inside right.
    p = PlacePanel(RECT{200, 100, 1800, 700}, work, 320);
    CHECK(p.side == PanelSide::InsideRight && p.x == 1480);
    // Maximized (frame beyond the work area): inside, clipped to the work area.
    p = PlacePanel(RECT{-8, -8, 1928, 1048}, work, 320);
    CHECK(p.side == PanelSide::InsideRight && p.x == 1600 && p.top == 0 && p.bottom == 1040);
    // Explorer partly off the right edge: the panel stays on screen.
    p = PlacePanel(RECT{1700, 100, 2500, 700}, work, 320);
    CHECK(p.side == PanelSide::OutsideLeft && p.x == 1380);
    p = PlacePanel(RECT{100, 100, 2500, 700}, work, 320);
    CHECK(p.side == PanelSide::InsideRight && p.x == 1600);
    // A second monitor to the right (work area does not start at 0).
    const RECT work2{1920, 0, 3840, 1040};
    p = PlacePanel(RECT{1900, 100, 2600, 700}, work2, 320);
    CHECK(p.side == PanelSide::OutsideRight && p.x == 2600);
    // Narrow work area: x never left of the work area.
    p = PlacePanel(RECT{0, 0, 200, 300}, RECT{0, 0, 250, 300}, 320);
    CHECK(p.side == PanelSide::InsideRight && p.x == 0);
    // A height set by the user: aligned with Explorer's top.
    p = PlacePanel(RECT{100, 100, 900, 700}, work, 320, 250);
    CHECK(p.top == 100 && p.bottom == 350);
    // Taller than Explorer: as tall as Explorer.
    p = PlacePanel(RECT{100, 100, 900, 700}, work, 320, 5000);
    CHECK(p.top == 100 && p.bottom == 700);
    // Explorer above the work area's top: the height counts from the clipped top.
    p = PlacePanel(RECT{100, -50, 900, 700}, work, 320, 300);
    CHECK(p.top == 0 && p.bottom == 300);
    // Which edge changes the width.
    CHECK(WidthEdgeIsRight(PanelSide::OutsideRight));
    CHECK(!WidthEdgeIsRight(PanelSide::OutsideLeft));
    CHECK(!WidthEdgeIsRight(PanelSide::InsideRight));
}

void TestPanelRules() {
    PanelRules r;
    CHECK(r.Shown().empty());
    CHECK(!r.TakeChanged());

    // Selection before any view: ignored.
    r.Selection(1, L"C:\\a", true);
    CHECK(r.Shown().empty() && !r.TakeChanged());

    // Navigation (e.g. navigation pane click): the current folder.
    r.Navigated(L"C:\\Users", true, SelectionKey(0, L""));
    CHECK(r.Shown() == L"C:\\Users" && r.TakeChanged());
    CHECK(!r.TakeChanged());

    // Same selection as the baseline: no change.
    r.Selection(0, L"", false);
    CHECK(r.Shown() == L"C:\\Users" && !r.TakeChanged());

    // Selecting one folder: that folder.
    r.Selection(1, L"C:\\Users\\Public", true);
    CHECK(r.Shown() == L"C:\\Users\\Public" && r.TakeChanged());

    // Selecting a file: hide.
    r.Selection(1, L"C:\\Users\\x.txt", false);
    CHECK(r.Shown().empty() && r.TakeChanged());

    // Another folder, then empty space: hide.
    r.Selection(1, L"C:\\Users\\Default", true);
    CHECK(r.Shown() == L"C:\\Users\\Default");
    r.Selection(0, L"", false);
    CHECK(r.Shown().empty());

    // Two items selected: hide.
    r.Selection(1, L"C:\\Users\\Default", true);
    r.Selection(2, L"", false);
    CHECK(r.Shown().empty());

    // Back: Explorer selects the folder we came from; that is the baseline,
    // so the panel shows the current folder, not the selected one.
    r.Navigated(L"C:\\", true, SelectionKey(1, L"C:\\Users"));
    CHECK(r.Shown() == L"C:\\");
    r.Selection(1, L"C:\\Users", true);  // unchanged selection
    CHECK(r.Shown() == L"C:\\");
    r.Selection(1, L"C:\\Windows", true);  // a real click
    CHECK(r.Shown() == L"C:\\Windows");

    // Not a file-system folder (This PC, a new tab): hide.
    r.Navigated(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", false, SelectionKey(0, L""));
    CHECK(r.Shown().empty());
    // Selecting a drive there is a file-system folder: show it.
    r.Selection(1, L"D:\\", true);
    CHECK(r.Shown() == L"D:\\");

    // View lost (minimized, tab undeterminable): hide, and selections are
    // ignored until the next navigation.
    r.ViewLost();
    CHECK(r.Shown().empty());
    r.TakeChanged();
    r.Selection(1, L"C:\\a", true);
    CHECK(r.Shown().empty() && !r.TakeChanged());
    r.Navigated(L"C:\\a", true, SelectionKey(0, L""));
    CHECK(r.Shown() == L"C:\\a");
}

}  // namespace

int main() {
    TestFormat();
    TestSort();
    TestPalette();
    TestPlacement();
    TestPanelRules();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
