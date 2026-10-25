#include "shelf/ShelfGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

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

    Place PlaceOf(const Box& panel, const Box& work, Edge edge, std::wstring device)
    {
        Place p;
        p.edge = edge;
        p.work = work;
        p.device = std::move(device);
        const float room = work.Width() - panel.Width();
        p.x = room > 0.f ? std::clamp((panel.left - work.left) / room, 0.f, 1.f) : 0.f;
        p.y = work.Height() > 0.f ? std::clamp((panel.top - work.top) / work.Height(), 0.f, 1.f) : 0.f;
        return p;
    }

    Box PlaceIn(const Place& place, const Box& work, float width, float height)
    {
        const float x = std::clamp(place.x, 0.f, 1.f), y = std::clamp(place.y, 0.f, 1.f);
        const float left = work.left + std::round(std::max(0.f, work.Width() - width) * x);
        const float top = work.top + std::round(work.Height() * y);
        return Dock(Box{ left, top, left + width, top + height }, work, place.edge);
    }

    size_t PickScreen(const std::vector<Screen>& screens, const Place& place)
    {
        const auto first = [&](auto&& match) -> std::optional<size_t> {
            for (size_t i = 0; i < screens.size(); ++i)
                if (match(screens[i]))
                    return i;
            return std::nullopt;
        };
        const bool named = !place.device.empty();
        if (const auto i = first([&](const Screen& s) { return s.work == place.work && named && s.device == place.device; }))
            return *i;
        if (const auto i = first([&](const Screen& s) { return s.work == place.work; }))
            return *i;
        if (const auto i = first([&](const Screen& s) { return named && s.device == place.device; }))
            return *i;
        // That monitor is gone: the work area nearest to the spot (0 when the spot is inside one).
        const float px = place.work.left + place.work.Width() * std::clamp(place.x, 0.f, 1.f);
        const float py = place.work.top + place.work.Height() * std::clamp(place.y, 0.f, 1.f);
        size_t best = 0;
        float bestDistance = std::numeric_limits<float>::max();
        for (size_t i = 0; i < screens.size(); ++i)
        {
            const Box& w = screens[i].work;
            const float dx = std::max({ w.left - px, 0.f, px - w.right });
            const float dy = std::max({ w.top - py, 0.f, py - w.bottom });
            if (const float d = dx * dx + dy * dy; d < bestDistance)
            {
                best = i;
                bestDistance = d;
            }
        }
        return best;
    }

    Metrics::Metrics(float s)
        : scale(s),
          // As the brand kit's App UI page draws the shelf (8 px corners, 28 px buttons, 30 px tab).
          width(std::round(268.f * s)),
          header(std::round(46.f * s)),
          row(std::round(53.f * s)),
          footer(std::round(50.f * s)),
          inset(std::round(6.f * s)),
          thumb(std::round(37.f * s)),
          button(std::round(28.f * s)),
          clearAll(std::round(70.f * s)),
          radius(std::round(8.f * s)),
          label(std::round(28.f * s)),
          tabThickness(std::round(30.f * s)),
          tabLength(std::round(84.f * s)),
          snap(std::round(32.f * s))
    {
    }

    float PanelHeight(const Metrics& m, size_t count)
    {
        return PanelHeight(m, ShelfShape(count));
    }

    float MaxScroll(const Metrics& m, size_t count)
    {
        return MaxScroll(m, ShelfShape(count));
    }

    ListShape ShelfShape(size_t count)
    {
        ListShape shape;
        shape.rows.assign(count, RowKind::Item);
        shape.clear = shape.footer = count > 0;
        shape.drop = count == 0;
        return shape;
    }

    float RowHeight(const Metrics& m, const ListShape& shape, size_t index)
    {
        return index < shape.rows.size() && shape.rows[index] == RowKind::Label ? m.label : m.row;
    }

    float RowTop(const Metrics& m, const ListShape& shape, size_t index)
    {
        float top = 0;
        for (size_t i = 0; i < index && i < shape.rows.size(); ++i)
            top += RowHeight(m, shape, i);
        return top;
    }

    float ContentHeight(const Metrics& m, const ListShape& shape)
    {
        return RowTop(m, shape, shape.rows.size());
    }

    float PanelHeight(const Metrics& m, const ListShape& shape)
    {
        // At least one row's room (the empty shelf's drop box), at most five rows' before it scrolls.
        const float view = static_cast<float>(m.maxRows) * m.row;
        const float list = std::clamp(ContentHeight(m, shape), m.row, view);
        return m.header + list + (shape.footer || shape.drop ? m.footer : m.inset);
    }

    float MaxScroll(const Metrics& m, const ListShape& shape)
    {
        return std::max(0.f, ContentHeight(m, shape) - static_cast<float>(m.maxRows) * m.row);
    }

    const Box& ButtonBox(const RowBox& row, Part part)
    {
        switch (part)
        {
        case Part::PinItem: return row.pin;
        case Part::Open: return row.open;
        case Part::Reveal: return row.reveal;
        default: return row.remove;
        }
    }

    Layout LayOut(const Box& panel, const Metrics& m, size_t count, float scroll)
    {
        return LayOut(panel, m, ShelfShape(count), scroll);
    }

    Layout LayOut(const Box& panel, const Metrics& m, const ListShape& shape, float scroll)
    {
        Layout l;
        l.panel = panel;
        l.header = Box{ panel.left, panel.top, panel.right, panel.top + m.header };
        const float hy = (l.header.top + l.header.bottom) / 2.f;
        const float b = m.button;
        const float gap = std::round(4.f * m.scale);
        l.close = Box{ panel.right - m.inset - b, hy - b / 2.f, panel.right - m.inset, hy + b / 2.f };
        float left = l.close.left;
        if (shape.clear)
        {
            l.clear = Box{ left - gap - m.clearAll, l.close.top, left - gap, l.close.bottom };
            left = l.clear.left;
        }
        l.pin = Box{ left - gap - b, l.close.top, left - gap, l.close.bottom };
        l.title = Box{ panel.left + m.inset + std::round(8.f * m.scale), l.header.top, l.pin.left - m.inset, l.header.bottom };
        if (shape.dropDown > 0)
            l.dropDown = Box{ l.title.left - std::round(6.f * m.scale), l.close.top, std::min(l.title.right, l.title.left + shape.dropDown), l.close.bottom };
        const float bottom = shape.footer || shape.drop ? m.footer : m.inset;
        l.list = Box{ panel.left, l.header.bottom, panel.right, panel.bottom - bottom };
        if (shape.footer)
            l.footer = Box{ panel.left + m.inset, panel.bottom - m.footer + std::round(4.f * m.scale), panel.right - m.inset, panel.bottom - m.inset };
        else if (shape.drop)
            l.drop = Box{ panel.left + m.inset, l.header.bottom + std::round(4.f * m.scale), panel.right - m.inset, panel.bottom - m.inset };

        scroll = std::clamp(scroll, 0.f, MaxScroll(m, shape));
        const float pad = std::round(6.f * m.scale);
        float top = l.list.top - scroll;
        for (size_t i = 0; i < shape.rows.size(); ++i)
        {
            const float h = RowHeight(m, shape, i);
            const float rowTop = top;
            top += h;
            if (rowTop + h <= l.list.top)
                continue;
            if (rowTop >= l.list.bottom)
                break;
            RowBox r;
            r.index = i;
            r.kind = shape.rows[i];
            r.row = Box{ panel.left + m.inset, rowTop, panel.right - m.inset, rowTop + h };
            if (r.kind == RowKind::Label)
            {
                // A heading: small text at the bottom of its row, over the items it heads.
                r.name = Box{ r.row.left + pad + std::round(4.f * m.scale), rowTop + std::round(8.f * m.scale), r.row.right - pad, r.row.bottom - std::round(2.f * m.scale) };
                l.rows.push_back(r);
                continue;
            }
            const float cy = rowTop + h / 2.f;
            r.thumb = Box{ r.row.left + pad, cy - m.thumb / 2.f, r.row.left + pad + m.thumb, cy + m.thumb / 2.f };
            const float step = b + std::round(2.f * m.scale);
            r.remove = Box{ r.row.right - pad - b, cy - b / 2.f, r.row.right - pad, cy + b / 2.f };
            r.reveal = r.remove.Offset(-step, 0);
            r.open = r.reveal.Offset(-step, 0);
            r.pin = r.open.Offset(-step, 0);
            const float tx = r.thumb.right + std::round(10.f * m.scale);
            r.name = Box{ tx, rowTop + std::round(7.f * m.scale), r.pin.left - pad, cy + std::round(2.f * m.scale) };
            r.detail = Box{ tx, cy + std::round(1.f * m.scale), r.pin.left - pad, r.row.bottom - std::round(7.f * m.scale) };
            l.rows.push_back(r);
        }
        return l;
    }

    Hit HitTest(const Layout& l, float x, float y)
    {
        if (!l.panel.Contains(x, y))
            return {};
        if (l.close.Contains(x, y))
            return { Part::Close };
        if (l.clear.Contains(x, y))
            return { Part::Clear };
        if (l.pin.Contains(x, y))
            return { Part::Pin };
        if (l.dropDown.Contains(x, y))
            return { Part::DropDown };
        if (l.header.Contains(x, y))
            return { Part::Header };
        if (l.footer.Contains(x, y))
            return { Part::Footer };
        if (l.list.Contains(x, y))
            for (const auto& r : l.rows)
                if (r.row.Contains(x, y))
                {
                    if (r.kind == RowKind::Label)
                        return { Part::Label, r.index };
                    for (const Part part : kRowButtons)
                        if (ButtonBox(r, part).Contains(x, y))
                            return { part, r.index };
                    return { Part::Item, r.index };
                }
        return {};
    }
}
