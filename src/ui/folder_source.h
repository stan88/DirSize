#pragma once
// Loads a folder's children with their sizes, on a worker thread so the
// window never waits (design section 2):
//   - folders: from the DirSize service (QueryFolder, all pages); if the
//     service cannot answer, a walk of the folder (WalkTree), in background
//     thread mode, cancelled as soon as another folder is requested;
//   - files (optional): listed directly with the user's own rights.
// Results are posted to a window; the receiver owns them.

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ui/list_model.h"

namespace dirsize::ui {

enum class LoadStatus { Ok, NotFound, AccessDenied, Failed };

// Why the service did not answer, or that its answer may be old.
enum class ServiceNote {
    None,
    Updating,      // answer from the service while it rescans
    Starting,      // service installed but not answering yet (delayed start)
    NotRunning,    // installed, stopped, not set to start automatically
    NotInstalled,
    NotIndexed,    // the volume has no index (not NTFS, removable, ...)
    Indexing,      // the service is building the index
    NotLocal,      // not a drive-letter path (network share, ...)
};

struct FolderResult {
    std::uint64_t request = 0;
    std::wstring path;
    LoadStatus status = LoadStatus::Ok;
    unsigned long error = 0;  // Win32 error for Failed
    bool fromService = false;
    ServiceNote note = ServiceNote::None;
    std::uint64_t totalLogical = 0;
    std::uint64_t totalAllocated = 0;
    std::uint64_t totalFiles = 0;
    std::vector<ListItem> items;
};

class FolderSource {
public:
    // Results are posted as `message` with lParam = FolderResult*; the
    // receiver deletes it.
    FolderSource(HWND notify, UINT message);
    // Cancels the current load and waits briefly for the worker; if it is
    // stuck (e.g. a pipe call waiting for its timeout) it is left to finish
    // on its own and its result is dropped.
    ~FolderSource();
    FolderSource(const FolderSource&) = delete;
    FolderSource& operator=(const FolderSource&) = delete;

    // Starts loading `folder`; a load in progress is cancelled. Returns the
    // request number that its result will carry.
    std::uint64_t Request(const std::wstring& folder, bool includeFiles);

    struct State;

private:
    std::shared_ptr<State> state_;
};

}  // namespace dirsize::ui
