#pragma once
// "Check for updates" (the "..." menu): the only network access in DirSize,
// and only when the user chooses it. One HTTPS GET to api.github.com for the
// latest release (README, Privacy). Linked into dirsize.exe only; the
// service and the command-line tool have no network code.

#include <windows.h>

#include <string>

#include "ui/update_check.h"

namespace dirsize::app {

struct UpdateOutcome {
    ui::UpdateState state = ui::UpdateState::NetworkError;
    unsigned long code = 0;  // Win32 error for NetworkError, HTTP status otherwise
    std::wstring latest;     // "0.2.1" for Newer and UpToDate
};

// Runs the check on a new thread and posts `message` to `notify` with
// lParam = UpdateOutcome*; the receiver deletes it. If the window is gone,
// the outcome is dropped. False if no thread could be started.
bool StartUpdateCheck(HWND notify, UINT message, const std::wstring& currentVersion);

// The page that is opened for a new version (fixed; never a URL from the
// answer).
inline constexpr wchar_t kReleasesPage[] = L"https://github.com/stan88/DirSize/releases/latest";

}  // namespace dirsize::app
