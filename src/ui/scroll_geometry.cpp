#include "ui/scroll_geometry.h"

#include <algorithm>

namespace dirsize::ui {

namespace {

int ThumbLength(int trackLength, int count, int page, int minThumb) {
    long long length = static_cast<long long>(trackLength) * page / count;
    return static_cast<int>(std::clamp<long long>(length, std::min(minThumb, trackLength), trackLength));
}

}  // namespace

ThumbSpan ComputeThumb(int trackTop, int trackLength, int count, int page, int pos, int minThumb) {
    ThumbSpan t;
    if (count <= 0 || page <= 0 || count <= page || trackLength <= 0) return t;
    const int maxPos = count - page;
    t.visible = true;
    t.length = ThumbLength(trackLength, count, page, minThumb);
    const int travel = trackLength - t.length;
    pos = std::clamp(pos, 0, maxPos);
    t.top = trackTop + static_cast<int>(static_cast<long long>(travel) * pos / maxPos);
    return t;
}

int PositionFromThumbTop(int trackTop, int trackLength, int count, int page, int thumbTop, int minThumb) {
    if (count <= 0 || page <= 0 || count <= page || trackLength <= 0) return 0;
    const int maxPos = count - page;
    const int travel = trackLength - ThumbLength(trackLength, count, page, minThumb);
    if (travel <= 0) return 0;
    const int offset = std::clamp(thumbTop - trackTop, 0, travel);
    // Rounded to the nearest row.
    return static_cast<int>((static_cast<long long>(offset) * maxPos + travel / 2) / travel);
}

ScrollHit HitScroll(int trackTop, int trackLength, int count, int page, int pos, int minThumb, int y) {
    ThumbSpan t = ComputeThumb(trackTop, trackLength, count, page, pos, minThumb);
    if (!t.visible || y < trackTop || y >= trackTop + trackLength) return ScrollHit::None;
    if (y < t.top) return ScrollHit::Above;
    if (y >= t.top + t.length) return ScrollHit::Below;
    return ScrollHit::Thumb;
}

}  // namespace dirsize::ui
