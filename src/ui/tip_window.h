#pragma once
// A small self-drawn tooltip: the full name of a row whose name is cut off.
// Self-drawn with the palette like the rest of the front end, because the
// system tooltip stays light in dark mode. It never takes the focus and lets
// clicks through (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT), so it works over
// the Explorer panel, which must not become active.

#include <windows.h>

#include <string>

#include "ui/palette.h"

namespace dirsize::ui {

class TipWindow {
public:
    TipWindow() = default;
    TipWindow(const TipWindow&) = delete;
    TipWindow& operator=(const TipWindow&) = delete;
    ~TipWindow();

    // Shows `text` with its top-left corner at `screen` (moved to stay on
    // the monitor), in `font` (not owned).
    void Show(HINSTANCE instance, POINT screen, const std::wstring& text, HFONT font, const Palette& palette,
              UINT dpi);
    void Hide();
    bool Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    std::wstring text_;
    HFONT font_ = nullptr;
    Palette palette_;
    UINT dpi_ = 96;
};

}  // namespace dirsize::ui
