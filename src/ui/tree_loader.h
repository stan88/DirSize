#pragma once
// Lists the folder tree's children and the drives on a worker thread, so a
// slow or disconnected drive never stops the window.
//   - Subfolders follow the service's index rule (core/index.h): directory
//     reparse points that are name surrogates (junctions, symbolic links,
//     mount points) are left out; hidden and system folders are kept. The
//     tag comes from WIN32_FIND_DATAW::dwReserved0, which FindFirstFileExW
//     fills for reparse points.
//   - Drives: GetLogicalDriveStringsW first (posted at once), then labels
//     from GetVolumeInformationW (posted again); network drives keep the
//     letter only, so a disconnected share is never asked.
//   - The thread uses SetThreadErrorMode(SEM_FAILCRITICALERRORS) so an empty
//     card reader or optical drive returns an error instead of a dialog.
// Same lifetime pattern as FolderSource: shared state, results posted to a
// window, a stuck worker is left to finish on its own.

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "ui/tree_model.h"

namespace dirsize::ui {

struct TreeListing {
    std::wstring path;
    bool ok = false;
    unsigned long error = 0;
    std::vector<std::wstring> names;
};

struct DriveList {
    std::vector<TreeRoot> roots;
};

class TreeLoader {
public:
    // Posts `listingMessage` (lParam = TreeListing*) and `drivesMessage`
    // (lParam = DriveList*); the receiver deletes them.
    TreeLoader(HWND notify, UINT listingMessage, UINT drivesMessage);
    ~TreeLoader();
    TreeLoader(const TreeLoader&) = delete;
    TreeLoader& operator=(const TreeLoader&) = delete;

    // Queues a listing; a path already queued is not queued twice.
    void List(const std::wstring& path);
    void ListDrives();

    struct State;

private:
    std::shared_ptr<State> state_;
};

}  // namespace dirsize::ui
