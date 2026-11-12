#pragma once
// The classic registry verb "Open with DirSize" (design section 6, decision
// 7.3): per user under HKEY_CURRENT_USER\Software\Classes, so no elevation
// and no code loaded into Explorer. Registered for
//   - Directory: file folders;
//   - Drive: drives ("C:\");
// (Predefined Shell Objects table). Not for Directory\Background (the empty
// space inside a folder): the token for its folder (%V, used by the probe)
// is not on the verb documentation pages, so it is left out.
// On Windows 11 the verb appears under "Show more options" (measured).
// Switched on and off from the standalone window's "..." menu. The
// installer registers the same verb for all users under HKEY_LOCAL_MACHINE
// (Phase 6, decision 7.2); the menu then shows it as set by the installer.

#include <windows.h>

namespace dirsize::ui {

bool IsShellVerbInstalled();
// Both return ERROR_SUCCESS or the Win32 error. They notify the shell
// (SHChangeNotify, SHCNE_ASSOCCHANGED) after a change.
DWORD InstallShellVerb();
DWORD RemoveShellVerb();

// True when the installer registered the verb for all users (HKLM).
bool IsShellVerbSetByInstaller();

// Start the Explorer panel when this user signs in: a value under
// HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run (Run and
// RunOnce Registry Keys), so each user decides and no elevation is needed.
bool IsPanelAutostart();
DWORD SetPanelAutostart(bool on);  // ERROR_SUCCESS or the Win32 error

}  // namespace dirsize::ui
