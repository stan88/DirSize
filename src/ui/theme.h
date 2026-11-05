#pragma once
// Follows the system theme (design section 5):
//   - light/dark from UISettings (the foreground color), with
//     ColorValuesChanged for live changes;
//   - high contrast from SPI_GETHIGHCONTRAST and GetSysColor; the window
//     re-reads it on WM_SYSCOLORCHANGE / WM_SETTINGCHANGE;
//   - the dark title bar with DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 11
//     build 22000 and later per the DWM documentation; on Windows 10 the
//     call fails and the title bar stays light).
// The thread that creates a ThemeWatcher must have initialized the Windows
// Runtime (winrt::init_apartment) and must run a message loop.

#include <windows.h>

#include <memory>

#include "ui/palette.h"

namespace dirsize::ui {

class ThemeWatcher {
public:
    ThemeWatcher();
    ~ThemeWatcher();
    ThemeWatcher(const ThemeWatcher&) = delete;
    ThemeWatcher& operator=(const ThemeWatcher&) = delete;

    // Posts `message` to `notify` when the color values change. The event
    // arrives on a thread the documentation does not name, so the handler
    // only posts; the window re-reads Current() on its own thread.
    void Watch(HWND notify, UINT message);

    // Reads the current system state and builds the palette.
    Palette Current() const;

    // Dark or light title bar for a top-level window; ignored where
    // unsupported.
    static void ApplyTitleBar(HWND window, bool dark);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dirsize::ui
