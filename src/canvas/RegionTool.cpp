#include "canvas/RegionTool.h"
#include "Utf.h"

#include <windows.h>

namespace sd::canvas
{
    CursorShape CursorForGrip(Grip grip)
    {
        switch (grip)
        {
        case Grip::N:
        case Grip::S: return CursorShape::SizeNS;
        case Grip::E:
        case Grip::W: return CursorShape::SizeWE;
        case Grip::NW:
        case Grip::SE: return CursorShape::SizeNWSE;
        case Grip::NE:
        case Grip::SW: return CursorShape::SizeNESW;
        case Grip::Body: return CursorShape::Move;
        case Grip::None: break;
        }
        return CursorShape::Arrow;
    }

    CursorShape RegionTool::CursorAt(const Pointer& p) const
    {
        if (const Hit hit = m_regions.HitTest(p.at, p.tolerance))
            return p.ctrl && hit.grip == Grip::Body ? CursorShape::Hand : CursorForGrip(hit.grip);
        return m_canCreate && m_regions.Bounds().Contains(p.at) ? CursorShape::Cross : CursorShape::Arrow;
    }

    Response RegionTool::Hover(const Pointer& p)
    {
        const Hit hit = m_regions.HitTest(p.at, p.tolerance);
        if (hit.index == m_hot.index && hit.grip == m_hot.grip)
            return {};
        m_hot = hit;
        return kRedraw;
    }

    Response RegionTool::PointerDown(const Pointer& p)
    {
        m_downAt = p.at;
        m_downTolerance = p.tolerance;
        const Hit hit = m_regions.HitTest(p.at, p.tolerance);
        if (hit && hit.grip != Grip::Body)
        {
            m_regions.BeginDrag(hit, p.at);
            m_press = Press::Dragging;
            return kRedraw;
        }
        if (hit)
        {
            if (p.ctrl)
            {
                m_regions.Select(hit.index, true);   // Ctrl+click: add or remove, no drag
                m_press = Press::None;
                return kRedraw;
            }
            if (!m_regions.Items()[static_cast<size_t>(hit.index)].selected)
                m_regions.Select(hit.index, false);
            m_regions.BeginDrag(hit, p.at);
            m_press = Press::Dragging;
            return kRedraw;
        }
        // Empty picture: a click clears the selection; a drag draws a new region (from PointerMove,
        // once the pointer has really moved, so a click never leaves a tiny region behind).
        if (!p.ctrl && !m_single)
            m_regions.ClearSelection();
        m_press = m_canCreate && m_regions.Bounds().Contains(p.at) ? Press::Pending : Press::None;
        return kRedraw;
    }

    Response RegionTool::PointerMove(const Pointer& p)
    {
        if (m_press == Press::Pending)
        {
            const double dx = p.at.x - m_downAt.x, dy = p.at.y - m_downAt.y;
            if (dx * dx + dy * dy < m_downTolerance * m_downTolerance / 4)
                return {};
            if (m_single)
            {
                m_replaced = m_regions.Items();
                m_regions.Clear();
            }
            m_regions.BeginCreate(m_downAt, m_newStyle);
            m_press = Press::Dragging;
        }
        if (m_press != Press::Dragging)
            return {};
        m_regions.DragTo(p.at, p.shift, p.alt);
        RegionsChanged();
        return kRedraw;
    }

    Response RegionTool::PointerUp(const Pointer& p)
    {
        (void)p;
        const Press was = m_press;
        m_press = Press::None;
        if (was != Press::Dragging)
            return kRedraw;
        const bool creating = m_regions.Creating();
        const bool kept = m_regions.EndDrag();
        if (m_single && creating)
        {
            if (!kept)
            {
                m_regions.Items() = m_replaced;   // too small to be meant: keep the old box
                m_regions.SelectAll();
            }
            m_replaced.clear();
        }
        RegionsChanged();
        return kChanged;
    }

    Response RegionTool::PointerCancel()
    {
        const bool creating = m_regions.Creating();
        if (m_press == Press::Dragging)
            m_regions.CancelDrag();
        if (m_single && creating)
        {
            m_regions.Items() = m_replaced;
            m_regions.SelectAll();
            m_replaced.clear();
        }
        m_press = Press::None;
        RegionsChanged();
        return kRedraw;
    }

    Response RegionTool::Key(unsigned vk, bool shift, bool ctrl)
    {
        const double step = shift ? 10.0 : 1.0;
        switch (vk)
        {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        {
            const double dx = vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0;
            const double dy = vk == VK_UP ? -step : vk == VK_DOWN ? step : 0;
            if (!m_regions.Nudge(dx, dy))
                return {};
            RegionsChanged();
            return kChanged;
        }
        case VK_DELETE:
        case VK_BACK:
            if (!m_canDelete || m_regions.DeleteSelected() == 0)
                return {};
            RegionsChanged();
            return kChanged;
        case 'A':
            if (!ctrl || m_single)
                return {};
            m_regions.SelectAll();
            return kRedraw;
        case VK_TAB:
        {
            const int n = static_cast<int>(m_regions.Size());
            if (n == 0)
                return {};
            const int at = m_regions.FirstSelected();
            const int next = at < 0 ? (shift ? n - 1 : 0) : (at + (shift ? n - 1 : 1)) % n;
            m_regions.Select(next, false);
            return kRedraw;
        }
        default:
            return {};
        }
    }

    nlohmann::json RegionTool::SaveRegions(const RegionSet& regions)
    {
        auto out = nlohmann::json::array();
        for (const auto& r : regions.Items())
            out.push_back({ { "id", r.id }, { "x", r.rect.x }, { "y", r.rect.y }, { "w", r.rect.w }, { "h", r.rect.h }, { "selected", r.selected },
                            { "style", r.style }, { "origin", r.origin }, { "label", Narrow(r.label) } });
        return out;
    }

    void RegionTool::LoadRegions(RegionSet& regions, const nlohmann::json& saved)
    {
        regions.Clear();
        if (!saved.is_array())
            return;
        for (const auto& s : saved)
        {
            const int i = regions.Add({ s.value("x", 0.0), s.value("y", 0.0), s.value("w", 0.0), s.value("h", 0.0) }, s.value("style", 0),
                                      s.value("origin", std::string{}), Widen(s.value("label", std::string{})));
            auto& r = regions.Items()[static_cast<size_t>(i)];
            r.selected = s.value("selected", false);
        }
    }
}
