#pragma once
// What the Explorer panel shows (design 7.5, confirmed by the user and
// measured in three VM rounds with tools/explorer_probe.cpp):
//   - after a navigation (navigation pane, double-click, Back, tab switch):
//     the current folder; nothing if it is not a file-system folder;
//   - when the selection then changes to exactly one file-system folder:
//     that folder;
//   - when it changes to nothing while the keyboard focus is in the
//     navigation pane (the tree on the left): the current folder again.
//     Measured (dev machine, Windows 11, --trace): clicking the current
//     folder in the tree does not navigate, but clears the view's
//     selection, which used to hide the panel until the next navigation;
//   - when it changes to anything else (a file, nothing, several): nothing.
// The selection a view starts with (e.g. the folder Explorer selects again
// after Back) is the baseline and does not count as a click. Focus moving
// into or out of the navigation pane counts as a change.
// Pure: the Explorer worker feeds it plain values.

#include <cstdint>
#include <string>

namespace dirsize::ui {

// Identifies a selection: count, for a single item its path, and whether the
// focus is in the navigation pane.
std::wstring SelectionKey(std::uint32_t count, const std::wstring& path, bool inNavigationPane = false);

class PanelRules {
public:
    // A new active view. `folder` is its parsing name ("C:\x" or "::{GUID}"),
    // `isFileSystemFolder` whether it is a folder on disk, `selectionKey` the
    // selection it starts with.
    void Navigated(const std::wstring& folder, bool isFileSystemFolder, const std::wstring& selectionKey);

    // The selection of the current view, read successfully. `path` is the
    // single selected item's file-system path (empty if none or not on disk).
    // `inNavigationPane`: the keyboard focus is in Explorer's tree.
    void Selection(std::uint32_t count, const std::wstring& path, bool isFileSystemFolder,
                   bool inNavigationPane = false);

    // No active view could be determined (Explorer minimized, closed, tab
    // undeterminable, not in front): show nothing until the next Navigated.
    void ViewLost();

    // The folder whose subfolders the panel shows; empty = hide the panel.
    const std::wstring& Shown() const { return shown_; }

    // True once after Shown() changed; cleared by this call.
    bool TakeChanged();

private:
    void Set(const std::wstring& folder);

    bool haveView_ = false;
    std::wstring current_;  // the view's folder if on disk, else empty
    std::wstring baseline_;
    std::wstring shown_;
    bool changed_ = false;
};

}  // namespace dirsize::ui
