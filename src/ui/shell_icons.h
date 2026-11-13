#pragma once
// System icons for the list and the folder tree, the ones File Explorer
// shows for a type (user request 2026-11-13, option A: by type, without
// reading the items).
//   - SHGetFileInfoW with SHGFI_USEFILEATTRIBUTES ("should not attempt to
//     access the file") for folders and file extensions; a local drive's
//     own icon is asked for with its root path, a network drive gets the
//     generic one (it is never asked, as in tree_loader.h).
//   - The documentation asks for COM to be initialized and for the call to
//     run on a background thread, so the workers that load the rows call
//     LoadShellIcons before posting them; the window only draws.
//   - The icons come in the system DPI sizes (small and large; measured:
//     28 and 56 px at 168 DPI in a per-monitor aware process), so each is
//     drawn scaled to the monitor's size with DrawIconEx.
// The icons stay loaded for the life of the process (one per type).

#include <windows.h>

#include <string>
#include <vector>

namespace dirsize::ui {

// Worker thread, COM initialized: loads the icons of keys (icon_key.h) not
// loaded yet. A key that fails is remembered and not asked again.
void LoadShellIcons(const std::vector<std::wstring>& keys);

// Window thread: draws the key's icon `px` pixels square at (x, y). False
// when it is not loaded (the caller draws its own outline).
bool DrawShellIcon(HDC dc, const std::wstring& key, int x, int y, int px);

}  // namespace dirsize::ui
