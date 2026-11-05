#pragma once

#include <windows.h>

// The Explorer panel (dirsize.exe --panel): a narrow window beside the
// Explorer window in front, listing the subfolders of the folder the user
// navigated to or selected (docs/DESIGN-phase5.md sections 4 and 7.5).
// Runs its own message loop; returns the exit code.
int RunPanel(HINSTANCE instance);
