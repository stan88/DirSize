// Unit tests for the front end's pure logic (Phase 5): size formatting,
// list sorting, palettes, panel placement and the panel rules. The panel
// rules and placement are ported from tools/explorer_probe.cpp, where they
// were settled in three VM rounds; these tests pin them.

#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "ui/format.h"
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
    TestPalette();
    TestPlacement();
    TestPanelRules();
    TestScrollGeometry();
    TestVerbCommand();
    TestTreePaths();
    TestTreeModel();
    TestTreeReveal();
    TestFormatPattern();
    TestLanguagePick();
    TestLanguageIds();
    TestStringTables();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
