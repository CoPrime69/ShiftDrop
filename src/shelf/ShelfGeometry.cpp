#include "shelf/ShelfGeometry.h"

#include <algorithm>
#include <cmath>

namespace sd::shelf
{
    Box Lerp(const Box& a, const Box& b, float t)
    {
        const auto mix = [t](float x, float y) { return x + (y - x) * t; };
        return Box{ mix(a.left, b.left), mix(a.top, b.top), mix(a.right, b.right), mix(a.bottom, b.bottom) };
    }

    Box Union(const Box& a, const Box& b)
    {
        return Box{ std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom) };
    }

    Box Intersect(const Box& a, const Box& b)
    {
        Box r{ std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom) };
        if (r.right < r.left)
            r.right = r.left;
        if (r.bottom < r.top)
            r.bottom = r.top;
        return r;
    }

    const char* EdgeName(Edge edge)
    {
        switch (edge)
        {
        case Edge::Left: return "left";
        case Edge::Top: return "top";
        case Edge::Right: return "right";
        case Edge::Bottom: return "bottom";
        default: return "none";
        }
    }

    Edge EdgeFromName(std::string_view name)
    {
        if (name == "left") return Edge::Left;
        if (name == "top") return Edge::Top;
        if (name == "right") return Edge::Right;
        if (name == "bottom") return Edge::Bottom;
        return Edge::None;
    }

    Edge SnapEdge(const Box& panel, const Box& work, float threshold)
    {
        // Side edges first, so a panel dropped in a corner pins to the side (where a shelf belongs).
        const struct { Edge edge; float distance; } candidates[] = {
            { Edge::Left, std::max(0.f, panel.left - work.left) },
            { Edge::Right, std::max(0.f, work.right - panel.right) },
            { Edge::Top, std::max(0.f, panel.top - work.top) },
            { Edge::Bottom, std::max(0.f, work.bottom - panel.bottom) },
        };
        Edge best = Edge::None;
        float bestDistance = threshold;
        for (const auto& c : candidates)
            if (c.distance <= bestDistance && (best == Edge::None || c.distance < bestDistance))
            {
                best = c.edge;
                bestDistance = c.distance;
            }
        return best;
    }

    Box ClampInto(const Box& panel, const Box& work)
    {
        // Right/bottom first, then left/top, so a panel larger than the area keeps its top-left inside.
        float dx = 0, dy = 0;
        if (panel.right > work.right)
            dx = work.right - panel.right;
        if (panel.left + dx < work.left)
            dx = work.left - panel.left;
        if (panel.bottom > work.bottom)
            dy = work.bottom - panel.bottom;
        if (panel.top + dy < work.top)
            dy = work.top - panel.top;
        return panel.Offset(dx, dy);
    }

    Box Dock(const Box& panel, const Box& work, Edge edge)
    {
        Box b = panel;
        switch (edge)
        {
        case Edge::Left: b = panel.Offset(work.left - panel.left, 0); break;
        case Edge::Right: b = panel.Offset(work.right - panel.right, 0); break;
        case Edge::Top: b = panel.Offset(0, work.top - panel.top); break;
        case Edge::Bottom: b = panel.Offset(0, work.bottom - panel.bottom); break;
        default: break;
        }
        return ClampInto(b, work);
    }

    Box Tab(const Box& panel, const Box& work, Edge edge, float thickness, float length)
    {
        const float cx = (panel.left + panel.right) / 2.f, cy = (panel.top + panel.bottom) / 2.f;
        Box t;
        switch (edge)
        {
        case Edge::Left: t = Box{ work.left, cy - length / 2.f, work.left + thickness, cy + length / 2.f }; break;
        case Edge::Right: t = Box{ work.right - thickness, cy - length / 2.f, work.right, cy + length / 2.f }; break;
        case Edge::Top: t = Box{ cx - length / 2.f, work.top, cx + length / 2.f, work.top + thickness }; break;
        case Edge::Bottom: t = Box{ cx - length / 2.f, work.bottom - thickness, cx + length / 2.f, work.bottom }; break;
        default: return panel;
        }
        return ClampInto(t, work);
    }

    void Outward(Edge edge, float& dx, float& dy)
    {
        dx = edge == Edge::Left ? -1.f : edge == Edge::Right ? 1.f : 0.f;
        dy = edge == Edge::Top ? -1.f : edge == Edge::Bottom ? 1.f : 0.f;
    }

    Metrics::Metrics(float s)
        : scale(s),
          width(std::round(272.f * s)),
          header(std::round(42.f * s)),
          row(std::round(52.f * s)),
          footer(std::round(50.f * s)),
          inset(std::round(8.f * s)),
          thumb(std::round(38.f * s)),
          button(std::round(26.f * s)),
          radius(std::round(14.f * s)),
          tabThickness(std::round(22.f * s)),
          tabLength(std::round(84.f * s)),
          snap(std::round(32.f * s))
    {
    }

    float PanelHeight(const Metrics& m, size_t count)
    {
        const size_t rows = std::clamp<size_t>(count, 1, m.maxRows);
        return m.header + static_cast<float>(rows) * m.row + m.footer;
    }

    float MaxScroll(const Metrics& m, size_t count)
    {
        return count > m.maxRows ? static_cast<float>(count - m.maxRows) * m.row : 0.f;
    }

    Layout LayOut(const Box& panel, const Metrics& m, size_t count, float scroll)
    {
        Layout l;
        l.panel = panel;
        l.header = Box{ panel.left, panel.top, panel.right, panel.top + m.header };
        const float hy = (l.header.top + l.header.bottom) / 2.f;
        const float b = m.button;
        l.clear = Box{ panel.right - m.inset - b, hy - b / 2.f, panel.right - m.inset, hy + b / 2.f };
        l.pin = l.clear.Offset(-(b + std::round(4.f * m.scale)), 0);
        l.title = Box{ panel.left + m.inset + std::round(8.f * m.scale), l.header.top, l.pin.left - m.inset, l.header.bottom };
        l.footer = Box{ panel.left + m.inset, panel.bottom - m.footer + std::round(4.f * m.scale), panel.right - m.inset, panel.bottom - m.inset };
        l.list = Box{ panel.left, l.header.bottom, panel.right, panel.bottom - m.footer };

        scroll = std::clamp(scroll, 0.f, MaxScroll(m, count));
        const float pad = std::round(6.f * m.scale);
        for (size_t i = 0; i < count; ++i)
        {
            const float top = l.list.top + static_cast<float>(i) * m.row - scroll;
            if (top + m.row <= l.list.top)
                continue;
            if (top >= l.list.bottom)
                break;
            RowBox r;
            r.index = i;
            r.row = Box{ panel.left + m.inset, top, panel.right - m.inset, top + m.row };
            const float cy = top + m.row / 2.f;
            r.thumb = Box{ r.row.left + pad, cy - m.thumb / 2.f, r.row.left + pad + m.thumb, cy + m.thumb / 2.f };
            r.remove = Box{ r.row.right - pad - b, cy - b / 2.f, r.row.right - pad, cy + b / 2.f };
            r.reveal = r.remove.Offset(-(b + std::round(2.f * m.scale)), 0);
            const float tx = r.thumb.right + std::round(10.f * m.scale);
            r.name = Box{ tx, top + std::round(7.f * m.scale), r.reveal.left - pad, cy + std::round(2.f * m.scale) };
            r.detail = Box{ tx, cy + std::round(1.f * m.scale), r.reveal.left - pad, r.row.bottom - std::round(7.f * m.scale) };
            l.rows.push_back(r);
        }
        return l;
    }

    Hit HitTest(const Layout& l, float x, float y)
    {
        if (!l.panel.Contains(x, y))
            return {};
        if (l.clear.Contains(x, y))
            return { Part::Clear };
        if (l.pin.Contains(x, y))
            return { Part::Pin };
        if (l.header.Contains(x, y))
            return { Part::Header };
        if (l.footer.Contains(x, y))
            return { Part::Footer };
        if (l.list.Contains(x, y))
            for (const auto& r : l.rows)
                if (r.row.Contains(x, y))
                {
                    if (r.remove.Contains(x, y))
                        return { Part::Remove, r.index };
                    if (r.reveal.Contains(x, y))
                        return { Part::Reveal, r.index };
                    return { Part::Item, r.index };
                }
        return {};
    }
}
