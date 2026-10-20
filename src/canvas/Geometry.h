#pragma once
#include <algorithm>
#include <cmath>

// Plain geometry for the image canvas (src/canvas). Image coordinates are pixels of the picture as
// the editor shows it (EXIF orientation and any tool rotation applied), in doubles, so a region can
// sit between pixels while it is dragged and is rounded only when the output is rendered.
namespace sd::canvas
{
    struct PointD
    {
        double x = 0, y = 0;
    };

    struct SizeD
    {
        double w = 0, h = 0;
    };

    struct RectD
    {
        double x = 0, y = 0, w = 0, h = 0;

        double Right() const { return x + w; }
        double Bottom() const { return y + h; }
        PointD Centre() const { return { x + w / 2, y + h / 2 }; }
        bool Empty() const { return w <= 0 || h <= 0; }
        bool Contains(PointD p) const { return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h; }
        friend bool operator==(const RectD&, const RectD&) = default;
    };

    // A whole-pixel rectangle, for rendering.
    struct RectI
    {
        int x = 0, y = 0, w = 0, h = 0;
        friend bool operator==(const RectI&, const RectI&) = default;
    };

    // The rectangle spanned by two corners, in either order.
    inline RectD FromCorners(PointD a, PointD b)
    {
        return { std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y) };
    }

    inline RectD Intersect(const RectD& a, const RectD& b)
    {
        const double l = std::max(a.x, b.x), t = std::max(a.y, b.y);
        const double r = std::min(a.Right(), b.Right()), btm = std::min(a.Bottom(), b.Bottom());
        return r > l && btm > t ? RectD{ l, t, r - l, btm - t } : RectD{ l, t, 0, 0 };
    }

    // Moves `r` (keeping its size, shrunk only if it is larger than `bounds`) to lie inside `bounds`.
    inline RectD ClampInside(RectD r, const RectD& bounds)
    {
        r.w = std::min(r.w, bounds.w);
        r.h = std::min(r.h, bounds.h);
        r.x = std::clamp(r.x, bounds.x, bounds.Right() - r.w);
        r.y = std::clamp(r.y, bounds.y, bounds.Bottom() - r.h);
        return r;
    }

    // Rounds to whole pixels inside an image of `width` x `height`: edges snap to the nearest pixel
    // boundary, and a non-empty rectangle stays at least one pixel.
    inline RectI ToPixels(const RectD& r, int width, int height)
    {
        int l = static_cast<int>(std::lround(r.x)), t = static_cast<int>(std::lround(r.y));
        int rr = static_cast<int>(std::lround(r.Right())), b = static_cast<int>(std::lround(r.Bottom()));
        l = std::clamp(l, 0, width);
        t = std::clamp(t, 0, height);
        rr = std::clamp(rr, 0, width);
        b = std::clamp(b, 0, height);
        if (rr <= l && r.w > 0 && width > 0)
        {
            l = std::min(l, width - 1);
            rr = l + 1;
        }
        if (b <= t && r.h > 0 && height > 0)
        {
            t = std::min(t, height - 1);
            b = t + 1;
        }
        return { l, t, std::max(0, rr - l), std::max(0, b - t) };
    }

    // How the editor maps image pixels to the window (DIPs): view = image * zoom + offset.
    //
    // "Fit" shows the whole picture with a margin; zooming keeps the image point under the cursor
    // where it is; panning never lets the picture leave the view entirely.
    class ViewTransform
    {
    public:
        static constexpr double kMinZoom = 0.02;   // 2%
        static constexpr double kMaxZoom = 32.0;   // 3200%: single pixels are easy to place

        void SetViewport(const RectD& view) { m_view = view; Clamp(); }
        void SetImage(SizeD image) { m_image = image; }
        const RectD& Viewport() const { return m_view; }
        SizeD Image() const { return m_image; }

        double Zoom() const { return m_zoom; }
        bool IsFit() const { return m_fit; }
        // The zoom that shows the whole image inside the viewport less `margin` DIPs on each side;
        // never above 100% for small images (they show at their real size, centred).
        double FitZoom(double margin) const;

        void Fit(double margin);
        // Sets the zoom keeping the image point under `anchor` (view DIPs) fixed.
        void ZoomAt(double zoom, PointD anchor);
        // One wheel notch in (+1) or out (-1), about `anchor`. Steps are 2^(1/4), so four notches double.
        void Step(int notches, PointD anchor);
        void PanBy(double dx, double dy);

        PointD ToView(PointD image) const { return { image.x * m_zoom + m_offset.x, image.y * m_zoom + m_offset.y }; }
        PointD ToImage(PointD view) const { return { (view.x - m_offset.x) / m_zoom, (view.y - m_offset.y) / m_zoom }; }
        RectD ToView(const RectD& r) const { return { r.x * m_zoom + m_offset.x, r.y * m_zoom + m_offset.y, r.w * m_zoom, r.h * m_zoom }; }
        PointD Offset() const { return m_offset; }

    private:
        void Clamp();

        RectD m_view;
        SizeD m_image{ 1, 1 };
        double m_zoom = 1.0;
        PointD m_offset;
        bool m_fit = true;
        double m_fitMargin = 0;
    };
}
