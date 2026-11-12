#pragma once
// Display settings of the front end, per user in HKCU\Software\DirSize
// (the language and the panel size are there too).

#include <windows.h>

#include <string>

namespace dirsize::ui {

// "ShowShare" (REG_DWORD): show the share column (bar and percentage) in
// the window and the panel. Missing = shown (the default).
bool ShowShareColumn();
DWORD SetShowShareColumn(bool show);  // ERROR_SUCCESS or the Win32 error

// Column widths the user dragged ("PanelColumns" / "WindowColumns", REG_SZ,
// list_model.h ParseColumnWidths). Empty when none.
std::wstring ColumnWidthsSetting(bool panel);
DWORD SetColumnWidthsSetting(bool panel, const std::wstring& text);

// The window and the panel are separate processes. After a setting changes,
// NotifySettingsChanged posts this registered message to all top-level
// windows (HWND_BROADCAST), and each DirSize window reads the settings again.
UINT SettingsChangedMessage();
void NotifySettingsChanged();

}  // namespace dirsize::ui
