#include "photo/Layout.h"
#include "Recipe.h"
#include "Utf.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace sd::photo
{
    namespace
    {
        // Whole-pixel rectangle with rounded edges, so neighbouring cells share their gaps exactly.
        RectI Round(double l, double t, double r, double b)
        {
            const int x = static_cast<int>(std::lround(l)), y = static_cast<int>(std::lround(t));
            return { x, y, std::max(1, static_cast<int>(std::lround(r)) - x), std::max(1, static_cast<int>(std::lround(b)) - y) };
        }

        uint32_t Colour(const nlohmann::json& j, const char* key, uint32_t fallback)
        {
            const auto it = j.find(key);
            return it != j.end() && it->is_number_unsigned() ? it->get<uint32_t>() & 0xFFFFFF : fallback;
        }

        std::string Aspect(const nlohmann::json& j)
        {
            const std::string id = j.value("aspect", std::string("auto"));
            return AspectRatio(id) > 0 ? id : std::string("auto");
        }
    }

    const std::vector<AspectChoice>& AspectChoices()
    {
        static const std::vector<AspectChoice> choices = {
            { "auto", L"Auto", 0 },      { "1:1", L"1:1", 1.0 },         { "4:5", L"4:5", 0.8 },   { "4:3", L"4:3", 4.0 / 3 },
            { "3:2", L"3:2", 1.5 },      { "16:9", L"16:9", 16.0 / 9 }, { "9:16", L"9:16", 9.0 / 16 },
        };
        return choices;
    }

    double AspectRatio(const std::string& id)
    {
        for (const auto& c : AspectChoices())
            if (id == c.id)
                return c.ratio > 0 ? c.ratio : 0;
        return 0;
    }

    // ---- Background ---------------------------------------------------------------------------------

    BackgroundParams ParseBackground(const nlohmann::json& j)
    {
        BackgroundParams p;
        if (!j.is_object())
            return p;
        const std::string fill = j.value("fill", std::string("gradient"));
        p.fill = fill == "solid" ? Fill::Solid : fill == "blur" ? Fill::Blur : fill == "image" ? Fill::Image : Fill::Gradient;
        p.color1 = Colour(j, "color1", p.color1);
        p.color2 = Colour(j, "color2", p.color2);
        p.angle = JsonNumber(j, "angle", p.angle, 0, 360);
        p.image = Widen(j.value("image", std::string()));
        if (p.fill == Fill::Image && p.image.empty())
            p.fill = Fill::Blur;
        p.blur = JsonNumber(j, "blur", p.blur, 0, 100);
        p.aspect = Aspect(j);
        p.padding = JsonNumber(j, "padding", p.padding, 0, 40);
        p.radius = JsonNumber(j, "radius", p.radius, 0, 100);
        p.shadow = JsonNumber(j, "shadow", p.shadow, 0, 100);
        return p;
    }

    nlohmann::json ToJson(const BackgroundParams& p)
    {
        static const char* const fills[] = { "solid", "gradient", "blur", "image" };
        return { { "fill", fills[static_cast<int>(p.fill)] }, { "color1", p.color1 }, { "color2", p.color2 }, { "angle", p.angle },
                 { "image", Narrow(p.image) }, { "blur", p.blur }, { "aspect", p.aspect }, { "padding", p.padding },
                 { "radius", p.radius }, { "shadow", p.shadow } };
    }

    BackgroundLayout LayoutBackground(int pw, int ph, const BackgroundParams& p, int maxEdge)
    {
        BackgroundLayout l;
        pw = std::max(1, pw);
        ph = std::max(1, ph);
        const int longSide = std::max(pw, ph);
        int pad = static_cast<int>(std::lround(longSide * p.padding / 100.0));
        pad = std::clamp(pad, 0, std::max(0, (maxEdge - longSide) / 2));
        int w = pw + 2 * pad, h = ph + 2 * pad;
        if (const double ratio = AspectRatio(p.aspect); ratio > 0)
        {
            // The smallest canvas of that shape that holds the picture and its padding.
            if (static_cast<double>(w) / h < ratio)
                w = static_cast<int>(std::lround(h * ratio));
            else
                h = static_cast<int>(std::lround(w / ratio));
            if (std::max(w, h) > maxEdge)
            {
                const double k = static_cast<double>(maxEdge) / std::max(w, h);
                w = std::max(pw, static_cast<int>(w * k));
                h = std::max(ph, static_cast<int>(h * k));
            }
        }
        l.width = w;
        l.height = h;
        l.picture = { (w - pw) / 2, (h - ph) / 2, pw, ph };
        l.radius = std::min(pw, ph) * 0.25 * p.radius / 100.0;
        if (p.shadow > 0)
        {
            l.shadowSigma = longSide * 0.03 * p.shadow / 100.0;
            l.shadowOffset = l.shadowSigma * 0.35;
            l.shadowOpacity = 0.25 + 0.35 * p.shadow / 100.0;
        }
        return l;
    }

    double BackgroundBlurSigma(const BackgroundLayout& layout, double blur) { return std::max(layout.width, layout.height) * 0.05 * blur / 100.0; }

    RectD CoverSource(SizeD s, SizeD t)
    {
        if (s.w <= 0 || s.h <= 0 || t.w <= 0 || t.h <= 0)
            return { 0, 0, s.w, s.h };
        const double want = t.w / t.h;
        if (s.w / s.h > want)
        {
            const double w = s.h * want;
            return { (s.w - w) / 2, 0, w, s.h };
        }
        const double h = s.w / want;
        return { 0, (s.h - h) / 2, s.w, h };
    }

    RectD FitInside(SizeD s, const RectD& t)
    {
        if (s.w <= 0 || s.h <= 0)
            return t;
        const double k = std::min(t.w / s.w, t.h / s.h);
        const double w = s.w * k, h = s.h * k;
        return { t.x + (t.w - w) / 2, t.y + (t.h - h) / 2, w, h };
    }

    // ---- Collage ------------------------------------------------------------------------------------

    CollageParams ParseCollage(const nlohmann::json& j)
    {
        CollageParams p;
        if (!j.is_object())
            return p;
        const std::string kind = j.value("layout", std::string("grid"));
        p.kind = kind == "row" ? CollageKind::Row : kind == "column" ? CollageKind::Column : kind == "featured" ? CollageKind::Featured : CollageKind::Grid;
        if (const auto it = j.find("order"); it != j.end() && it->is_array())
            for (const auto& v : *it)
                if (v.is_number_integer())
                    p.order.push_back(v.get<int>());
        p.size = static_cast<int>(JsonNumber(j, "size", p.size, 256, 16384));
        p.aspect = Aspect(j);
        p.spacing = JsonNumber(j, "spacing", p.spacing, 0, 100);
        p.radius = JsonNumber(j, "radius", p.radius, 0, 100);
        p.background = Colour(j, "background", p.background);
        p.fit = j.value("fit", false);
        return p;
    }

    nlohmann::json ToJson(const CollageParams& p)
    {
        static const char* const kinds[] = { "grid", "row", "column", "featured" };
        return { { "layout", kinds[static_cast<int>(p.kind)] }, { "order", p.order }, { "size", p.size }, { "aspect", p.aspect },
                 { "spacing", p.spacing }, { "radius", p.radius }, { "background", p.background }, { "fit", p.fit } };
    }

    std::vector<int> NormalisedOrder(const std::vector<int>& order, size_t count)
    {
        std::vector<int> out;
        std::vector<bool> seen(count, false);
        for (int i : order)
            if (i >= 0 && static_cast<size_t>(i) < count && !seen[static_cast<size_t>(i)])
            {
                seen[static_cast<size_t>(i)] = true;
                out.push_back(i);
            }
        for (size_t i = 0; i < count; ++i)
            if (!seen[i])
                out.push_back(static_cast<int>(i));
        return out;
    }

    namespace
    {
        // `count` cells in `rows` rows filling a rectangle; the last row's cells widen to fill it.
        void GridCells(const RectD& area, size_t count, size_t cols, double gap, std::vector<RectI>& out)
        {
            if (count == 0)
                return;
            cols = std::max<size_t>(1, std::min(cols, count));
            const size_t rows = (count + cols - 1) / cols;
            const double rh = (area.h - gap * (rows - 1)) / rows;
            for (size_t r = 0; r < rows; ++r)
            {
                const size_t inRow = std::min(cols, count - r * cols);
                const double cw = (area.w - gap * (inRow - 1)) / inRow;
                const double top = area.y + r * (rh + gap);
                for (size_t c = 0; c < inRow; ++c)
                {
                    const double left = area.x + c * (cw + gap);
                    out.push_back(Round(left, top, left + cw, top + rh));
                }
            }
        }

        double Aspect(SizeD s) { return s.w > 0 && s.h > 0 ? std::clamp(s.w / s.h, 0.1, 10.0) : 1.0; }
    }

    CollageLayout LayoutCollage(const std::vector<SizeD>& sizes, const CollageParams& p, int maxEdge)
    {
        CollageLayout l;
        const auto order = NormalisedOrder(p.order, sizes.size());
        const size_t n = order.size();
        if (n == 0)
            return l;
        const double longSide = std::clamp(p.size, 64, std::max(64, maxEdge));
        const double gap = std::round(longSide * 0.04 * p.spacing / 100.0);
        std::vector<double> aspects;
        for (int i : order)
            aspects.push_back(Aspect(sizes[static_cast<size_t>(i)]));
        const double sum = std::accumulate(aspects.begin(), aspects.end(), 0.0);
        std::vector<RectI> rects;

        double W = 0, H = 0;
        const auto canvasOfRatio = [&](double ratio) {
            ratio = std::clamp(ratio, 0.1, 10.0);
            if (ratio >= 1)
                W = longSide, H = std::round(longSide / ratio);
            else
                H = longSide, W = std::round(longSide * ratio);
        };

        switch (p.kind)
        {
        case CollageKind::Row:
        {
            // One row, every photo whole at the same height.
            double h = (longSide - gap * (n + 1)) / sum;
            W = longSide, H = std::round(h + 2 * gap);
            if (H > W)
            {
                h = longSide - 2 * gap;
                H = longSide, W = std::round(h * sum + gap * (n + 1));
            }
            double x = gap;
            for (size_t i = 0; i < n; ++i)
            {
                const double w = h * aspects[i];
                rects.push_back(Round(x, gap, x + w, gap + h));
                x += w + gap;
            }
            break;
        }
        case CollageKind::Column:
        {
            double inv = 0;
            for (double a : aspects)
                inv += 1 / a;
            double w = (longSide - gap * (n + 1)) / inv;
            H = longSide, W = std::round(w + 2 * gap);
            if (W > H)
            {
                w = longSide - 2 * gap;
                W = longSide, H = std::round(w * inv + gap * (n + 1));
            }
            double y = gap;
            for (size_t i = 0; i < n; ++i)
            {
                const double h = w / aspects[i];
                rects.push_back(Round(gap, y, gap + w, y + h));
                y += h + gap;
            }
            break;
        }
        case CollageKind::Grid:
        {
            const size_t cols = static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(n))));
            const size_t rows = (n + cols - 1) / cols;
            const double ratio = AspectRatio(p.aspect);
            canvasOfRatio(ratio > 0 ? ratio : std::clamp(sum / n, 0.5, 2.0) * cols / rows);
            GridCells({ gap, gap, W - 2 * gap, H - 2 * gap }, n, cols, gap, rects);
            break;
        }
        case CollageKind::Featured:
        {
            const double ratio = AspectRatio(p.aspect);
            canvasOfRatio(ratio > 0 ? ratio : std::clamp(aspects[0] * 1.5, 0.6, 2.5));
            const RectD area{ gap, gap, W - 2 * gap, H - 2 * gap };
            if (n == 1)
            {
                rects.push_back(Round(area.x, area.y, area.Right(), area.Bottom()));
                break;
            }
            const size_t rest = n - 1;
            const size_t lanes = rest > 3 ? 2 : 1;
            if (W >= H)
            {
                // The first photo on the left, two thirds of the width; the rest in a column (or two).
                const double mw = (area.w - gap) * 2 / 3;
                rects.push_back(Round(area.x, area.y, area.x + mw, area.Bottom()));
                const RectD side{ area.x + mw + gap, area.y, area.w - mw - gap, area.h };
                // Columns of cells: transpose a grid laid out with `lanes` rows.
                const size_t perLane = (rest + lanes - 1) / lanes;
                const double lw = (side.w - gap * (lanes - 1)) / lanes;
                for (size_t lane = 0, done = 0; lane < lanes; ++lane)
                {
                    const size_t here = std::min(perLane, rest - done);
                    const double ch = (side.h - gap * (here - 1)) / here;
                    for (size_t c = 0; c < here; ++c)
                    {
                        const double left = side.x + lane * (lw + gap), top = side.y + c * (ch + gap);
                        rects.push_back(Round(left, top, left + lw, top + ch));
                    }
                    done += here;
                }
            }
            else
            {
                const double mh = (area.h - gap) * 2 / 3;
                rects.push_back(Round(area.x, area.y, area.Right(), area.y + mh));
                const RectD side{ area.x, area.y + mh + gap, area.w, area.h - mh - gap };
                GridCells(side, rest, (rest + lanes - 1) / lanes, gap, rects);
            }
            break;
        }
        }

        l.width = static_cast<int>(W);
        l.height = static_cast<int>(H);
        l.gap = static_cast<int>(gap);
        double shortest = 1e9;
        for (size_t i = 0; i < n && i < rects.size(); ++i)
        {
            l.cells.push_back({ order[i], rects[i] });
            shortest = std::min<double>(shortest, std::min(rects[i].w, rects[i].h));
        }
        l.radius = shortest < 1e9 ? shortest * 0.25 * p.radius / 100.0 : 0;
        return l;
    }

    int CellAt(const CollageLayout& layout, canvas::PointD at)
    {
        for (size_t i = 0; i < layout.cells.size(); ++i)
        {
            const auto& r = layout.cells[i].rect;
            if (at.x >= r.x && at.x < r.x + r.w && at.y >= r.y && at.y < r.y + r.h)
                return static_cast<int>(i);
        }
        return -1;
    }
}
