#include "mediaeditor/EditorLayout.h"

#include <algorithm>
#include <cmath>

namespace sd::mediatools
{
    RectF FitInside(float contentW, float contentH, const RectF& box)
    {
        if (contentW <= 0 || contentH <= 0 || box.w <= 0 || box.h <= 0)
            return { box.x, box.y, 0, 0 };
        const float scale = std::min(box.w / contentW, box.h / contentH);
        const float w = contentW * scale, h = contentH * scale;
        return { box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h };
    }

    double PreviewMap::PixelsPerDip() const { return shown.w > 0 ? frameW / static_cast<double>(shown.w) : 1.0; }

    int PreviewMap::ToFrameX(float x) const
    {
        return shown.w > 0 ? static_cast<int>(std::lround((x - shown.x) * frameW / static_cast<double>(shown.w))) : 0;
    }

    int PreviewMap::ToFrameY(float y) const
    {
        return shown.h > 0 ? static_cast<int>(std::lround((y - shown.y) * frameH / static_cast<double>(shown.h))) : 0;
    }

    float PreviewMap::ToViewX(double px) const
    {
        return frameW > 0 ? shown.x + static_cast<float>(px * shown.w / frameW) : shown.x;
    }

    float PreviewMap::ToViewY(double py) const
    {
        return frameH > 0 ? shown.y + static_cast<float>(py * shown.h / frameH) : shown.y;
    }

    RectF PreviewMap::ToView(const PixelRect& r) const
    {
        const float x0 = ToViewX(r.x), y0 = ToViewY(r.y);
        return { x0, y0, ToViewX(r.x + r.w) - x0, ToViewY(r.y + r.h) - y0 };
    }

    int HitBoxHandle(const RectF& b, float x, float y, float grip)
    {
        const bool nearL = std::abs(x - b.x) <= grip, nearR = std::abs(x - b.Right()) <= grip;
        const bool nearT = std::abs(y - b.y) <= grip, nearB = std::abs(y - b.Bottom()) <= grip;
        const bool inX = x >= b.x - grip && x <= b.Right() + grip, inY = y >= b.y - grip && y <= b.Bottom() + grip;
        if (nearT && nearL) return 0;
        if (nearT && nearR) return 1;
        if (nearB && nearR) return 2;
        if (nearB && nearL) return 3;
        if (nearT && inX) return 4;
        if (nearR && inY) return 5;
        if (nearB && inX) return 6;
        if (nearL && inY) return 7;
        if (b.Contains(x, y)) return 8;
        return -1;
    }

    RangeHit HitRange(const TimelineView& view, const std::vector<TimeRange>& ranges, float x, float grip)
    {
        RangeHit best;
        float bestDistance = grip + 1;
        for (size_t i = 0; i < ranges.size(); ++i)
        {
            const float x0 = static_cast<float>(view.XOf(ranges[i].start)), x1 = static_cast<float>(view.XOf(ranges[i].end));
            // Edges win over bodies; of two edges the nearer one (a short range's ends are close).
            for (const auto& [edgeX, part] : { std::pair{ x0, RangeHit::Part::Start }, std::pair{ x1, RangeHit::Part::End } })
            {
                const float d = std::abs(x - edgeX);
                if (d <= grip && d < bestDistance)
                {
                    bestDistance = d;
                    best = { static_cast<int>(i), part };
                }
            }
        }
        if (best.index >= 0)
            return best;
        for (size_t i = 0; i < ranges.size(); ++i)
        {
            const float x0 = static_cast<float>(view.XOf(ranges[i].start)), x1 = static_cast<float>(view.XOf(ranges[i].end));
            if (x > x0 && x < x1)
                return { static_cast<int>(i), RangeHit::Part::Body };
        }
        return {};
    }

    int HitMarker(const TimelineView& view, const std::vector<int64_t>& markers, float x, float grip)
    {
        int best = -1;
        float bestDistance = grip;
        for (size_t i = 0; i < markers.size(); ++i)
        {
            const float d = std::abs(x - static_cast<float>(view.XOf(markers[i])));
            if (d <= bestDistance)
            {
                bestDistance = d;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    int FilmstripSlots(float width, float thumbW)
    {
        if (thumbW <= 1)
            return 1;
        return std::max(1, static_cast<int>(std::ceil(width / thumbW)));
    }
}
