#include "canvas/Regions.h"

namespace sd::canvas
{
    namespace
    {
        bool IsLeft(Grip g) { return g == Grip::W || g == Grip::NW || g == Grip::SW; }
        bool IsRight(Grip g) { return g == Grip::E || g == Grip::NE || g == Grip::SE; }
        bool IsTop(Grip g) { return g == Grip::N || g == Grip::NE || g == Grip::NW; }
        bool IsBottom(Grip g) { return g == Grip::S || g == Grip::SE || g == Grip::SW; }

        RectD Union(const RectD& a, const RectD& b)
        {
            const double l = std::min(a.x, b.x), t = std::min(a.y, b.y);
            return { l, t, std::max(a.Right(), b.Right()) - l, std::max(a.Bottom(), b.Bottom()) - t };
        }
    }

    void RegionSet::SetBounds(const RectD& bounds)
    {
        m_bounds = bounds;
        for (auto& r : m_items)
            r.rect = Normalised(r.rect);
    }

    Region* RegionSet::Find(uint32_t id)
    {
        for (auto& r : m_items)
            if (r.id == id)
                return &r;
        return nullptr;
    }

    RectD RegionSet::Normalised(RectD r) const
    {
        r.w = std::max(r.w, std::min(m_min, m_bounds.w));
        r.h = std::max(r.h, std::min(m_min, m_bounds.h));
        return ClampInside(r, m_bounds);
    }

    int RegionSet::Add(const RectD& rect, int style, std::string origin, std::wstring label)
    {
        Region r;
        r.id = m_nextId++;
        r.rect = Normalised(rect);
        r.style = style;
        r.origin = std::move(origin);
        r.label = std::move(label);
        m_items.push_back(std::move(r));
        return static_cast<int>(m_items.size()) - 1;
    }

    Hit RegionSet::HitTest(PointD p, double tol) const
    {
        // Grips of selected regions first, topmost (last drawn) first.
        for (int i = static_cast<int>(m_items.size()) - 1; i >= 0; --i)
        {
            const auto& reg = m_items[static_cast<size_t>(i)];
            if (!reg.selected)
                continue;
            const RectD& r = reg.rect;
            // On a small region the grips would cover all of it; shrink their reach so the body
            // can still be grabbed in the middle.
            const double t = std::min(tol, std::min(r.w, r.h) / 3);
            const bool nearL = std::abs(p.x - r.x) <= t, nearR = std::abs(p.x - r.Right()) <= t;
            const bool nearT = std::abs(p.y - r.y) <= t, nearB = std::abs(p.y - r.Bottom()) <= t;
            const bool inX = p.x >= r.x - t && p.x <= r.Right() + t, inY = p.y >= r.y - t && p.y <= r.Bottom() + t;
            if (nearT && nearL) return { i, Grip::NW };
            if (nearT && nearR) return { i, Grip::NE };
            if (nearB && nearR) return { i, Grip::SE };
            if (nearB && nearL) return { i, Grip::SW };
            if (nearT && inX) return { i, Grip::N };
            if (nearB && inX) return { i, Grip::S };
            if (nearL && inY) return { i, Grip::W };
            if (nearR && inY) return { i, Grip::E };
        }
        for (int i = static_cast<int>(m_items.size()) - 1; i >= 0; --i)
            if (m_items[static_cast<size_t>(i)].rect.Contains(p))
                return { i, Grip::Body };
        return {};
    }

    void RegionSet::Select(int index, bool toggle)
    {
        if (index < 0 || index >= static_cast<int>(m_items.size()))
            return;
        if (toggle)
        {
            m_items[static_cast<size_t>(index)].selected = !m_items[static_cast<size_t>(index)].selected;
            return;
        }
        for (size_t i = 0; i < m_items.size(); ++i)
            m_items[i].selected = static_cast<int>(i) == index;
    }

    void RegionSet::SelectAll()
    {
        for (auto& r : m_items)
            r.selected = true;
    }

    void RegionSet::ClearSelection()
    {
        for (auto& r : m_items)
            r.selected = false;
    }

    int RegionSet::SelectedCount() const
    {
        int n = 0;
        for (const auto& r : m_items)
            n += r.selected ? 1 : 0;
        return n;
    }

    int RegionSet::FirstSelected() const
    {
        for (size_t i = 0; i < m_items.size(); ++i)
            if (m_items[i].selected)
                return static_cast<int>(i);
        return -1;
    }

    int RegionSet::DeleteSelected()
    {
        const auto before = m_items.size();
        std::erase_if(m_items, [](const Region& r) { return r.selected; });
        m_drag = {};
        return static_cast<int>(before - m_items.size());
    }

    bool RegionSet::Nudge(double dx, double dy)
    {
        bool any = false;
        RectD all;
        for (const auto& r : m_items)
            if (r.selected)
            {
                all = any ? Union(all, r.rect) : r.rect;
                any = true;
            }
        if (!any)
            return false;
        dx = std::clamp(dx, m_bounds.x - all.x, m_bounds.Right() - all.Right());
        dy = std::clamp(dy, m_bounds.y - all.y, m_bounds.Bottom() - all.Bottom());
        if (dx == 0 && dy == 0)
            return false;
        for (auto& r : m_items)
            if (r.selected)
            {
                r.rect.x += dx;
                r.rect.y += dy;
            }
        return true;
    }

    void RegionSet::BeginDrag(const Hit& hit, PointD at)
    {
        m_drag = {};
        if (!hit || hit.index >= static_cast<int>(m_items.size()))
            return;
        m_drag.active = true;
        m_drag.index = hit.index;
        m_drag.grip = hit.grip;
        m_drag.start = at;
        if (hit.grip == Grip::Body)
        {
            for (size_t i = 0; i < m_items.size(); ++i)
                if (m_items[i].selected || static_cast<int>(i) == hit.index)
                    m_drag.originals.emplace_back(static_cast<int>(i), m_items[i].rect);
        }
        else
        {
            m_drag.originals.emplace_back(hit.index, m_items[static_cast<size_t>(hit.index)].rect);
        }
    }

    int RegionSet::BeginCreate(PointD at, int style)
    {
        at.x = std::clamp(at.x, m_bounds.x, m_bounds.Right());
        at.y = std::clamp(at.y, m_bounds.y, m_bounds.Bottom());
        Region r;
        r.id = m_nextId++;
        r.rect = { at.x, at.y, 0, 0 };
        r.style = style;
        m_items.push_back(std::move(r));
        const int index = static_cast<int>(m_items.size()) - 1;
        Select(index, false);
        m_drag = {};
        m_drag.active = true;
        m_drag.creating = true;
        m_drag.index = index;
        m_drag.grip = Grip::SE;
        m_drag.start = at;
        m_drag.last = at;
        m_drag.originals.emplace_back(index, m_items.back().rect);
        return index;
    }

    RectD RegionSet::Resized(const RectD& o, Grip grip, PointD at, double aspect, bool fromCentre) const
    {
        const RectD& b = m_bounds;
        const PointD p{ std::clamp(at.x, b.x, b.Right()), std::clamp(at.y, b.y, b.Bottom()) };
        const bool horiz = IsLeft(grip) || IsRight(grip), vert = IsTop(grip) || IsBottom(grip);
        const PointD c = o.Centre();
        // The fixed point: the opposite edge or corner, or the centre.
        const double ax = fromCentre ? c.x : IsLeft(grip) ? o.Right() : o.x;
        const double ay = fromCentre ? c.y : IsTop(grip) ? o.Bottom() : o.y;
        const double dx = p.x - ax, dy = p.y - ay;   // the pointer may cross the anchor: the rectangle flips
        const double k = fromCentre ? 2.0 : 1.0;
        double w = horiz ? std::abs(dx) * k : o.w;
        double h = vert ? std::abs(dy) * k : o.h;
        if (aspect > 0)
        {
            if (horiz && vert)
            {
                // A corner follows whichever side the pointer pulls further.
                if (w / aspect >= h)
                    h = w / aspect;
                else
                    w = h * aspect;
            }
            else if (horiz)
                h = w / aspect;
            else
                w = h * aspect;
        }
        // Room in the direction of growth. An edge drag with a fixed aspect grows the other side
        // about its centre line.
        const auto room = [&](bool along, double anchor, double sign, double lo, double hi, double centre) {
            if (!along)
                return 2 * std::min(centre - lo, hi - centre);
            if (fromCentre)
                return 2 * std::min(anchor - lo, hi - anchor);
            return sign >= 0 ? hi - anchor : anchor - lo;
        };
        const double maxW = room(horiz, ax, dx, b.x, b.Right(), c.x);
        const double maxH = room(vert, ay, dy, b.y, b.Bottom(), c.y);
        if (aspect > 0)
        {
            const double s = std::min({ 1.0, w > 0 ? maxW / w : 1.0, h > 0 ? maxH / h : 1.0 });
            w *= s;
            h *= s;
        }
        else
        {
            w = std::min(w, horiz ? maxW : b.w);
            h = std::min(h, vert ? maxH : b.h);
        }
        // Minimum size (kept in shape when an aspect is fixed).
        const double minW = std::min(m_min, b.w), minH = std::min(m_min, b.h);
        if (w < minW || h < minH)
        {
            if (aspect > 0)
            {
                w = std::max({ w, minW, minH * aspect });
                h = w / aspect;
            }
            else
            {
                w = std::max(w, minW);
                h = std::max(h, minH);
            }
        }
        double x = o.x, y = o.y;
        if (horiz)
            x = fromCentre ? ax - w / 2 : dx >= 0 ? ax : ax - w;
        else if (aspect > 0)
            x = c.x - w / 2;
        if (vert)
            y = fromCentre ? ay - h / 2 : dy >= 0 ? ay : ay - h;
        else if (aspect > 0)
            y = c.y - h / 2;
        return ClampInside({ x, y, w, h }, b);
    }

    void RegionSet::DragTo(PointD at, bool lockAspect, bool fromCentre)
    {
        if (!m_drag.active || m_drag.originals.empty())
            return;
        m_drag.last = at;
        if (m_drag.grip == Grip::Body)
        {
            RectD all = m_drag.originals.front().second;
            for (const auto& [i, r] : m_drag.originals)
                all = Union(all, r);
            const double dx = std::clamp(at.x - m_drag.start.x, m_bounds.x - all.x, m_bounds.Right() - all.Right());
            const double dy = std::clamp(at.y - m_drag.start.y, m_bounds.y - all.y, m_bounds.Bottom() - all.Bottom());
            for (const auto& [i, r] : m_drag.originals)
                if (i < static_cast<int>(m_items.size()))
                    m_items[static_cast<size_t>(i)].rect = { r.x + dx, r.y + dy, r.w, r.h };
            return;
        }
        const auto& [index, original] = m_drag.originals.front();
        if (index >= static_cast<int>(m_items.size()))
            return;
        double aspect = m_aspect;
        if (aspect <= 0 && lockAspect && original.w > 0 && original.h > 0)
            aspect = original.w / original.h;
        if (m_drag.creating && aspect <= 0 && lockAspect)
            aspect = 1.0;   // Shift while drawing: a square
        m_items[static_cast<size_t>(index)].rect = Resized(original, m_drag.grip, at, aspect, fromCentre);
    }

    bool RegionSet::EndDrag()
    {
        const bool creating = m_drag.creating;
        const int index = m_drag.index;
        // A new region the pointer drew less than the minimum size across (a click, a slip) is dropped.
        const double drawn = std::max(std::abs(m_drag.last.x - m_drag.start.x), std::abs(m_drag.last.y - m_drag.start.y));
        m_drag = {};
        if (creating && index >= 0 && index < static_cast<int>(m_items.size()))
        {
            const RectD& r = m_items[static_cast<size_t>(index)].rect;
            if (drawn < m_min || r.w < m_min || r.h < m_min)
            {
                m_items.erase(m_items.begin() + index);
                return false;
            }
        }
        return true;
    }

    void RegionSet::CancelDrag()
    {
        if (!m_drag.active)
            return;
        if (m_drag.creating)
        {
            if (m_drag.index >= 0 && m_drag.index < static_cast<int>(m_items.size()))
                m_items.erase(m_items.begin() + m_drag.index);
        }
        else
        {
            for (const auto& [i, r] : m_drag.originals)
                if (i < static_cast<int>(m_items.size()))
                    m_items[static_cast<size_t>(i)].rect = r;
        }
        m_drag = {};
    }
}
