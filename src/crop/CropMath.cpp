#include "crop/CropMath.h"

#include <cmath>
#include <numbers>

namespace sd::crop
{
    const std::vector<AspectPreset>& AspectPresets()
    {
        static const std::vector<AspectPreset> presets = {
            { "free", L"Free", 0, 0 },     { "original", L"Original", 0, 0 }, { "1:1", L"1:1", 1, 1 },     { "4:5", L"4:5", 4, 5 },
            { "3:2", L"3:2", 3, 2 },       { "16:9", L"16:9", 16, 9 },        { "9:16", L"9:16", 9, 16 },  { "custom", L"Custom", 0, 0 },
        };
        return presets;
    }

    const AspectPreset* FindAspect(std::string_view id)
    {
        for (const auto& p : AspectPresets())
            if (id == p.id)
                return &p;
        return nullptr;
    }

    double AspectRatio(std::string_view preset, double customW, double customH, double imageW, double imageH)
    {
        if (preset == "original")
            return imageW > 0 && imageH > 0 ? imageW / imageH : 0;
        if (preset == "custom")
            return customW > 0 && customH > 0 ? customW / customH : 0;
        if (const auto* p = FindAspect(preset); p && p->w > 0 && p->h > 0)
            return p->w / p->h;
        return 0;
    }

    RectD LargestCentred(const RectD& bounds, double aspect)
    {
        if (aspect <= 0 || bounds.Empty())
            return bounds;
        double w = bounds.w, h = bounds.h;
        if (w / h > aspect)
            w = h * aspect;
        else
            h = w / aspect;
        return { bounds.x + (bounds.w - w) / 2, bounds.y + (bounds.h - h) / 2, w, h };
    }

    SizeD RotatedSize(double w, double h, int quarterTurns)
    {
        return (quarterTurns & 1) ? SizeD{ h, w } : SizeD{ w, h };
    }

    RectD StraightenBounds(double w, double h, double degrees)
    {
        if (degrees == 0 || w <= 0 || h <= 0)
            return { 0, 0, w, h };
        const double t = std::abs(degrees) * std::numbers::pi / 180.0;
        const double c = std::cos(t), s = std::sin(t);
        // A centred k*w x k*h box fits inside the picture rotated by t when its own bounding box,
        // turned back by t, fits inside w x h.
        const double k = std::min(w / (w * c + h * s), h / (w * s + h * c));
        return { (w - k * w) / 2, (h - k * h) / 2, k * w, k * h };
    }

    CropParams ParseParams(const nlohmann::json& p)
    {
        CropParams c;
        if (!p.is_object())
            return c;
        c.quarterTurns = ((p.value("quarterTurns", 0) % 4) + 4) % 4;
        c.straighten = std::clamp(p.value("straighten", 0.0), -45.0, 45.0);
        c.box = { p.value("x", 0.0), p.value("y", 0.0), p.value("w", 0.0), p.value("h", 0.0) };
        c.refW = p.value("refW", 0.0);
        c.refH = p.value("refH", 0.0);
        c.aspect = p.value("aspect", std::string("free"));
        c.ratioW = p.value("ratioW", 1.0);
        c.ratioH = p.value("ratioH", 1.0);
        c.exactSize = p.value("exactSize", false);
        c.samePlace = p.value("samePlace", false);
        return c;
    }

    nlohmann::json ToJson(const CropParams& c)
    {
        return { { "quarterTurns", c.quarterTurns }, { "straighten", c.straighten }, { "x", c.box.x },       { "y", c.box.y },
                 { "w", c.box.w },                   { "h", c.box.h },                   { "refW", c.refW },     { "refH", c.refH },
                 { "aspect", c.aspect },             { "ratioW", c.ratioW },             { "ratioH", c.ratioH }, { "exactSize", c.exactSize },
                 { "samePlace", c.samePlace } };
    }

    CropPlan PlanCrop(const CropParams& params, int width, int height)
    {
        CropPlan plan;
        plan.quarterTurns = params.quarterTurns;
        plan.degrees = params.straighten;
        const SizeD size = RotatedSize(width, height, params.quarterTurns);
        plan.rotatedW = static_cast<int>(size.w);
        plan.rotatedH = static_cast<int>(size.h);
        const RectD bounds = StraightenBounds(size.w, size.h, params.straighten);

        RectD box = params.box;
        const bool sameSize = (params.refW <= 0 || params.refH <= 0) || (std::abs(size.w - params.refW) < 0.5 && std::abs(size.h - params.refH) < 0.5);
        if (box.Empty())
        {
            box = bounds;
        }
        else if (!sameSize)
        {
            // Another picture of the batch: the same shape, scaled with the picture or at the typed size.
            const double s = params.exactSize ? 1.0 : std::min(size.w / params.refW, size.h / params.refH);
            double w = box.w * s, h = box.h * s;
            const double fit = std::min({ 1.0, bounds.w / w, bounds.h / h });
            w *= fit;
            h *= fit;
            const canvas::PointD c = params.samePlace ? canvas::PointD{ box.Centre().x / params.refW * size.w, box.Centre().y / params.refH * size.h }
                                                      : bounds.Centre();
            box = { c.x - w / 2, c.y - h / 2, w, h };
        }
        box = canvas::ClampInside(box, bounds);
        plan.box = canvas::ToPixels(box, plan.rotatedW, plan.rotatedH);
        return plan;
    }

    namespace
    {
        // The source pixel under rotated pixel (u, v), for exact quarter turns.
        inline void SourceOf(int q, int u, int v, int W, int H, int& sx, int& sy)
        {
            switch (q)
            {
            case 1: sx = v, sy = H - 1 - u; break;
            case 2: sx = W - 1 - u, sy = H - 1 - v; break;
            case 3: sx = W - 1 - v, sy = u; break;
            default: sx = u, sy = v; break;
            }
        }

        // Bilinear sample at continuous coordinates (pixel centres at +0.5), edges extended. Weights in
        // premultiplied alpha, so transparent pixels don't bleed their colour.
        void Sample(const canvas::Raster& src, double x, double y, uint8_t* out)
        {
            x = std::clamp(x - 0.5, 0.0, static_cast<double>(src.width - 1));
            y = std::clamp(y - 0.5, 0.0, static_cast<double>(src.height - 1));
            const UINT x0 = static_cast<UINT>(x), y0 = static_cast<UINT>(y);
            const UINT x1 = std::min(x0 + 1, src.width - 1), y1 = std::min(y0 + 1, src.height - 1);
            const double fx = x - x0, fy = y - y0;
            const uint8_t* p[4] = { src.At(x0, y0), src.At(x1, y0), src.At(x0, y1), src.At(x1, y1) };
            const double w[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
            double a = 0, c[3]{};
            for (int i = 0; i < 4; ++i)
            {
                const double pa = p[i][3] * w[i];
                a += pa;
                for (int k = 0; k < 3; ++k)
                    c[k] += p[i][k] * pa;
            }
            for (int k = 0; k < 3; ++k)
                out[k] = static_cast<uint8_t>(a > 0 ? std::clamp(std::lround(c[k] / a), 0l, 255l) : 0);
            out[3] = static_cast<uint8_t>(std::clamp(std::lround(a), 0l, 255l));
        }
    }

    bool RenderCrop(const canvas::Raster& source, const CropPlan& plan, canvas::Raster& out)
    {
        out = {};
        if (source.Empty() || plan.box.w <= 0 || plan.box.h <= 0 || !out.Allocate(static_cast<UINT>(plan.box.w), static_cast<UINT>(plan.box.h)))
            return false;
        out.dpiX = (plan.quarterTurns & 1) ? source.dpiY : source.dpiX;
        out.dpiY = (plan.quarterTurns & 1) ? source.dpiX : source.dpiY;
        out.icc = source.icc;
        const int W = static_cast<int>(source.width), H = static_cast<int>(source.height);
        const int q = plan.quarterTurns & 3;
        if (plan.degrees == 0)
        {
            for (int v = 0; v < plan.box.h; ++v)
                for (int u = 0; u < plan.box.w; ++u)
                {
                    int sx = 0, sy = 0;
                    SourceOf(q, plan.box.x + u, plan.box.y + v, W, H, sx, sy);
                    sx = std::clamp(sx, 0, W - 1);
                    sy = std::clamp(sy, 0, H - 1);
                    std::copy_n(source.At(static_cast<UINT>(sx), static_cast<UINT>(sy)), 4, out.At(static_cast<UINT>(u), static_cast<UINT>(v)));
                }
            return true;
        }
        const double t = plan.degrees * std::numbers::pi / 180.0;
        const double c = std::cos(t), s = std::sin(t);
        const double cx = plan.rotatedW / 2.0, cy = plan.rotatedH / 2.0;
        for (int v = 0; v < plan.box.h; ++v)
            for (int u = 0; u < plan.box.w; ++u)
            {
                // Undo the clockwise straighten (y points down), then the quarter turns.
                const double dx = plan.box.x + u + 0.5 - cx, dy = plan.box.y + v + 0.5 - cy;
                const double qx = c * dx + s * dy + cx, qy = -s * dx + c * dy + cy;
                double sx = qx, sy = qy;
                switch (q)
                {
                case 1: sx = qy, sy = H - qx; break;
                case 2: sx = W - qx, sy = H - qy; break;
                case 3: sx = W - qy, sy = qx; break;
                default: break;
                }
                Sample(source, sx, sy, out.At(static_cast<UINT>(u), static_cast<UINT>(v)));
            }
        return true;
    }
}
