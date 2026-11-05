#include "ui/theme.h"

#include <unknwn.h>
#include <dwmapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>

namespace dirsize::ui {

namespace wuv = winrt::Windows::UI::ViewManagement;

struct ThemeWatcher::Impl {
    wuv::UISettings settings{nullptr};
    winrt::event_token token{};
    bool watching = false;
};

ThemeWatcher::ThemeWatcher() : impl_(std::make_unique<Impl>()) {
    try {
        impl_->settings = wuv::UISettings();
    } catch (...) {
        // Without UISettings the palette falls back to light (see Current).
    }
}

ThemeWatcher::~ThemeWatcher() {
    if (impl_->watching) {
        try {
            impl_->settings.ColorValuesChanged(impl_->token);
        } catch (...) {
        }
    }
}

void ThemeWatcher::Watch(HWND notify, UINT message) {
    if (!impl_->settings || impl_->watching) return;
    try {
        impl_->token = impl_->settings.ColorValuesChanged(
            [notify, message](const wuv::UISettings&, const winrt::Windows::Foundation::IInspectable&) {
                PostMessageW(notify, message, 0, 0);
            });
        impl_->watching = true;
    } catch (...) {
    }
}

Palette ThemeWatcher::Current() const {
    bool dark = false;
    COLORREF accent = RGB(0x00, 0x78, 0xD4);
    if (impl_->settings) {
        try {
            auto fg = impl_->settings.GetColorValue(wuv::UIColorType::Foreground);
            dark = IsColorLight(fg.R, fg.G, fg.B);  // light foreground = dark mode (Microsoft themes page)
            auto a = impl_->settings.GetColorValue(dark ? wuv::UIColorType::AccentLight2 : wuv::UIColorType::Accent);
            accent = RGB(a.R, a.G, a.B);
        } catch (...) {
        }
    }
    HIGHCONTRASTW hc{sizeof(hc)};
    bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
                        (hc.dwFlags & HCF_HIGHCONTRASTON);
    SystemColors sys{GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_HIGHLIGHT),
                     GetSysColor(COLOR_HIGHLIGHTTEXT), GetSysColor(COLOR_GRAYTEXT)};
    return MakePalette(dark, highContrast, accent, sys);
}

void ThemeWatcher::ApplyTitleBar(HWND window, bool dark) {
    BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));
}

}  // namespace dirsize::ui
