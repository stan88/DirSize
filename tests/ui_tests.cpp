// Unit tests for the front end's pure logic (Phase 5): size formatting,
// list sorting, palettes, panel placement and the panel rules. The panel
// rules and placement are ported from tools/explorer_probe.cpp, where they
// were settled in three VM rounds; these tests pin them.

#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "ui/format.h"
#include "ui/icon_key.h"
#include "ui/language.h"
#include "ui/list_model.h"
#include "ui/string_ids.h"
#include "ui/palette.h"
#include "ui/panel_rules.h"
#include "ui/placement.h"
#include "ui/scroll_geometry.h"
#include "ui/tree_model.h"
#include "ui/verb_command.h"

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

    // Fixed units (user request 2026-11-13): KB rounded up as File
    // Explorer does, GB with two decimals; both with thousands separators.
    CHECK(FormatSize(0, SizeUnit::KB) == L"0 KB");
    CHECK(FormatSize(1, SizeUnit::KB) == L"1 KB");
    CHECK(FormatSize(1024, SizeUnit::KB) == L"1 KB");
    CHECK(FormatSize(1025, SizeUnit::KB) == L"2 KB");
    CHECK(FormatSize(608811614208ull, SizeUnit::KB) == L"594,542,592 KB");
    CHECK(FormatSize(~0ull, SizeUnit::KB) == L"18,014,398,509,481,984 KB");  // no overflow
    CHECK(FormatSize(0, SizeUnit::GB) == L"0.00 GB");
    CHECK(FormatSize(1, SizeUnit::GB) == L"0.00 GB");
    CHECK(FormatSize(5ull << 30, SizeUnit::GB) == L"5.00 GB");
    CHECK(FormatSize((1234ull << 30) + (1ull << 29), SizeUnit::GB) == L"1,234.50 GB");
    CHECK(FormatSize((1000ull << 30) - 1, SizeUnit::GB) == L"1,000.00 GB");  // rounded up into a new group
    CHECK(FormatSize(999ull << 30, SizeUnit::GB) == L"999.00 GB");
    CHECK(FormatSize(~0ull, SizeUnit::GB) == L"17,179,869,184.00 GB");
    CHECK(FormatSize(1536, SizeUnit::Auto) == FormatBytes(1536));

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

    // Folders before files in every column and both directions, each group
    // sorted on its own (user request 2026-11-13).
    std::vector<ListItem> m = {Item(L"a.txt", 50, 50, 1), Item(L"Zeta", 10, 10, 1), Item(L"b.txt", 5, 5, 1),
                               Item(L"Alpha", 20, 20, 1)};
    m[1].isFolder = m[3].isFolder = true;
    SortItems(m, Column::Name, false);
    CHECK(Names(m) == L"Alpha Zeta a.txt b.txt ");
    SortItems(m, Column::Name, true);
    CHECK(Names(m) == L"Zeta Alpha b.txt a.txt ");
    SortItems(m, Column::Allocated, true);
    CHECK(Names(m) == L"Alpha Zeta a.txt b.txt ");
    SortItems(m, Column::Allocated, false);
    CHECK(Names(m) == L"Zeta Alpha b.txt a.txt ");
    SortItems(m, Column::Size, true);
    CHECK(Names(m) == L"Alpha Zeta a.txt b.txt ");
    static_assert(kDefaultSortColumn == Column::Name);
}

bool Has(const std::vector<Column>& v, Column c) { return std::find(v.begin(), v.end(), c) != v.end(); }

// The "show the Share column" setting (user request 2026-11-12).
void TestColumns() {
    const auto win = WindowColumns(true), winNo = WindowColumns(false);
    const auto pan = PanelColumns(true), panNo = PanelColumns(false);
    CHECK(win.size() == 5 && Has(win, Column::Share));
    CHECK(winNo.size() == 4 && !Has(winNo, Column::Share));
    CHECK(Has(winNo, Column::Size) && Has(winNo, Column::Allocated) && Has(winNo, Column::Files));
    CHECK(pan.size() == 3 && pan[0] == Column::Name && pan[1] == Column::Allocated && pan[2] == Column::Share);
    CHECK(panNo.size() == 2 && panNo[0] == Column::Name && panNo[1] == Column::Allocated);
    for (const auto* v : {&win, &winNo, &pan, &panNo}) CHECK(!v->empty() && v->front() == Column::Name);

    // Sorting by Share and hiding it: Allocated (same key), not Size.
    CHECK(FallbackSortColumn(Column::Share, winNo) == Column::Allocated);
    CHECK(FallbackSortColumn(Column::Share, panNo) == Column::Allocated);
    CHECK(FallbackSortColumn(Column::Share, win) == Column::Share);  // still shown
    CHECK(FallbackSortColumn(Column::Files, panNo) == Column::Allocated);  // second column
    CHECK(FallbackSortColumn(Column::Size, pan) == Column::Allocated);
    CHECK(FallbackSortColumn(Column::Name, panNo) == Column::Name);
    CHECK(FallbackSortColumn(Column::Share, {Column::Name}) == Column::Name);

    // Panel width: narrower by the share column when it is hidden, and the
    // saved width (with the column) survives turning it off and on.
    const int share = ColumnWidthDip(Column::Share, true);
    CHECK(share == 112);
    CHECK(PanelShownWidthDip(340, true, share) == 340);
    CHECK(PanelShownWidthDip(340, false, share) == 340 - share);
    CHECK(PanelSavedWidthDip(PanelShownWidthDip(340, false, share), false, share) == 340);
    CHECK(PanelShownWidthDip(240, false, share) == kPanelMinWidthNoShareDip);  // 128 would be too narrow
    CHECK(PanelShownWidthDip(900, false, share) == 900 - share);
    CHECK(PanelShownWidthDip(5000, true, share) == kPanelMaxWidthDip);
    CHECK(PanelShownWidthDip(10, true, share) == kPanelMinWidthDip);
    // Resized while hidden: saved with the column added back, within limits.
    CHECK(PanelSavedWidthDip(200, false, share) == 200 + share);
    CHECK(PanelSavedWidthDip(kPanelMinWidthNoShareDip, false, share) == kPanelMinWidthDip + 32);
    CHECK(PanelShownWidthDip(PanelSavedWidthDip(kPanelMinWidthNoShareDip, false, share), false, share) ==
          kPanelMinWidthNoShareDip);
    CHECK(PanelSavedWidthDip(PanelMaxWidthDip(false, share), false, share) == kPanelMaxWidthDip);
    CHECK(PanelSavedWidthDip(300, true, share) == 300);
    for (int w = kPanelMinWidthNoShareDip; w <= PanelMaxWidthDip(false, share); w += 7) {
        CHECK(PanelShownWidthDip(PanelSavedWidthDip(w, false, share), false, share) == w);
    }
    CHECK(PanelMinWidthDip(true) == kPanelMinWidthDip && PanelMinWidthDip(false) == kPanelMinWidthNoShareDip);
    // A share column the user widened: the panel narrows by that width.
    CHECK(PanelShownWidthDip(500, false, 200) == 300);
    CHECK(PanelSavedWidthDip(300, false, 200) == 500);
    CHECK(PanelShownWidthDip(500, true, 200) == 500);  // shown: the user's width is inside
    CHECK(PanelShownWidthDip(900, false, 9999) == 900 - kColumnMaxDip);  // a bad width is clamped
    CHECK(PanelShownWidthDip(500, false, 9999) == kPanelMinWidthNoShareDip);
}

// Column widths the user dragged, saved as text (settings "PanelColumns").
void TestColumnWidths() {
    auto w = ParseColumnWidths(L"Allocated=96;Share=150");
    CHECK(w.size() == 2 && w[0].column == Column::Allocated && w[0].dip == 96 && w[1].column == Column::Share &&
          w[1].dip == 150);
    CHECK(FormatColumnWidths(w) == L"Allocated=96;Share=150");
    CHECK(ParseColumnWidths(L"").empty());
    CHECK(ParseColumnWidths(L";;;").empty());
    // Name: the window's name column, with a larger limit.
    auto name = ParseColumnWidths(L"Name=600");
    CHECK(name.size() == 1 && name[0].column == Column::Name && name[0].dip == 600);
    CHECK(ParseColumnWidths(L"Name=1200").size() == 1);
    CHECK(ParseColumnWidths(L"Name=1201").empty());
    CHECK(ParseColumnWidths(L"Name=39").empty());
    CHECK(FormatColumnWidths({{Column::Name, 5000}}) == L"Name=1200");
    CHECK(ColumnMaxDip(Column::Name) == kNameColumnMaxDip && ColumnMaxDip(Column::Size) == kColumnMaxDip);
    // Skipped: unknown names, bad numbers, limits.
    CHECK(ParseColumnWidths(L"Bogus=100").empty());
    CHECK(ParseColumnWidths(L"Size=abc;Files=-5;Share=").empty());
    CHECK(ParseColumnWidths(L"Size=39").empty());
    CHECK(ParseColumnWidths(L"Size=401").empty());
    CHECK(ParseColumnWidths(L"Size=99999999999999999999").empty());
    CHECK(ParseColumnWidths(L"size=100").empty());  // names are exact
    auto edge = ParseColumnWidths(L"Size=40;Files=400");
    CHECK(edge.size() == 2 && edge[0].dip == kColumnMinDip && edge[1].dip == kColumnMaxDip);
    // A damaged entry loses only itself; a repeated column keeps the last.
    auto mixed = ParseColumnWidths(L"Size=80;garbage;Files=x;Size=90;Share=120");
    CHECK(mixed.size() == 2 && mixed[0].column == Column::Size && mixed[0].dip == 90 &&
          mixed[1].column == Column::Share && mixed[1].dip == 120);
    // Dragging a separator (right edge of the column): right = wider.
    CHECK(DraggedWidthDip(80, 0, 96, true) == 80);
    CHECK(DraggedWidthDip(80, 20, 96, true) == 100);
    CHECK(DraggedWidthDip(80, -20, 96, true) == 60);
    CHECK(DraggedWidthDip(80, 35, 168, true) == 100);  // 35 px at 175% = 20 DIPs
    CHECK(DraggedWidthDip(80, -35, 168, true) == 60);
    CHECK(DraggedWidthDip(80, -1000, 96, true) == kColumnMinDip);
    CHECK(DraggedWidthDip(80, 1000, 96, true) == kColumnMaxDip);
    CHECK(DraggedWidthDip(0, -50, 96, false) == -50);  // the name column: a change, not clamped
    CHECK(DraggedWidthDip(0, 175, 168, false) == 100);
    CHECK(DraggedWidthDip(80, 20, 0, true) == 100);    // dpi 0 taken as 96
    CHECK(DraggedWidthDip(300, 192, 96, true, kNameColumnMaxDip) == 492);  // name in the window
    CHECK(DraggedWidthDip(1100, 500, 96, true, kNameColumnMaxDip) == kNameColumnMaxDip);
    CHECK(FormatColumnWidths({}) == L"");
    CHECK(FormatColumnWidths({{Column::Files, 5}}) == L"Files=40");  // clamped on writing too
    const std::vector<ColumnWidth> all = {
        {Column::Size, 41}, {Column::Allocated, 222}, {Column::Share, 333}, {Column::Files, 400}};
    auto back = ParseColumnWidths(FormatColumnWidths(all));
    CHECK(back.size() == all.size());
    for (size_t i = 0; i < all.size() && i < back.size(); ++i) {
        CHECK(back[i].column == all[i].column && back[i].dip == all[i].dip);
    }

    // Layout: a fill column takes the rest; fixed ones keep their widths
    // and may add up to more than the list (it then scrolls sideways).
    auto sum = [](const std::vector<int>& v) { int s = 0; for (int x : v) s += x; return s; };
    auto fill = LayoutColumns({-1, 90, 96}, 96, 500, 8, 120);
    CHECK(fill.size() == 3 && fill[0] == 500 - 8 - 90 - 96 && fill[1] == 90 && fill[2] == 96);
    CHECK(sum(fill) + 8 == 500);  // fills exactly
    auto narrow = LayoutColumns({-1, 90, 96}, 96, 200, 8, 120);
    CHECK(narrow[0] == 120 && sum(narrow) + 8 > 200);  // the minimum wins: wider than the list
    auto fixed = LayoutColumns({300, 90, 96}, 96, 1000, 8, 120);
    CHECK(fixed[0] == 300 && sum(fixed) + 8 == 494);  // blank space at the right
    auto scaled = LayoutColumns({300, 90}, 168, 2000, 14, 210);
    CHECK(scaled[0] == 525 && scaled[1] == 158);  // 175%: 300 -> 525, 90 -> 157.5 -> 158
    // One column wider: only it changes, the others keep their widths.
    auto before = LayoutColumns({300, 90, 96, 150}, 96, 900, 8, 120);
    auto after = LayoutColumns({300, 120, 96, 150}, 96, 900, 8, 120);
    CHECK(after[0] == before[0] && after[2] == before[2] && after[3] == before[3] && after[1] == before[1] + 30);
    CHECK(LayoutColumns({}, 96, 500, 8, 120).empty());
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

    // Scroll bar thumb: system colors only in high contrast; otherwise set,
    // different from the background, and the active one differs too.
    CHECK(hc.scrollThumb == sys.windowText && hc.scrollThumbActive == sys.highlight);
    for (const Palette& p : {dark, light}) {
        CHECK(p.scrollThumb != p.background && p.scrollThumbActive != p.background);
        CHECK(p.scrollThumb != p.scrollThumbActive);
    }
}

void TestScrollGeometry() {
    // Track 100..300 (200 px), 100 rows, 20 visible: thumb 40 px.
    ThumbSpan t = ComputeThumb(100, 200, 100, 20, 0, 24);
    CHECK(t.visible && t.top == 100 && t.length == 40);
    t = ComputeThumb(100, 200, 100, 20, 80, 24);  // last position
    CHECK(t.top == 260 && t.top + t.length == 300);
    t = ComputeThumb(100, 200, 100, 20, 40, 24);  // half way
    CHECK(t.top == 180);
    t = ComputeThumb(100, 200, 100, 20, 500, 24);  // beyond the end: clamped
    CHECK(t.top == 260);
    // Everything fits, nothing to scroll, no track: no thumb.
    CHECK(!ComputeThumb(0, 200, 20, 20, 0, 24).visible);
    CHECK(!ComputeThumb(0, 200, 0, 20, 0, 24).visible);
    CHECK(!ComputeThumb(0, 0, 100, 20, 0, 24).visible);
    // Very many rows: the minimum length keeps the thumb grabbable.
    t = ComputeThumb(0, 200, 1000000, 20, 0, 24);
    CHECK(t.length == 24);
    t = ComputeThumb(0, 200, 1000000, 20, 1000000 - 20, 24);
    CHECK(t.top + t.length == 200);
    // A track shorter than the minimum: the thumb fills it.
    t = ComputeThumb(0, 10, 100, 20, 50, 24);
    CHECK(t.length == 10 && t.top == 0);

    // Dragging maps back to rows, rounded, clamped to the track.
    CHECK(PositionFromThumbTop(100, 200, 100, 20, 100, 24) == 0);
    CHECK(PositionFromThumbTop(100, 200, 100, 20, 260, 24) == 80);
    CHECK(PositionFromThumbTop(100, 200, 100, 20, 180, 24) == 40);
    CHECK(PositionFromThumbTop(100, 200, 100, 20, 50, 24) == 0);
    CHECK(PositionFromThumbTop(100, 200, 100, 20, 999, 24) == 80);
    CHECK(PositionFromThumbTop(100, 200, 100, 20, 181, 24) == 41);  // 81 px * 80 / 160 = 40.5 -> 41
    CHECK(PositionFromThumbTop(0, 10, 100, 20, 5, 24) == 0);       // no travel
    // Round trip at every position.
    bool roundTrip = true;
    for (int pos = 0; pos <= 80; ++pos) {
        ThumbSpan s = ComputeThumb(100, 200, 100, 20, pos, 24);
        if (PositionFromThumbTop(100, 200, 100, 20, s.top, 24) != pos) roundTrip = false;
    }
    CHECK(roundTrip);

    // Hit testing around a thumb at 180..220.
    CHECK(HitScroll(100, 200, 100, 20, 40, 24, 99) == ScrollHit::None);
    CHECK(HitScroll(100, 200, 100, 20, 40, 24, 100) == ScrollHit::Above);
    CHECK(HitScroll(100, 200, 100, 20, 40, 24, 179) == ScrollHit::Above);
    CHECK(HitScroll(100, 200, 100, 20, 40, 24, 180) == ScrollHit::Thumb);
    CHECK(HitScroll(100, 200, 100, 20, 40, 24, 219) == ScrollHit::Thumb);
    CHECK(HitScroll(100, 200, 100, 20, 40, 24, 220) == ScrollHit::Below);
    CHECK(HitScroll(100, 200, 100, 20, 40, 24, 299) == ScrollHit::Below);
    CHECK(HitScroll(100, 200, 100, 20, 40, 24, 300) == ScrollHit::None);
    CHECK(HitScroll(100, 200, 20, 20, 0, 24, 150) == ScrollHit::None);  // nothing scrolls
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

    // The sequence from the --trace run (Windows 11): a folder selected in
    // the view, then the current folder clicked in the navigation pane. No
    // navigation; the view's selection is cleared and the focus is in the
    // tree: the panel shows the current folder again instead of hiding.
    PanelRules t;
    t.Navigated(L"D:\\P", true, SelectionKey(0, L""));
    t.Selection(1, L"D:\\P\\a", true);
    CHECK(t.Shown() == L"D:\\P\\a");
    t.Selection(0, L"", false, true);
    CHECK(t.Shown() == L"D:\\P");
    t.Selection(1, L"D:\\P\\b", true);  // back in the view: a click shows it
    CHECK(t.Shown() == L"D:\\P\\b");
    // Selection cleared first, focus reaches the tree a tick later: the
    // focus change alone brings the current folder back.
    t.Selection(0, L"", false, false);
    CHECK(t.Shown().empty());
    t.Selection(0, L"", false, true);
    CHECK(t.Shown() == L"D:\\P");
    // Clicking empty space in the view still hides.
    t.Selection(0, L"", false, false);
    CHECK(t.Shown().empty());
    // A navigation done in the tree starts with the focus there: staying in
    // the tree changes nothing.
    t.Navigated(L"D:\\Q", true, SelectionKey(0, L"", true));
    CHECK(t.Shown() == L"D:\\Q");
    t.TakeChanged();
    t.Selection(0, L"", false, true);
    CHECK(t.Shown() == L"D:\\Q" && !t.TakeChanged());
    // The current folder not on disk (This PC): the tree rule shows nothing.
    t.Navigated(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", false, SelectionKey(1, L"C:\\"));
    t.Selection(0, L"", false, true);
    CHECK(t.Shown().empty());
    // A file selected while the focus is in the tree: still hidden.
    t.Navigated(L"D:\\Q", true, SelectionKey(0, L""));
    t.Selection(1, L"D:\\Q\\f.txt", false, true);
    CHECK(t.Shown().empty());
    CHECK(SelectionKey(0, L"") != SelectionKey(0, L"", true));
}

using Paths = std::vector<std::wstring>;

// Parses a command line the way the program's own start-up does.
std::wstring FirstArgument(const std::wstring& commandLine) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &argc);
    std::wstring arg = argv && argc >= 2 ? argv[1] : L"";
    if (argv) LocalFree(argv);
    return arg;
}

std::wstring Expand(std::wstring command, const std::wstring& path) {  // what Explorer does with %1
    command.replace(command.find(L"%1"), 2, path);
    return command;
}

void TestVerbCommand() {
    const std::wstring exe = L"C:\\Program Files\\DirSize\\dirsize.exe";
    const std::wstring command = VerbCommandLine(exe);
    CHECK(command == L"\"C:\\Program Files\\DirSize\\dirsize.exe\" \"%1\"");
    CHECK(PanelCommandLine(exe) == L"\"C:\\Program Files\\DirSize\\dirsize.exe\" --panel");

    // Through the documented parsing rules (CommandLineToArgvW) and back.
    auto roundTrip = [&](const std::wstring& path) {
        return PathFromArgument(FirstArgument(Expand(command, path)));
    };
    CHECK(roundTrip(L"D:\\Some Folder") == L"D:\\Some Folder");
    CHECK(roundTrip(L"C:\\Users") == L"C:\\Users");
    CHECK(roundTrip(L"D:\\a b\\c d") == L"D:\\a b\\c d");
    // A drive: "C:\" ends in backslash-quote, which the parser turns into
    // a literal quotation mark.
    CHECK(FirstArgument(Expand(command, L"C:\\")) == L"C:\"");
    CHECK(roundTrip(L"C:\\") == L"C:\\");
    CHECK(roundTrip(L"E:\\") == L"E:\\");
    // Typed by hand: unchanged.
    CHECK(PathFromArgument(L"C:\\Windows") == L"C:\\Windows");
    CHECK(PathFromArgument(L"") == L"");
}

void TestTreePaths() {
    std::wstring root;
    std::vector<std::wstring> parts;
    CHECK(SplitTreePath(L"C:", root, parts) && root == L"C:\\" && parts.empty());
    CHECK(SplitTreePath(L"c:\\", root, parts) && root == L"C:\\" && parts.empty());
    CHECK(SplitTreePath(L"d:\\x\\\\y\\", root, parts) && root == L"D:\\" && parts == Paths({L"x", L"y"}));
    CHECK(!SplitTreePath(L"\\\\server\\share\\x", root, parts));
    CHECK(!SplitTreePath(L"C:x", root, parts));  // relative to C:'s current folder
    CHECK(!SplitTreePath(L"1:\\", root, parts));
    CHECK(!SplitTreePath(L"", root, parts));

    // The listing rule: the service index's rule (name surrogates out).
    CHECK(IsTreeFolder(FILE_ATTRIBUTE_DIRECTORY, 0, L"Users"));
    CHECK(IsTreeFolder(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, 0, L"$Recycle.Bin"));
    CHECK(!IsTreeFolder(FILE_ATTRIBUTE_ARCHIVE, 0, L"file.txt"));
    CHECK(!IsTreeFolder(FILE_ATTRIBUTE_DIRECTORY, 0, L"."));
    CHECK(!IsTreeFolder(FILE_ATTRIBUTE_DIRECTORY, 0, L".."));
    CHECK(IsTreeFolder(FILE_ATTRIBUTE_DIRECTORY, 0, L"..x"));
    CHECK(IsTreeFolder(FILE_ATTRIBUTE_DIRECTORY, 0, L".git"));
    const DWORD reparseDir = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    CHECK(!IsTreeFolder(reparseDir, IO_REPARSE_TAG_MOUNT_POINT, L"Documents and Settings"));  // junction
    CHECK(!IsTreeFolder(reparseDir, IO_REPARSE_TAG_SYMLINK, L"link"));
    CHECK(IsTreeFolder(reparseDir, IO_REPARSE_TAG_DEDUP, L"dedup"));  // not a name surrogate: an ordinary folder
    // The tag field means nothing without the reparse attribute.
    CHECK(IsTreeFolder(FILE_ATTRIBUTE_DIRECTORY, IO_REPARSE_TAG_MOUNT_POINT, L"plain"));
}

void TestTreeModel() {
    TreeModel m;
    CHECK(m.SetRoots({{L"C:\\", L"System (C:)"}, {L"D:\\", L""}, {L"bad", L""}}).empty());
    CHECK(m.Rows().size() == 2);  // "bad" is not a drive root
    TreeNode* c = m.Find(L"c:");
    CHECK(c && c->label == L"System (C:)" && c->HasExpander() && !c->expanded);
    CHECK(m.Find(L"\\\\server\\share") == nullptr);
    if (!c) return;  // the rest uses the node

    // Expanding lists the folder; children arrive in name order (digits as
    // numbers), duplicates by case dropped, and the rows follow.
    TreeAction a = m.Toggle(c);
    CHECK(a.loads == Paths({L"C:\\"}) && !a.selectionChanged && c->expanded);
    CHECK(c->state == ChildState::Loading && c->HasExpander());
    CHECK(m.Toggle(c).loads.empty() && !c->expanded);  // collapse while loading
    CHECK(m.Toggle(c).loads.empty() && c->expanded);   // already loading: no second listing
    CHECK(m.SetChildren(L"C:\\", {L"file10", L"Users", L"file2", L"users"}).empty());
    CHECK(c->state == ChildState::Loaded && c->children.size() == 3);
    CHECK(c->children[0]->name == L"file2" && c->children[1]->name == L"file10");
    CHECK(m.Rows().size() == 5 && m.Rows()[1].depth == 1 && m.Rows()[4].node->name == L"D:\\");
    CHECK(TreeModel::PathOf(c->children[2].get()) == L"C:\\Users");  // the first spelling listed wins

    // Keyboard: Down from nothing selects the first row; Right expands,
    // then goes to the first child; Left goes to the parent, then collapses.
    a = m.Key(TreeKey::Down, 10);
    CHECK(a.selectionChanged && m.Selected() == c);
    a = m.Key(TreeKey::Right, 10);
    CHECK(a.selectionChanged && m.Selected() == c->children[0].get());
    a = m.Key(TreeKey::Right, 10);  // file2: not listed yet -> expand
    CHECK(!a.selectionChanged && a.loads == Paths({L"C:\\file2"}));
    CHECK(m.SetChildren(L"C:\\file2", {}).empty());
    TreeNode* file2 = m.Find(L"C:\\file2");
    CHECK(file2 && !file2->HasExpander() && !file2->expanded);  // empty: expander gone
    CHECK(m.Toggle(file2).loads.empty() && !file2->expanded);
    a = m.Key(TreeKey::Left, 10);
    CHECK(a.selectionChanged && m.Selected() == c);
    a = m.Key(TreeKey::Left, 10);
    CHECK(!a.selectionChanged && !c->expanded && m.Rows().size() == 2);
    a = m.Key(TreeKey::Left, 10);  // a collapsed root: nothing
    CHECK(!a.selectionChanged && a.loads.empty());
    a = m.Key(TreeKey::End, 10);
    CHECK(a.selectionChanged && m.Selected()->name == L"D:\\");
    a = m.Key(TreeKey::Down, 10);  // already last
    CHECK(!a.selectionChanged);
    a = m.Key(TreeKey::PageUp, 10);
    CHECK(a.selectionChanged && m.Selected() == c);
    CHECK(!m.Click(c).selectionChanged);  // same node again
    m.Toggle(c);

    // Collapsing above the selection moves it up (and reports it).
    TreeNode* users = m.Find(L"C:\\USERS");
    CHECK(users && m.Click(users).selectionChanged);
    a = m.Toggle(c);
    CHECK(a.selectionChanged && m.Selected() == c);
    m.Toggle(c);

    // A new listing keeps the nodes still there with their expansion; the
    // selected folder disappearing moves the selection to its parent.
    CHECK(m.Toggle(users).loads == Paths({L"C:\\Users"}));
    CHECK(m.SetChildren(L"C:\\Users", {L"a", L"b"}).empty());
    m.Click(m.Find(L"C:\\Users\\b"));
    CHECK(m.SetChildren(L"C:\\", {L"Users", L"New"}).empty());
    CHECK(m.Find(L"C:\\Users") == users && users->expanded && users->children.size() == 2);
    CHECK(m.Find(L"C:\\file2") == nullptr && m.Find(L"C:\\New") != nullptr);
    CHECK(m.SetChildren(L"C:\\Users", {L"a"}).empty());
    CHECK(m.Selected() == users && m.TakeScrollRequest() && !m.TakeScrollRequest());
    CHECK(m.SetChildren(L"C:\\nowhere", {L"x"}).empty());  // a node that is gone: ignored

    // Refresh: a folder never listed and collapsed is listed on expand.
    CHECK(m.Refresh(L"C:\\New").empty());
    CHECK(m.Refresh(L"C:\\Users") == Paths({L"C:\\Users"}) && users->HasExpander());
    CHECK(m.Refresh(L"C:\\Users").empty());  // already loading
    CHECK(m.SetFailed(L"C:\\Users").empty());
    CHECK(users->state == ChildState::Failed && !users->HasExpander() && users->children.empty());
    CHECK(m.Selected() == users);
    CHECK(m.Refresh(L"C:\\Users") == Paths({L"C:\\Users"}));  // F5 tries again
    m.SetChildren(L"C:\\Users", {L"a"});

    // Roots: a drive that goes away takes the selection with it; one that
    // stays keeps its nodes.
    m.Click(m.Find(L"D:\\"));
    CHECK(m.SetRoots({{L"C:\\", L"New label"}}).empty());
    CHECK(m.Selected() == nullptr && m.Find(L"C:\\Users") == users && m.Find(L"C:\\")->label == L"New label");
}

void TestTreeReveal() {
    TreeModel m;
    m.SetRoots({{L"C:\\", L""}, {L"D:\\", L""}});

    // Revealing lists the ancestors one at a time, matching names without
    // case, and never reports a selection change (no navigation back).
    Paths loads = m.Reveal(L"c:\\users\\Me\\Docs\\");
    CHECK(loads == Paths({L"C:\\"}) && m.Selected() == m.Find(L"C:\\") && m.TakeScrollRequest());
    CHECK(m.Find(L"C:\\")->expanded);
    CHECK(m.SetChildren(L"C:\\", {L"Windows", L"Users"}) == Paths({L"C:\\Users"}));
    CHECK(m.Selected() == m.Find(L"C:\\Users"));
    CHECK(m.SetChildren(L"C:\\Users", {L"me", L"Public"}) == Paths({L"C:\\Users\\me"}));
    CHECK(m.SetChildren(L"C:\\Users\\me", {L"Docs", L"Music"}).empty());
    TreeNode* docs = m.Find(L"C:\\Users\\me\\Docs");
    CHECK(docs && m.Selected() == docs && !docs->expanded && docs->state == ChildState::NotLoaded);
    CHECK(TreeModel::PathOf(docs) == L"C:\\Users\\me\\Docs");  // the listing's spelling
    CHECK(m.Find(L"C:\\Users")->expanded && m.Find(L"C:\\Users\\me")->expanded);
    CHECK(m.RowOf(docs) == 3);  // C:, Users, me, Docs, Music, Public, Windows, D:
    CHECK(m.Rows().size() == 8);
    // The reveal is over: later listings do not move the selection.
    CHECK(m.SetChildren(L"C:\\Users\\me", {L"Docs", L"Music", L"Pictures"}).empty() && m.Selected() == docs);

    // Everything already listed: selected at once, no loads, and collapsed
    // ancestors are opened again so the selection is visible.
    m.Toggle(m.Find(L"C:\\Users"));
    m.Toggle(m.Find(L"C:\\"));
    CHECK(m.RowOf(docs) == -1 && m.Rows().size() == 2);
    CHECK(m.Reveal(L"C:\\Users\\Public").empty() && m.Selected() == m.Find(L"C:\\Users\\Public"));
    CHECK(m.Find(L"C:\\")->expanded && m.Find(L"C:\\Users")->expanded && m.RowOf(m.Selected()) == 6);
    CHECK(m.Reveal(L"C:\\").empty() && m.Selected() == m.Find(L"C:\\"));

    // A folder missing from its parent's listing: stop at the parent.
    CHECK(m.Reveal(L"C:\\Users\\ghost\\x").empty() && m.Selected() == m.Find(L"C:\\Users"));
    CHECK(m.SetChildren(L"C:\\Users", {L"me", L"Public", L"ghost"}).empty());  // reveal ended: no resume
    CHECK(m.Selected() == m.Find(L"C:\\Users"));

    // A parent that cannot be listed: stop there.
    CHECK(m.Reveal(L"D:\\locked\\inner") == Paths({L"D:\\"}));
    CHECK(m.SetChildren(L"D:\\", {L"locked"}) == Paths({L"D:\\locked"}));
    CHECK(m.SetFailed(L"D:\\locked").empty() && m.Selected() == m.Find(L"D:\\locked"));

    // Outside the drives: no selection, no loads.
    CHECK(m.Reveal(L"\\\\server\\share\\x").empty() && m.Selected() == nullptr);

    // A user action ends a reveal in progress: a late listing does not
    // move the selection away from what the user chose.
    CHECK(m.Reveal(L"C:\\Windows\\System32") == Paths({L"C:\\Windows"}));
    TreeNode* users = m.Find(L"C:\\Users");
    CHECK(m.Click(users).selectionChanged);
    CHECK(m.SetChildren(L"C:\\Windows", {L"System32"}).empty() && m.Selected() == users);

    // A new reveal replaces an older one still waiting.
    CHECK(m.Reveal(L"C:\\Windows\\System32\\drivers") == Paths({L"C:\\Windows\\System32"}));
    CHECK(m.Reveal(L"C:\\Users\\Public").empty() && m.Selected() == m.Find(L"C:\\Users\\Public"));
    CHECK(m.SetChildren(L"C:\\Windows\\System32", {L"drivers"}).empty());
    CHECK(m.Selected() == m.Find(L"C:\\Users\\Public"));

    // A drive that is not in the list yet: the reveal waits for SetRoots.
    CHECK(m.Reveal(L"E:\\data").empty() && m.Selected() == nullptr);
    CHECK(m.SetRoots({{L"C:\\", L""}, {L"D:\\", L""}, {L"E:\\", L""}}) == Paths({L"E:\\"}));
    CHECK(m.SetChildren(L"E:\\", {L"data"}).empty() && m.Selected() == m.Find(L"E:\\data"));
}

// Icons by type (user request 2026-11-13, option A).
void TestIconKey() {
    CHECK(IconKey(L"Windows", true) == L"<folder>");
    CHECK(IconKey(L"a.b.c", true) == L"<folder>");  // a folder with dots
    CHECK(IconKey(L"Report.DOCX", false) == L".docx");
    CHECK(IconKey(L"archive.tar.gz", false) == L".gz");
    CHECK(IconKey(L".gitignore", false) == L".gitignore");
    CHECK(IconKey(L"README", false) == L"<file>");
    CHECK(IconKey(L"name.", false) == L"<file>");
    CHECK(IconKey(L"", false) == L"<file>");
    CHECK(DriveIconKey(L"C:\\") == L"<drive>C:\\");
}

void TestFormatPattern() {
    CHECK(FormatPattern(L"Total %1", {L"5 MB"}) == L"Total 5 MB");
    CHECK(FormatPattern(L"%2 (%1)", {L"C:", L"Disk"}) == L"Disk (C:)");
    CHECK(FormatPattern(L"%1 files", {L"%2"}) == L"%2 files");  // one pass
    CHECK(FormatPattern(L"%1 %3 100%", {L"a"}) == L"a %3 100%");
    CHECK(FormatPattern(L"%", {}) == L"%");
    CHECK(FormatPattern(L"%0%9", {L"x"}) == L"%0%9");
}

void TestLanguagePick() {
    CHECK(TagMatches(L"en-GB", L"en") && TagMatches(L"EN", L"en") && TagMatches(L"zh-Hant-TW", L"zh-Hant"));
    CHECK(!TagMatches(L"zh-CN", L"zh-TW") && !TagMatches(L"eng", L"en") && !TagMatches(L"zh", L"zh-TW"));
    CHECK(!TagMatches(L"en", L""));

    const wchar_t* const en[] = {L"en", nullptr};
    const wchar_t* const zh[] = {L"zh-TW", L"zh-HK", L"zh-MO", L"zh-Hant", nullptr};
    const wchar_t* const ja[] = {L"ja", nullptr};
    const LanguageEntry table[] = {{L"en-US", 0x0409, en}, {L"zh-TW", 0x0404, zh}, {L"ja-JP", 0x0411, ja}};
    using V = std::vector<std::wstring>;
    CHECK(PickLanguage(V{L"zh-TW"}, table) == 1);
    CHECK(PickLanguage(V{L"zh-HK"}, table) == 1);
    CHECK(PickLanguage(V{L"zh-Hant-TW"}, table) == 1);
    CHECK(PickLanguage(V{L"zh-CN"}, table) == 0);              // simplified: skipped, English
    CHECK(PickLanguage(V{L"zh-CN", L"zh-TW"}, table) == 1);    // next in the list
    CHECK(PickLanguage(V{L"en-GB"}, table) == 0);
    CHECK(PickLanguage(V{L"ja-JP", L"zh-TW"}, table) == 2);
    CHECK(PickLanguage(V{L"fr-FR", L"zh-TW"}, table) == 1);
    CHECK(PickLanguage(V{L"fr-FR"}, table) == 0);
    CHECK(PickLanguage(V{}, table) == 0);
    CHECK(FindLanguage(L"ZH-tw", table) == 1 && FindLanguage(L"", table) == 3 && FindLanguage(L"zh", table) == 3);
    const LanguageEntry noEnglish[] = {{L"zh-TW", 0x0404, zh}};
    CHECK(PickLanguage(V{L"fr-FR"}, noEnglish) == 1);  // table.size(): nothing to pick

    // The built-in table: en-US is there, and the zh-TW rule is the decided one.
    auto built = Languages();
    CHECK(FindLanguage(L"en-US", built) < built.size());
    CHECK(PickLanguage(V{L"zh-HK"}, built) == FindLanguage(L"zh-TW", built));
    CHECK(PickLanguage(V{L"zh-CN"}, built) == FindLanguage(L"en-US", built));
}

// Each lang/<locale>.json: its language/sublanguage names give the same
// LANGID Windows has for its locale name. A wrong pair would compile but its
// table would not be picked (DEVLOG 2026-11-06).
void TestLanguageIds() {
    for (const auto& e : Languages()) {
        LCID lcid = LocaleNameToLCID(e.locale, 0);
        CHECK(lcid != 0 && LANGIDFROMLCID(lcid) == e.langId);
    }
}

struct ExpectedString {
    const wchar_t* locale;
    UINT id;
    const char* text;  // UTF-8 (/utf-8)
};

const ExpectedString kExpected[] = {
#include "lang_expected.inc"
};

std::wstring Wide(const char* utf8) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (n <= 0) return L"<bad utf-8>";
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
    w.resize(static_cast<size_t>(n) - 1);
    return w;
}

// Every string of every language loads from the compiled tables when that
// language is preferred, exactly as written in the JSON file (tabs included).
void TestStringTables() {
    size_t perLanguage = 0;
    for (const auto& e : kExpected) perLanguage += wcscmp(e.locale, kExpected[0].locale) == 0 ? 1 : 0;
    CHECK(perLanguage > 0 && perLanguage * Languages().size() == std::size(kExpected));
    for (const auto& lang : Languages()) {
        std::wstring list = lang.locale;
        list.append(2, L'\0');
        ULONG count = 0;
        CHECK(SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, list.c_str(), &count) && count == 1);
        int bad = 0;
        for (const auto& e : kExpected) {
            if (wcscmp(e.locale, lang.locale) != 0) continue;
            const wchar_t* text = nullptr;
            int length = LoadStringW(GetModuleHandleW(nullptr), e.id, reinterpret_cast<LPWSTR>(&text), 0);
            std::wstring got = length > 0 && text ? std::wstring(text, static_cast<size_t>(length)) : L"";
            if (got != Wide(e.text) || got.empty()) {
                std::printf("  string %u of %ls differs\n", e.id, lang.locale);
                ++bad;
            }
        }
        CHECK(bad == 0);
    }
    SetThreadPreferredUILanguages(0, nullptr, nullptr);

    // The "..." menu switches the language of a running process: a second
    // SetProcessPreferredUILanguages call must take effect, on this thread
    // and on a new one.
    auto expectedText = [](const wchar_t* locale, UINT id) {
        for (const auto& e : kExpected) {
            if (e.id == id && wcscmp(e.locale, locale) == 0) return Wide(e.text);
        }
        return std::wstring(L"<none>");
    };
    auto loaded = [](UINT id) {
        const wchar_t* text = nullptr;
        int length = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&text), 0);
        return length > 0 && text ? std::wstring(text, static_cast<size_t>(length)) : std::wstring();
    };
    for (const wchar_t* locale : {L"en-US", L"zh-TW", L"en-US"}) {
        std::wstring list = locale;
        list.append(2, L'\0');
        ULONG count = 0;
        CHECK(SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, list.c_str(), &count) && count == 1);
        CHECK(loaded(IDS_COL_NAME) == expectedText(locale, IDS_COL_NAME));
        std::wstring fromThread;
        std::thread([&] { fromThread = loaded(IDS_COL_NAME); }).join();
        CHECK(fromThread == expectedText(locale, IDS_COL_NAME));
    }
    // Spot checks that the JSON escapes survived (a menu accelerator tab).
    std::wstring pick = Wide(kExpected[0].text);
    CHECK(!pick.empty());
    bool hasTab = false;
    for (const auto& e : kExpected) {
        if (e.id == IDS_MENU_REFRESH && Wide(e.text).find(L'\t') != std::wstring::npos) hasTab = true;
    }
    CHECK(hasTab);
}

}  // namespace

int main() {
    TestFormat();
    TestSort();
    TestColumns();
    TestColumnWidths();
    TestPalette();
    TestPlacement();
    TestPanelRules();
    TestScrollGeometry();
    TestVerbCommand();
    TestTreePaths();
    TestTreeModel();
    TestTreeReveal();
    TestIconKey();
    TestFormatPattern();
    TestLanguagePick();
    TestLanguageIds();
    TestStringTables();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
