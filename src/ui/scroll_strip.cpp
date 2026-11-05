#include "ui/scroll_strip.h"

#include <algorithm>

#include "ui/scroll_geometry.h"

namespace dirsize::ui {

namespace {

// Holding the button on the track: one page at once, more after a pause.
constexpr UINT kFirstRepeatMs = 350;
constexpr UINT kRepeatMs = 60;

}  // namespace

void ScrollStrip::SetRange(int count, int page, int pos) {
    count_ = count;
    page_ = page;
    pos_ = pos;
}

bool ScrollStrip::Visible() const {
    return ComputeThumb(rect_.top, rect_.bottom - rect_.top, count_, page_, pos_, MinThumb()).visible;
}

void ScrollStrip::Paint(HDC dc, const Palette& palette) const {
    SetDCBrushColor(dc, palette.background);
    FillRect(dc, &rect_, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    ThumbSpan t = ComputeThumb(rect_.top, rect_.bottom - rect_.top, count_, page_, pos_, MinThumb());
    if (!t.visible) return;
    const bool active = hover_ || dragging_;
    // Thin while idle, wider under the mouse, centered in the strip.
    const int width = active ? Scale(8) : Scale(4);
    const int left = (rect_.left + rect_.right - width) / 2;
    const int inset = Scale(2);
    RECT thumb{left, t.top + inset, left + width, t.top + t.length - inset};
    if (thumb.bottom <= thumb.top) thumb.bottom = thumb.top + 1;
    SetDCBrushColor(dc, active ? palette.scrollThumbActive : palette.scrollThumb);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    // NULL_PEN draws one pixel smaller, hence the +1.
    RoundRect(dc, thumb.left, thumb.top, thumb.right + 1, thumb.bottom + 1, width, width);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

int ScrollStrip::PageStep(POINT pt) const {
    ScrollHit hit = HitScroll(rect_.top, rect_.bottom - rect_.top, count_, page_, pos_, MinThumb(), pt.y);
    if (hit == ScrollHit::Above && repeatDirection_ < 0) return -page_;
    if (hit == ScrollHit::Below && repeatDirection_ > 0) return page_;
    return 0;
}

bool ScrollStrip::ButtonDown(HWND owner, POINT pt, int& pos) {
    pos = -1;
    if (!Visible() || !PtInRect(&rect_, pt)) return false;
    ScrollHit hit = HitScroll(rect_.top, rect_.bottom - rect_.top, count_, page_, pos_, MinThumb(), pt.y);
    SetCapture(owner);
    if (hit == ScrollHit::Thumb) {
        ThumbSpan t = ComputeThumb(rect_.top, rect_.bottom - rect_.top, count_, page_, pos_, MinThumb());
        dragging_ = true;
        grabOffset_ = pt.y - t.top;
        InvalidateRect(owner, &rect_, FALSE);
        return true;
    }
    if (hit == ScrollHit::Above || hit == ScrollHit::Below) {
        repeating_ = true;
        repeatDirection_ = hit == ScrollHit::Above ? -1 : 1;
        int step = PageStep(pt);
        if (step) pos = std::clamp(pos_ + step, 0, std::max(0, count_ - page_));
        SetTimer(owner, kRepeatTimer, kFirstRepeatMs, nullptr);
    }
    return true;
}

bool ScrollStrip::MouseMove(HWND owner, POINT pt, int& pos) {
    pos = -1;
    if (dragging_) {
        pos = PositionFromThumbTop(rect_.top, rect_.bottom - rect_.top, count_, page_, pt.y - grabOffset_, MinThumb());
        return true;
    }
    SetHover(owner, Visible() && PtInRect(&rect_, pt));
    return repeating_;
}

bool ScrollStrip::ButtonUp(HWND owner) {
    if (!dragging_ && !repeating_) return false;
    ReleaseCapture();  // WM_CAPTURECHANGED ends the drag or the repeat
    CaptureLost(owner);
    return true;
}

bool ScrollStrip::Timer(HWND owner, UINT_PTR id, int& pos) {
    pos = -1;
    if (id != kRepeatTimer) return false;
    if (!repeating_) {
        KillTimer(owner, kRepeatTimer);
        return true;
    }
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(owner, &pt);
    int step = PageStep(pt);
    if (step) pos = std::clamp(pos_ + step, 0, std::max(0, count_ - page_));
    SetTimer(owner, kRepeatTimer, kRepeatMs, nullptr);
    return true;
}

void ScrollStrip::CaptureLost(HWND owner) {
    if (repeating_) KillTimer(owner, kRepeatTimer);
    const bool was = dragging_;
    dragging_ = false;
    repeating_ = false;
    repeatDirection_ = 0;
    if (was) InvalidateRect(owner, &rect_, FALSE);
}

void ScrollStrip::MouseLeave(HWND owner) {
    hover_ = true;  // force the change below
    SetHover(owner, false);
}

void ScrollStrip::SetHover(HWND owner, bool hover) {
    if (hover == hover_) return;
    hover_ = hover;
    if (hover) {
        // WM_MOUSELEAVE when the cursor leaves the owner, to drop the hover.
        TRACKMOUSEEVENT tme{sizeof(tme)};
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = owner;
        TrackMouseEvent(&tme);
    }
    InvalidateRect(owner, &rect_, FALSE);
}

}  // namespace dirsize::ui
