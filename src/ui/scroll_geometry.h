#pragma once
// Geometry of the self-drawn vertical scroll bar (scroll_strip.h), without
// window code so it is unit-tested. Positions are in rows: `count` rows,
// `page` of them visible, the first visible one is `pos` (0..count-page).

namespace dirsize::ui {

struct ThumbSpan {
    bool visible = false;  // false when everything fits
    int top = 0;           // pixels, inside the track
    int length = 0;
};

// The thumb in a track of `trackLength` pixels starting at `trackTop`. Its
// length is the visible share of the rows, at least `minThumb` pixels.
ThumbSpan ComputeThumb(int trackTop, int trackLength, int count, int page, int pos, int minThumb);

// The position for a thumb dragged to `thumbTop` (clamped to the track).
int PositionFromThumbTop(int trackTop, int trackLength, int count, int page, int thumbTop, int minThumb);

enum class ScrollHit { None, Above, Thumb, Below };

// Where `y` falls in the track (None outside it or when nothing scrolls).
ScrollHit HitScroll(int trackTop, int trackLength, int count, int page, int pos, int minThumb, int y);

}  // namespace dirsize::ui
