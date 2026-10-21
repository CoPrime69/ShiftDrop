#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

// Scrolling and drag-to-insert for a list or grid of equal cells, shared by Merge's file list (one
// column) and the page view's grid (OrganizeWindow.cpp). Plain maths in DIPs, relative to the top
// left of the viewport, so it is unit-tested.
namespace sd::pdf
{
    struct ScrollList
    {
        float offset = 0;        // how far the content is scrolled
        float cellWidth = 1, rowHeight = 1;
        size_t columns = 1;

        size_t Rows(size_t count) const { return (count + std::max<size_t>(columns, 1) - 1) / std::max<size_t>(columns, 1); }
        float ContentHeight(size_t count) const { return static_cast<float>(Rows(count)) * rowHeight; }

        void Clamp(size_t count, float viewport) { offset = std::clamp(offset, 0.f, std::max(0.f, ContentHeight(count) - viewport)); }

        // Scrolls just enough to show item `index` whole.
        void Reveal(size_t index, size_t count, float viewport)
        {
            const float top = static_cast<float>(index / std::max<size_t>(columns, 1)) * rowHeight;
            if (top < offset)
                offset = top;
            else if (top + rowHeight > offset + viewport)
                offset = top + rowHeight - viewport;
            Clamp(count, viewport);
        }

        // The item under (x, y), or -1 between or past them.
        long long ItemAt(float x, float y, size_t count) const
        {
            const float fx = x / cellWidth, fy = (y + offset) / rowHeight;
            if (fx < 0 || fy < 0 || fx >= static_cast<float>(columns))
                return -1;
            const size_t i = static_cast<size_t>(fy) * columns + static_cast<size_t>(fx);
            return i < count ? static_cast<long long>(i) : -1;
        }

        // Where dragged items would land: before this index (0..count). A list goes by the nearest
        // gap between rows; a grid by the row under the pointer and the nearest gap in it.
        size_t InsertAt(float x, float y, size_t count) const
        {
            if (columns <= 1)
                return static_cast<size_t>(std::clamp((y + offset) / rowHeight + 0.5f, 0.f, static_cast<float>(count)));
            const size_t rows = std::max<size_t>(Rows(count), 1);
            const auto row = static_cast<size_t>(std::clamp((y + offset) / rowHeight, 0.f, static_cast<float>(rows - 1)));
            const auto col = static_cast<size_t>(std::clamp(x / cellWidth + 0.5f, 0.f, static_cast<float>(columns)));
            return std::min(count, row * columns + col);
        }

        // While dragging: within `edge` of the viewport's top or bottom, scrolls a step that way.
        bool EdgeScroll(float y, float viewport, size_t count, float edge = 24.f, float step = 12.f)
        {
            const float before = offset;
            if (y < edge)
                offset -= step;
            else if (y > viewport - edge)
                offset += step;
            Clamp(count, viewport);
            return offset != before;
        }

        // A mouse-wheel turn of `delta` (WHEEL_DELTA = 120 a notch) scrolls `rowsPerNotch` rows.
        void Wheel(int delta, float rowsPerNotch, size_t count, float viewport)
        {
            offset -= static_cast<float>(delta) / 120.f * rowHeight * rowsPerNotch;
            Clamp(count, viewport);
        }

        // The items on screen: [first, last).
        std::pair<size_t, size_t> Visible(size_t count, float viewport) const
        {
            const auto firstRow = static_cast<size_t>(std::max(0.f, offset) / rowHeight);
            const auto lastRow = static_cast<size_t>((offset + viewport) / rowHeight) + 1;
            return { std::min(count, firstRow * columns), std::min(count, lastRow * columns) };
        }
    };

    // A press that becomes a drag once the pointer moves past the system's drag distance.
    struct DragGesture
    {
        bool pressed = false, dragging = false;
        float fromX = 0, fromY = 0;

        void Press(float x, float y)
        {
            pressed = true;
            dragging = false;
            fromX = x;
            fromY = y;
        }
        // True once, when the press turns into a drag.
        bool Move(float x, float y, float threshold)
        {
            if (!pressed || dragging || (std::abs(x - fromX) <= threshold && std::abs(y - fromY) <= threshold))
                return false;
            dragging = true;
            return true;
        }
        void Reset() { pressed = dragging = false; }
    };
}
