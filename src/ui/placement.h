#pragma once
// Where the panel goes beside an Explorer window (design 4.2, order
// confirmed by the user): outside on the right, else outside on the left,
// else inside on the right edge; always within the monitor's work area.

#include <windows.h>

namespace dirsize::ui {

enum class PanelSide { OutsideRight, OutsideLeft, InsideRight };

struct PanelPlacement {
    PanelSide side = PanelSide::OutsideRight;
    int x = 0;       // left edge of the panel's visible frame
    int top = 0;     // visible frame, clipped to the work area
    int bottom = 0;
};

// frame: Explorer's visible frame (DWMWA_EXTENDED_FRAME_BOUNDS).
// work: the work area of Explorer's monitor. width: the panel's width.
// height: the panel's height set by the user, aligned with Explorer's top;
// 0 = as tall as Explorer. Never taller than Explorer or the work area.
PanelPlacement PlacePanel(const RECT& frame, const RECT& work, int width, int height = 0);

// The panel's edge away from Explorer, the one the user drags to change the
// width: the right edge when the panel is outside on the right, otherwise
// the left edge. (The bottom edge changes the height.)
bool WidthEdgeIsRight(PanelSide side);

}  // namespace dirsize::ui
