#include "ui/placement.h"

#include <algorithm>

namespace dirsize::ui {

PanelPlacement PlacePanel(const RECT& frame, const RECT& work, int width, int height) {
    PanelPlacement p;
    if (frame.right + width <= work.right) {
        p.side = PanelSide::OutsideRight;
        p.x = frame.right;
    } else if (frame.left - width >= work.left) {
        p.side = PanelSide::OutsideLeft;
        p.x = frame.left - width;
    } else {
        p.side = PanelSide::InsideRight;
        p.x = std::min(frame.right, work.right) - width;
        p.x = std::max(p.x, static_cast<int>(work.left));
    }
    p.top = std::max(frame.top, work.top);
    p.bottom = std::min(frame.bottom, work.bottom);
    if (height > 0 && p.top + height < p.bottom) p.bottom = p.top + height;
    if (p.bottom < p.top) p.bottom = p.top;
    return p;
}

bool WidthEdgeIsRight(PanelSide side) { return side == PanelSide::OutsideRight; }

}  // namespace dirsize::ui
