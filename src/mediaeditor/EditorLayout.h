#pragma once
#include "mediaeditor/Timeline.h"

#include <vector>

// Geometry of the editor window that does not need a window: fitting the preview, mapping between
// the preview and the frame's pixels (crop and redaction boxes are drawn on the preview but stored in
// pixels), and hit-testing handles and ranges. All in DIPs.
namespace sd::mediatools
{
    struct RectF
    {
        float x = 0, y = 0, w = 0, h = 0;
        float Right() const { return x + w; }
        float Bottom() const { return y + h; }
        bool Contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
        RectF Inset(float d) const { return { x + d, y + d, w - 2 * d, h - 2 * d }; }
        bool operator==(const RectF&) const = default;
    };

    // The largest `contentW` x `contentH` rectangle that fits in `box`, centred (letterboxed).
    RectF FitInside(float contentW, float contentH, const RectF& box);

    // The preview shows a frame of frameW x frameH pixels in `shown`.
    struct PreviewMap
    {
        RectF shown;
        int frameW = 0, frameH = 0;
        int ToFrameX(float x) const;   // rounded, not clamped
        int ToFrameY(float y) const;
        float ToViewX(double px) const;
        float ToViewY(double py) const;
        RectF ToView(const PixelRect& r) const;
        double PixelsPerDip() const;   // frame pixels per DIP of the preview
    };

    // Which crop handle is at (x, y): 0..3 corners (top-left, top-right, bottom-right, bottom-left),
    // 4..7 edges (top, right, bottom, left), 8 inside, -1 none. `grip` is the handle's reach.
    int HitBoxHandle(const RectF& box, float x, float y, float grip);

    // A timeline target under the cursor: an edge of one of `ranges`, the body of one, or nothing.
    struct RangeHit
    {
        int index = -1;
        enum class Part { None, Start, End, Body } part = Part::None;
    };
    RangeHit HitRange(const TimelineView& view, const std::vector<TimeRange>& ranges, float x, float grip);
    // The marker within `grip` of x (the nearest), or -1.
    int HitMarker(const TimelineView& view, const std::vector<int64_t>& markers, float x, float grip);

    // How many thumbnails of `thumbW` fill a filmstrip `width` wide (at least 1).
    int FilmstripSlots(float width, float thumbW);
}
