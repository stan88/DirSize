#pragma once
// Self-drawn scroll bar for SizeList and FolderTree, vertical (rows) or
// horizontal (pixels, the list's columns). The system
// scroll bar (WS_VSCROLL) stays light in dark mode; the only way to change
// that is an undocumented theme name, so, as for the list and the tree
// themselves (design section 5), the bar is drawn with the palette.
//
// Not a window: the owner gives it a strip of its client area, paints it,
// and forwards mouse and timer messages. Thumb drag, clicks on the track
// (a page, repeating while held), and a thicker thumb while hovered.

#include <windows.h>

#include "ui/palette.h"

namespace dirsize::ui {

class ScrollStrip {
public:
    static constexpr UINT_PTR kRepeatTimer = 0x5C01;  // owner forwards WM_TIMER with this ID

    void SetDpi(UINT dpi) { dpi_ = dpi ? dpi : 96; }
    // A horizontal bar; with its own timer ID (kRepeatTimer + 1), so both
    // bars of one owner can repeat independently.
    void SetHorizontal(bool horizontal) { horizontal_ = horizontal; }
    int Width() const { return Scale(12); }  // the thickness
    // The strip, in the owner's client coordinates.
    void SetRect(const RECT& r) { rect_ = r; }
    const RECT& Rect() const { return rect_; }
    void SetRange(int count, int page, int pos);
    bool Visible() const;  // false when everything fits
    void Paint(HDC dc, const Palette& palette) const;

    // Mouse and timer messages, in client coordinates. Each returns true
    // when the bar used the message; `pos` is then the new first row, or -1
    // for no change.
    bool ButtonDown(HWND owner, POINT pt, int& pos);
    bool MouseMove(HWND owner, POINT pt, int& pos);
    bool ButtonUp(HWND owner);
    bool Timer(HWND owner, UINT_PTR id, int& pos);
    void CaptureLost(HWND owner);  // WM_CAPTURECHANGED
    void MouseLeave(HWND owner);   // WM_MOUSELEAVE

private:
    int Scale(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }
    int MinThumb() const { return Scale(24); }
    int PageStep(POINT pt) const;  // -page, +page, or 0 when the thumb reached the cursor
    void SetHover(HWND owner, bool hover);
    // The track along the bar and a point's coordinate along it.
    int TrackStart() const { return horizontal_ ? rect_.left : rect_.top; }
    int TrackLength() const { return horizontal_ ? rect_.right - rect_.left : rect_.bottom - rect_.top; }
    int Along(POINT pt) const { return horizontal_ ? pt.x : pt.y; }
    UINT_PTR TimerId() const { return horizontal_ ? kRepeatTimer + 1 : kRepeatTimer; }

    bool horizontal_ = false;
    UINT dpi_ = 96;
    RECT rect_{};
    int count_ = 0;
    int page_ = 0;
    int pos_ = 0;
    bool hover_ = false;
    bool dragging_ = false;
    int grabOffset_ = 0;  // cursor minus thumb start (along the bar) when the drag started
    bool repeating_ = false;
    int repeatDirection_ = 0;
};

}  // namespace dirsize::ui
