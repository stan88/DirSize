#pragma once
// Follows the Explorer window in front and decides which folder the panel
// shows (panel_rules.h). Everything that talks to Explorer runs on its own
// single-threaded-apartment thread, so a busy or hung Explorer never stops
// the panel's window (VM test: single calls took up to 834 ms). Ported from
// tools/explorer_probe.cpp, where the approach was measured:
//   - every 250 ms while an Explorer window is in front (nothing otherwise);
//   - the active view is cached; IShellWindows is walked again only when the
//     cached view window is gone or hidden, the front tab changed, or every
//     2 s as a safety net;
//   - the selection is read with IFolderView2::GetSelection;
//   - Windows 11 tabs: the front tab is the topmost of the tabs' browser
//     windows in z-order (measured, not documented); when it cannot be
//     determined the panel hides;
//   - if Explorer restarts, the COM objects are dropped and created again.

#include <windows.h>

#include <memory>
#include <string>

namespace dirsize::ui {

struct ExplorerUpdate {
    HWND explorer = nullptr;  // the Explorer window followed
    std::wstring folder;      // folder to show; empty = hide the panel
};

class ExplorerTracker {
public:
    // Posts `message` to `panel` with lParam = ExplorerUpdate* (the receiver
    // deletes it) whenever the followed window or the folder changes. While
    // `panel` itself is in front, nothing changes.
    // `tracePath` non-empty: append a line to that file whenever what the
    // tracker sees changes (view, folder, selection, rule decisions), for
    // diagnosing the panel rules on a real Explorer (dirsize --panel --trace).
    ExplorerTracker(HWND panel, UINT message, const std::wstring& tracePath = L"");
    // Stops the thread; waits at most 2 s (a call into a hung Explorer must
    // not hang DirSize), then leaves the thread to end on its own.
    ~ExplorerTracker();
    ExplorerTracker(const ExplorerTracker&) = delete;
    ExplorerTracker& operator=(const ExplorerTracker&) = delete;

    // Makes the followed Explorer window (its front tab) browse to `folder`
    // (IShellBrowser::BrowseObject, SBSP_SAMEBROWSER | SBSP_ABSOLUTE). Runs
    // on the tracker's thread; does nothing if there is no active view.
    void Navigate(const std::wstring& folder);

    struct State;

private:
    std::shared_ptr<State> state_;
};

}  // namespace dirsize::ui
