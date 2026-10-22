#include "EditorChrome.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace sd::chrome
{
    FrameRects LayoutFrame(float width, float height, bool panel)
    {
        const float w = std::max(0.f, width), h = std::max(0.f, height);
        FrameRects r;
        r.toolBar = D2D1::RectF(0, 0, w, std::min(h, kToolBarHeight));
        r.footer = D2D1::RectF(0, std::max(r.toolBar.bottom, h - kFooterHeight), w, h);
        const float panelLeft = panel ? std::max(0.f, w - kPanelWidth) : w;
        r.stage = D2D1::RectF(0, r.toolBar.bottom, panelLeft, r.footer.top);
        r.panel = D2D1::RectF(panelLeft, r.toolBar.bottom, w, r.footer.top);
        return r;
    }

    float TabWidth(float labelWidth) { return std::max(kTabMinWidth, std::ceil(labelWidth) + 2 * kTabPadX); }

    std::vector<D2D1_RECT_F> Flow(const std::vector<float>& widths, float left, float right, float top, float height, float gap, float rowGap)
    {
        std::vector<D2D1_RECT_F> out;
        out.reserve(widths.size());
        float x = left, y = top;
        for (const float w : widths)
        {
            if (x > left && x + w > right + 0.5f)
            {
                x = left;
                y += height + rowGap;
            }
            out.push_back(D2D1::RectF(x, y, std::min(x + w, std::max(right, x + 1)), y + height));
            x += w + gap;
        }
        return out;
    }

    std::vector<D2D1_RECT_F> Segments(const std::vector<float>& textWidths, float left, float top, float maxWidth)
    {
        if (textWidths.empty())
            return {};
        const float n = static_cast<float>(textWidths.size());
        float texts = 0;
        for (const float w : textWidths)
            texts += std::ceil(w);
        const float pad = std::min(kSegmentPadX, std::floor((maxWidth - texts) / (2 * n)));
        if (pad < kSegmentMinPadX)
            return {};
        std::vector<D2D1_RECT_F> out;
        float x = left;
        for (const float w : textWidths)
        {
            const float sw = std::ceil(w) + 2 * pad;
            out.push_back(D2D1::RectF(x, top, x + sw, top + kSegmentHeight));
            x += sw;
        }
        return out;
    }

    ToolBarRight LayoutToolBarRight(const D2D1_RECT_F& bar, bool undo, bool zoom)
    {
        ToolBarRight r;
        const float cy = (bar.top + bar.bottom) / 2;
        float x = bar.right - kBarPadX;
        if (zoom)
        {
            r.zoom = D2D1::RectF(x - kZoomFieldWidth, cy - kControlHeight / 2, x, cy + kControlHeight / 2);
            x = r.zoom.left - 4;
        }
        if (undo)
        {
            if (zoom)
            {
                // A line between the history buttons and the zoom: 1 x 22 with 6 either side.
                r.divider = D2D1::RectF(x - 7, cy - 11, x - 6, cy + 11);
                x -= 13 + 4;
            }
            r.redo = D2D1::RectF(x - kToolBarButton, cy - kToolBarButton / 2, x, cy + kToolBarButton / 2);
            x = r.redo.left - 4;
            r.undo = D2D1::RectF(x - kToolBarButton, cy - kToolBarButton / 2, x, cy + kToolBarButton / 2);
            x = r.undo.left;
        }
        r.left = x;
        return r;
    }

    std::vector<ZoomChoice> ZoomChoices()
    {
        return { { L"Fit", 0 }, { L"25%", 0.25 }, { L"50%", 0.5 }, { L"100%", 1 }, { L"200%", 2 }, { L"400%", 4 } };
    }

    std::wstring ZoomLabel(double zoom, bool fit)
    {
        if (fit)
            return L"Fit";
        return std::format(L"{:.0f}%", std::max(0.0, zoom) * 100);
    }

    SliderParts SliderOnChannel(const D2D1_RECT_F& channel, const D2D1_RECT_F& thumb, float trackHeight)
    {
        SliderParts parts;
        const float cy = std::round((channel.top + channel.bottom) / 2);
        parts.track = D2D1::RectF(channel.left, cy - trackHeight / 2, std::max(channel.left, channel.right), cy + trackHeight / 2);
        const float width = parts.track.right - parts.track.left;
        const float x = (thumb.left + thumb.right) / 2;
        parts.value = width > 0 ? std::clamp((x - parts.track.left) / width, 0.f, 1.f) : 0.f;
        return parts;
    }

    SliderEnds LayoutSliderEnds(float left, float right, float top, float height, float lowWidth, float highWidth)
    {
        right = std::max(left, right);
        lowWidth = std::max(0.f, lowWidth);
        highWidth = std::max(0.f, highWidth);
        // Too short for both: each gets its share of the bar, the line between them where they meet.
        if (lowWidth + highWidth > right - left)
        {
            const float total = lowWidth + highWidth;
            const float split = total > 0 ? left + (right - left) * lowWidth / total : left;
            return { D2D1::RectF(left, top, split, top + height), D2D1::RectF(split, top, right, top + height) };
        }
        return { D2D1::RectF(left, top, left + lowWidth, top + height), D2D1::RectF(right - highWidth, top, right, top + height) };
    }

    int WheelScroll(int delta, unsigned lines, int linePx, int pagePx, int& rest)
    {
        constexpr int kNotch = 120;   // WHEEL_DELTA
        if (lines == 0 || delta == 0)
            return 0;
        const long long step = lines == kWheelPage ? pagePx : static_cast<long long>(std::min(lines, 100u)) * linePx;
        // The wheel turned away from the user (delta > 0) scrolls towards the start; turning it back
        // drops what was left over from the other way.
        if ((rest > 0 && delta > 0) || (rest < 0 && delta < 0))
            rest = 0;
        const long long total = rest - static_cast<long long>(delta) * step;
        const long long px = total / kNotch;   // towards zero: what is left over stays in `rest`
        rest = static_cast<int>(total - px * kNotch);
        return static_cast<int>(px);
    }
}
