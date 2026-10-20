#include "canvas/Geometry.h"

namespace sd::canvas
{
    double ViewTransform::FitZoom(double margin) const
    {
        const double w = std::max(1.0, m_view.w - 2 * margin), h = std::max(1.0, m_view.h - 2 * margin);
        if (m_image.w <= 0 || m_image.h <= 0)
            return 1.0;
        return std::clamp(std::min({ w / m_image.w, h / m_image.h, 1.0 }), kMinZoom, kMaxZoom);
    }

    void ViewTransform::Fit(double margin)
    {
        m_fit = true;
        m_fitMargin = margin;
        m_zoom = FitZoom(margin);
        m_offset = { m_view.x + (m_view.w - m_image.w * m_zoom) / 2, m_view.y + (m_view.h - m_image.h * m_zoom) / 2 };
    }

    void ViewTransform::ZoomAt(double zoom, PointD anchor)
    {
        zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
        const PointD at = ToImage(anchor);
        m_zoom = zoom;
        m_offset = { anchor.x - at.x * zoom, anchor.y - at.y * zoom };
        m_fit = false;
        Clamp();
    }

    void ViewTransform::Step(int notches, PointD anchor)
    {
        // Snap to the fit zoom and to 100% when a step crosses them, so both are easy to get back to.
        const double fit = FitZoom(m_fitMargin);
        double next = m_zoom * std::pow(2.0, notches / 4.0);
        for (const double stop : { fit, 1.0 })
            if ((m_zoom < stop && next > stop) || (m_zoom > stop && next < stop))
                next = stop;
        if (std::abs(next - fit) < 1e-9)
        {
            Fit(m_fitMargin);
            return;
        }
        ZoomAt(next, anchor);
    }

    void ViewTransform::PanBy(double dx, double dy)
    {
        m_offset.x += dx;
        m_offset.y += dy;
        m_fit = false;
        Clamp();
    }

    void ViewTransform::Clamp()
    {
        if (m_fit)
        {
            Fit(m_fitMargin);
            return;
        }
        // Keep at least a quarter of the view (or the whole image, if smaller) covered by the image.
        const double iw = m_image.w * m_zoom, ih = m_image.h * m_zoom;
        const double keepX = std::min(iw, m_view.w / 4), keepY = std::min(ih, m_view.h / 4);
        m_offset.x = std::clamp(m_offset.x, m_view.x + keepX - iw, m_view.Right() - keepX);
        m_offset.y = std::clamp(m_offset.y, m_view.y + keepY - ih, m_view.Bottom() - keepY);
    }
}
