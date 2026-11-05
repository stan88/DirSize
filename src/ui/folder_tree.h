#pragma once
// Self-drawn folder tree of the standalone window (design sections 3 and 5:
// the built-in TreeView has no documented dark mode, so the tree draws
// itself with the palette, like SizeList). Drives at the top level, lazily
// listed subfolders (TreeLoader), keyboard and mouse wheel, per-monitor DPI.
//
// The parent learns about user selections only: kTreeSelect is posted after
// a click at once, and after keyboard moves when the keys rest briefly (so
// holding an arrow key does not load every folder on the way). Reveal(),
// called when the list navigates, never posts it.

#include <windows.h>

#include <memory>
#include <string>

#include "ui/palette.h"
#include "ui/scroll_strip.h"
#include "ui/tree_loader.h"
#include "ui/tree_model.h"

namespace dirsize::ui {

constexpr UINT kTreeSelect = WM_APP + 210;  // read SelectedPath()

class FolderTree {
public:
    // Creates the child window; the object belongs to the window and is
    // deleted with it.
    static FolderTree* Create(HWND parent, HINSTANCE instance);

    HWND Window() const { return hwnd_; }
    void SetPalette(const Palette& palette);
    void SetDpi(UINT dpi);
    // Selects `path` without posting kTreeSelect.
    void Reveal(const std::wstring& path);
    // Lists the drives and `path` again (F5).
    void Refresh(const std::wstring& path);
    std::wstring SelectedPath() const { return TreeModel::PathOf(model_.Selected()); }

private:
    FolderTree() = default;
    ~FolderTree();
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void Apply(const TreeAction& action, bool fromKeyboard);
    void Load(const std::vector<std::wstring>& paths);
    void AfterChange();  // rows changed: scroll range, keep the selection in view
    void UpdateMetrics();
    void UpdateScroll();
    int VisibleRows() const;
    void EnsureVisible(int row);
    void ScrollTo(int top);
    int HitRow(int y);
    bool HitExpander(int row, int x);
    int Indent(int depth) const { return Scale(6) + depth * Scale(16); }
    void Paint(HDC dc, const RECT& client);
    void PaintRow(HDC dc, const TreeRow& row, const RECT& r, bool selected);
    int Scale(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    TreeModel model_;
    std::unique_ptr<TreeLoader> loader_;
    Palette palette_;
    UINT dpi_ = 96;
    HFONT font_ = nullptr;
    int rowHeight_ = 24;
    int top_ = 0;
    int wheelRemainder_ = 0;
    bool focused_ = false;
    ScrollStrip scroll_;
};

}  // namespace dirsize::ui
