#include "ui/tip_window.h"

#include <algorithm>

namespace dirsize::ui {

namespace {

constexpr wchar_t kClassName[] = L"DirSizeTip";

int Scale(int dip, UINT dpi) { return MulDiv(dip, static_cast<int>(dpi), 96); }

}  // namespace

TipWindow::~TipWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void TipWindow::Show(HINSTANCE instance, POINT screen, const std::wstring& text, HFONT font, const Palette& palette,
                     UINT dpi) {
    if (!hwnd_) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = Proc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kClassName;
            registered = RegisterClassExW(&wc) != 0;
        }
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT, kClassName,
                                L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (!hwnd_) return;
    }
    text_ = text;
    font_ = font;
    palette_ = palette;
    dpi_ = dpi ? dpi : 96;

    // Size from the text, with padding and a one-pixel border.
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    RECT measure{0, 0, 0, 0};
    DrawTextW(dc, text_.c_str(), static_cast<int>(text_.size()), &measure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    const int padX = Scale(8, dpi_), padY = Scale(4, dpi_);
    int width = measure.right - measure.left + 2 * padX;
    const int height = measure.bottom - measure.top + 2 * padY;

    // Stay on the monitor; a name wider than the monitor is cut off there.
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST), &mi);
    width = std::min(width, static_cast<int>(mi.rcWork.right - mi.rcWork.left));
    int x = std::clamp(static_cast<int>(screen.x), static_cast<int>(mi.rcWork.left),
                       static_cast<int>(mi.rcWork.right) - width);
    int y = std::clamp(static_cast<int>(screen.y), static_cast<int>(mi.rcWork.top),
                       static_cast<int>(mi.rcWork.bottom) - height);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void TipWindow::Hide() {
    if (hwnd_ && IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
}

LRESULT CALLBACK TipWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<TipWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            break;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;  // the mouse goes to the window below
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (self) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                SetDCBrushColor(dc, self->palette_.line);
                FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                RECT inner{rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1};
                SetDCBrushColor(dc, self->palette_.headerBackground);
                FillRect(dc, &inner, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                HGDIOBJ old = SelectObject(dc, self->font_ ? self->font_ : GetStockObject(DEFAULT_GUI_FONT));
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, self->palette_.text);
                RECT text{inner.left + Scale(7, self->dpi_), inner.top, inner.right - Scale(7, self->dpi_),
                          inner.bottom};
                DrawTextW(dc, self->text_.c_str(), static_cast<int>(self->text_.size()), &text,
                          DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                SelectObject(dc, old);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            if (self) self->hwnd_ = nullptr;
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace dirsize::ui
