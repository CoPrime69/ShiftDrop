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

    float ChipWidth(float textWidth, bool chosen)
    {
        // A chosen chip's left padding is shorter: the check mark sits in it (the kit's 10 + 12 + 6).
        return std::ceil(textWidth) + 2 * kChipPadX + (chosen ? kChipCheck - 4.f : 0.f);
    }

    float SegmentWidth(float textWidth) { return std::ceil(textWidth) + 2 * kSegmentPadX; }

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
        float total = 2 * kSegmentInset + kSegmentGap * static_cast<float>(textWidths.size() - 1);
        for (const float w : textWidths)
            total += SegmentWidth(w);
        if (total > maxWidth + 0.5f)
            return {};
        std::vector<D2D1_RECT_F> out;
        out.push_back(D2D1::RectF(left, top, left + total, top + kSegmentHeight));
        float x = left + kSegmentInset;
        for (const float w : textWidths)
        {
            const float sw = SegmentWidth(w);
            out.push_back(D2D1::RectF(x, top + kSegmentInset, x + sw, top + kSegmentHeight - kSegmentInset));
            x += sw + kSegmentGap;
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
}
