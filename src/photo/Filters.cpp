#include "photo/Filters.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace sd::photo
{
    using shade::float3;

    float Plane::Sample(double x, double y) const
    {
        if (v.empty())
            return 0;
        const double fx = std::clamp(x - 0.5, 0.0, width - 1.0), fy = std::clamp(y - 0.5, 0.0, height - 1.0);
        const unsigned x0 = static_cast<unsigned>(fx), y0 = static_cast<unsigned>(fy);
        const unsigned x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
        const float tx = static_cast<float>(fx - x0), ty = static_cast<float>(fy - y0);
        const float top = At(x0, y0) + (At(x1, y0) - At(x0, y0)) * tx;
        const float bottom = At(x0, y1) + (At(x1, y1) - At(x0, y1)) * tx;
        return top + (bottom - top) * ty;
    }

    void ParallelFor(unsigned count, const std::function<void(unsigned, unsigned)>& fn)
    {
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        const unsigned parts = std::clamp(count / 32, 1u, cores);
        if (parts <= 1)
        {
            fn(0, count);
            return;
        }
        std::vector<std::jthread> threads;
        threads.reserve(parts - 1);
        const unsigned per = (count + parts - 1) / parts;
        for (unsigned p = 1; p < parts; ++p)
        {
            const unsigned begin = p * per, end = std::min(count, begin + per);
            if (begin < end)
                threads.emplace_back([&fn, begin, end] { fn(begin, end); });
        }
        fn(0, std::min(count, per));
    }

    Plane BoxMean(const Plane& in, int radius)
    {
        const unsigned w = in.width, h = in.height;
        Plane rows(w, h), out(w, h);
        if (w == 0 || h == 0)
            return out;
        const int r = std::max(0, radius);
        // Horizontal running sums, then vertical ones; the divisor is the window's pixels inside.
        ParallelFor(h, [&](unsigned y0, unsigned y1) {
            for (unsigned y = y0; y < y1; ++y)
            {
                double sum = 0;
                const float* src = &in.v[static_cast<size_t>(y) * w];
                float* dst = &rows.v[static_cast<size_t>(y) * w];
                for (int x = 0; x < std::min<int>(r, w); ++x)
                    sum += src[x];
                for (int x = 0; x < static_cast<int>(w); ++x)
                {
                    if (x + r < static_cast<int>(w))
                        sum += src[x + r];
                    if (x - r - 1 >= 0)
                        sum -= src[x - r - 1];
                    dst[x] = static_cast<float>(sum);
                }
            }
        });
        ParallelFor(w, [&](unsigned x0, unsigned x1) {
            std::vector<double> sums(x1 - x0, 0.0);
            for (int y = 0; y < std::min<int>(r, h); ++y)
                for (unsigned x = x0; x < x1; ++x)
                    sums[x - x0] += rows.At(x, y);
            for (int y = 0; y < static_cast<int>(h); ++y)
            {
                const int cy = std::min<int>(h - 1, y + r) - std::max(0, y - r) + 1;
                for (unsigned x = x0; x < x1; ++x)
                {
                    double& s = sums[x - x0];
                    if (y + r < static_cast<int>(h))
                        s += rows.At(x, y + r);
                    if (y - r - 1 >= 0)
                        s -= rows.At(x, y - r - 1);
                    const int cx = std::min<int>(w - 1, static_cast<int>(x) + r) - std::max(0, static_cast<int>(x) - r) + 1;
                    out.At(x, y) = static_cast<float>(s / (static_cast<double>(cx) * cy));
                }
            }
        });
        return out;
    }

    Plane MinFilter(const Plane& in, int radius)
    {
        const unsigned w = in.width, h = in.height;
        Plane rows(w, h), out(w, h);
        const int r = std::max(0, radius);
        for (unsigned y = 0; y < h; ++y)
            for (int x = 0; x < static_cast<int>(w); ++x)
            {
                float m = 1e30f;
                for (int i = std::max(0, x - r); i <= std::min<int>(w - 1, x + r); ++i)
                    m = std::min(m, in.At(i, y));
                rows.At(x, y) = m;
            }
        for (int y = 0; y < static_cast<int>(h); ++y)
            for (unsigned x = 0; x < w; ++x)
            {
                float m = 1e30f;
                for (int j = std::max(0, y - r); j <= std::min<int>(h - 1, y + r); ++j)
                    m = std::min(m, rows.At(x, j));
                out.At(x, y) = m;
            }
        return out;
    }

    namespace
    {
        // Separable Gaussian with the kernel renormalised over the taps inside the picture.
        Plane ExactBlur(const Plane& in, double sigma)
        {
            const unsigned w = in.width, h = in.height;
            const int r = std::max(1, static_cast<int>(std::ceil(sigma * 3)));
            std::vector<double> k(2 * r + 1);
            for (int i = -r; i <= r; ++i)
                k[i + r] = std::exp(-(i * i) / (2 * sigma * sigma));
            Plane rows(w, h), out(w, h);
            ParallelFor(h, [&](unsigned y0, unsigned y1) {
                for (unsigned y = y0; y < y1; ++y)
                    for (int x = 0; x < static_cast<int>(w); ++x)
                    {
                        double s = 0, n = 0;
                        for (int i = std::max(-r, -x); i <= std::min(r, static_cast<int>(w) - 1 - x); ++i)
                        {
                            s += k[i + r] * in.At(x + i, y);
                            n += k[i + r];
                        }
                        rows.At(x, y) = static_cast<float>(s / n);
                    }
            });
            ParallelFor(h, [&](unsigned y0, unsigned y1) {
                for (unsigned y = y0; y < y1; ++y)
                    for (unsigned x = 0; x < w; ++x)
                    {
                        double s = 0, n = 0;
                        for (int j = std::max(-r, -static_cast<int>(y)); j <= std::min(r, static_cast<int>(h) - 1 - static_cast<int>(y)); ++j)
                        {
                            s += k[j + r] * rows.At(x, y + j);
                            n += k[j + r];
                        }
                        out.At(x, y) = static_cast<float>(s / n);
                    }
            });
            return out;
        }
    }

    Plane Blur(const Plane& in, double sigma)
    {
        if (sigma <= 0.01 || in.v.empty())
            return in;
        if (sigma < 3)
            return ExactBlur(in, sigma);
        // Three box passes whose widths give this sigma (Kovesi, "Fast almost-Gaussian filtering").
        constexpr int n = 3;
        const double ideal = std::sqrt(12 * sigma * sigma / n + 1);
        int wl = static_cast<int>(std::floor(ideal));
        if (wl % 2 == 0)
            --wl;
        const int wu = wl + 2;
        const int m = static_cast<int>(std::lround((12 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4)));
        Plane out = in;
        for (int i = 0; i < n; ++i)
            out = BoxMean(out, ((i < m ? wl : wu) - 1) / 2);
        return out;
    }

    void GuidedCoefficients(const Plane& guide, const Plane& p, int radius, double eps, Plane& a, Plane& b)
    {
        const unsigned w = guide.width, h = guide.height;
        Plane ii(w, h), ip(w, h);
        for (size_t i = 0; i < guide.v.size(); ++i)
        {
            ii.v[i] = guide.v[i] * guide.v[i];
            ip.v[i] = guide.v[i] * p.v[i];
        }
        const Plane meanI = BoxMean(guide, radius), meanP = BoxMean(p, radius);
        const Plane corrI = BoxMean(ii, radius), corrIp = BoxMean(ip, radius);
        Plane ra(w, h), rb(w, h);
        for (size_t i = 0; i < guide.v.size(); ++i)
        {
            const double var = corrI.v[i] - meanI.v[i] * meanI.v[i];
            const double cov = corrIp.v[i] - meanI.v[i] * meanP.v[i];
            const double k = cov / (var + eps);
            ra.v[i] = static_cast<float>(k);
            rb.v[i] = static_cast<float>(meanP.v[i] - k * meanI.v[i]);
        }
        a = BoxMean(ra, radius);
        b = BoxMean(rb, radius);
    }

    float HazeModel::At(double x, double y, unsigned width, unsigned height, float luma) const
    {
        if (Empty() || width == 0 || height == 0)
            return 0;
        const double sx = static_cast<double>(a.width) / width, sy = static_cast<double>(a.height) / height;
        const double px = (x + 0.5) * sx, py = (y + 0.5) * sy;
        return std::clamp(a.Sample(px, py) * luma + b.Sample(px, py), 0.f, 1.f);
    }

    namespace
    {
        // A small straight-alpha copy as floats, averaged down to at most `maxEdge`.
        struct Small
        {
            unsigned width = 0, height = 0;
            std::vector<float3> rgb;
        };

        Small Reduce(const canvas::Raster& src, unsigned maxEdge)
        {
            Small out;
            if (src.Empty())
                return out;
            const double s = std::min(1.0, static_cast<double>(maxEdge) / std::max(src.width, src.height));
            out.width = std::max(1u, static_cast<unsigned>(std::lround(src.width * s)));
            out.height = std::max(1u, static_cast<unsigned>(std::lround(src.height * s)));
            out.rgb.resize(static_cast<size_t>(out.width) * out.height);
            const double fx = static_cast<double>(src.width) / out.width, fy = static_cast<double>(src.height) / out.height;
            ParallelFor(out.height, [&](unsigned yb, unsigned ye) {
                for (unsigned y = yb; y < ye; ++y)
                {
                    const unsigned y0 = static_cast<unsigned>(y * fy);
                    const unsigned y1 = std::max(y0 + 1, std::min(src.height, static_cast<unsigned>(std::ceil((y + 1) * fy))));
                    for (unsigned x = 0; x < out.width; ++x)
                    {
                        const unsigned x0 = static_cast<unsigned>(x * fx);
                        const unsigned x1 = std::max(x0 + 1, std::min(src.width, static_cast<unsigned>(std::ceil((x + 1) * fx))));
                        double sum[3]{};
                        for (unsigned sy = y0; sy < y1; ++sy)
                            for (unsigned sx = x0; sx < x1; ++sx)
                            {
                                const uint8_t* p = src.At(sx, sy);
                                sum[0] += p[2], sum[1] += p[1], sum[2] += p[0];
                            }
                        const double n = 255.0 * (x1 - x0) * (y1 - y0);
                        out.rgb[static_cast<size_t>(y) * out.width + x] = float3(float(sum[0] / n), float(sum[1] / n), float(sum[2] / n));
                    }
                }
            });
            return out;
        }

        int PatchRadius(unsigned w, unsigned h) { return std::max(1, static_cast<int>(std::lround(7.0 * std::max(w, h) / 512.0))); }
    }

    Plane DarkChannel(const canvas::Raster& picture, int radius)
    {
        Plane m(picture.width, picture.height);
        for (unsigned y = 0; y < picture.height; ++y)
            for (unsigned x = 0; x < picture.width; ++x)
            {
                const uint8_t* p = picture.At(x, y);
                m.At(x, y) = std::min({ p[0], p[1], p[2] }) / 255.f;
            }
        return MinFilter(m, radius);
    }

    HazeModel AnalyseHaze(const canvas::Raster& picture)
    {
        HazeModel model;
        const Small s = Reduce(picture, 512);
        if (s.rgb.empty())
            return model;
        const unsigned w = s.width, h = s.height;
        const int r = PatchRadius(w, h);

        // The atmospheric light: the mean colour of the 0.1% of pixels whose dark channel is highest
        // (the haziest, most distant part of the scene).
        Plane mins(w, h);
        for (size_t i = 0; i < s.rgb.size(); ++i)
            mins.v[i] = std::min({ s.rgb[i].x, s.rgb[i].y, s.rgb[i].z });
        const Plane dark = MinFilter(mins, r);
        std::vector<size_t> order(dark.v.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        const size_t top = std::max<size_t>(1, order.size() / 1000);
        std::partial_sort(order.begin(), order.begin() + top, order.end(), [&](size_t a, size_t b) { return dark.v[a] > dark.v[b]; });
        double atm[3]{};
        for (size_t i = 0; i < top; ++i)
        {
            atm[0] += s.rgb[order[i]].x;
            atm[1] += s.rgb[order[i]].y;
            atm[2] += s.rgb[order[i]].z;
        }
        for (int c = 0; c < 3; ++c)
            model.atmosphere[c] = static_cast<float>(std::clamp(atm[c] / top, 0.2, 1.0));

        // The haze: the dark channel of the picture over the atmospheric light, refined along the
        // picture's edges by the guided filter (luma as the guide).
        Plane normalised(w, h), guide(w, h);
        for (size_t i = 0; i < s.rgb.size(); ++i)
        {
            const float3 c = s.rgb[i];
            normalised.v[i] = std::min({ c.x / model.atmosphere[0], c.y / model.atmosphere[1], c.z / model.atmosphere[2] });
            guide.v[i] = shade::Luma(c);
        }
        const Plane raw = MinFilter(normalised, r);
        GuidedCoefficients(guide, raw, r * 4, 1e-3, model.a, model.b);
        return model;
    }

    namespace
    {
        constexpr float kInv255 = 1.f / 255.f;

        float3 Rgb(const uint8_t* p) { return float3(p[2] * kInv255, p[1] * kInv255, p[0] * kInv255); }

        uint8_t Byte(float v) { return static_cast<uint8_t>(std::clamp(std::lround(v * 255.f), 0l, 255l)); }

        void Denoise(std::vector<float3>& rgb, const std::vector<float>& alpha, unsigned w, unsigned h, const shade::DenoiseK& k)
        {
            const std::vector<float3> src = rgb;
            const int step = static_cast<int>(k.step);
            ParallelFor(h, [&](unsigned y0, unsigned y1) {
                for (unsigned y = y0; y < y1; ++y)
                    for (unsigned x = 0; x < w; ++x)
                    {
                        const size_t at = static_cast<size_t>(y) * w + x;
                        const float l0 = shade::Luma(src[at]);
                        float3 sum(0, 0, 0);
                        float total = 0;
                        for (int j = -3; j <= 3; ++j)
                        {
                            const int sy = static_cast<int>(y) + j * step;
                            if (sy < 0 || sy >= static_cast<int>(h))
                                continue;
                            for (int i = -3; i <= 3; ++i)
                            {
                                const int sx = static_cast<int>(x) + i * step;
                                if (sx < 0 || sx >= static_cast<int>(w))
                                    continue;
                                const size_t t = static_cast<size_t>(sy) * w + sx;
                                const float wt = shade::BilateralWeight(float(i * i + j * j), shade::Luma(src[t]) - l0, k) * alpha[t];
                                sum = sum + src[t] * wt;
                                total += wt;
                            }
                        }
                        if (total > 1e-6f)
                            rgb[at] = sum / total;
                    }
            });
        }
    }

    bool ApplyAdjustments(canvas::Raster& image, const Adjustments& a, const HazeModel* haze, double scale, std::stop_token stop)
    {
        if (image.Empty() || a.IsIdentity())
            return true;
        const unsigned w = image.width, h = image.height;
        const size_t n = static_cast<size_t>(w) * h;
        std::vector<float3> rgb(n);
        std::vector<float> alpha(n);
        bool opaque = true;
        for (size_t i = 0; i < n; ++i)
        {
            const uint8_t* p = image.px.data() + i * 4;
            rgb[i] = Rgb(p);
            alpha[i] = p[3] * kInv255;
            opaque &= p[3] == 255;
        }
        const unsigned fullW = static_cast<unsigned>(std::lround(w / scale)), fullH = static_cast<unsigned>(std::lround(h / scale));

        if (NeedsDenoise(a))
            Denoise(rgb, alpha, w, h, DenoiseConstants(a, fullW, fullH, scale));
        if (stop.stop_requested())
            return false;

        if (NeedsDevelop(a))
        {
            const bool useHaze = a.dehaze != 0 && haze && !haze->Empty();
            auto k = DevelopConstants(a, useHaze ? haze->atmosphere : nullptr);
            if (!useHaze)
                k.dehaze = 0;   // no model of the haze (yet): nothing to clear
            ParallelFor(h, [&](unsigned y0, unsigned y1) {
                for (unsigned y = y0; y < y1; ++y)
                    for (unsigned x = 0; x < w; ++x)
                    {
                        float3& c = rgb[static_cast<size_t>(y) * w + x];
                        const float hz = useHaze ? haze->At(x, y, w, h, shade::Luma(c)) : 0.f;
                        c = shade::Develop(c, hz, k);
                    }
            });
        }
        if (stop.stop_requested())
            return false;

        if (NeedsFinish(a))
        {
            Plane smallBlur, largeBlur;
            if (NeedsBlur(a))
            {
                // Luma blurred with the alpha as weight, so transparent and outside pixels count for nothing.
                Plane luma(w, h), weight(w, h);
                for (size_t i = 0; i < n; ++i)
                {
                    luma.v[i] = shade::Luma(rgb[i]) * alpha[i];
                    weight.v[i] = alpha[i];
                }
                const Radii radii = RadiiFor(fullW, fullH, a);
                const auto blurred = [&](double sigma) {
                    Plane l = Blur(luma, sigma * scale);
                    if (!opaque)
                    {
                        const Plane wt = Blur(weight, sigma * scale);
                        for (size_t i = 0; i < n; ++i)
                            l.v[i] = wt.v[i] > 1e-5f ? l.v[i] / wt.v[i] : 0.f;
                    }
                    return l;
                };
                if (a.sharpen != 0)
                    smallBlur = blurred(radii.sharpenSigma);
                if (a.clarity != 0)
                    largeBlur = blurred(radii.claritySigma);
            }
            if (stop.stop_requested())
                return false;
            const auto k = FinishConstants(a, w, h, scale);
            ParallelFor(h, [&](unsigned y0, unsigned y1) {
                for (unsigned y = y0; y < y1; ++y)
                    for (unsigned x = 0; x < w; ++x)
                    {
                        const size_t i = static_cast<size_t>(y) * w + x;
                        float3& c = rgb[i];
                        // Finish reads only the blurs' luma, so a grey of that luma stands for each.
                        const float ls = smallBlur.v.empty() ? shade::Luma(c) : smallBlur.v[i];
                        const float ll = largeBlur.v.empty() ? shade::Luma(c) : largeBlur.v[i];
                        c = shade::Finish(c, float3(ls, ls, ls), float3(ll, ll, ll), x + 0.5f, y + 0.5f, k);
                    }
            });
        }

        for (size_t i = 0; i < n; ++i)
        {
            uint8_t* p = image.px.data() + i * 4;
            p[2] = Byte(rgb[i].x);
            p[1] = Byte(rgb[i].y);
            p[0] = Byte(rgb[i].z);
        }
        return true;
    }

    Adjustments AutoAdjust(const canvas::Raster& picture)
    {
        Adjustments a;
        const Small s = Reduce(picture, 512);
        if (s.rgb.empty())
            return a;
        std::vector<float> luma(s.rgb.size());
        for (size_t i = 0; i < luma.size(); ++i)
            luma[i] = shade::Luma(s.rgb[i]);
        const auto pct = [&](double q) {
            const size_t k = std::min(luma.size() - 1, static_cast<size_t>(q * (luma.size() - 1)));
            std::nth_element(luma.begin(), luma.begin() + k, luma.end());
            return luma[k];
        };
        const float low = pct(0.01), median = pct(0.5), high = pct(0.99);
        // Mid-grey: exposure moves the median towards 0.46 (in display values, so 2.2 stops per doubling).
        if (median > 0.01f)
            a.exposure = std::clamp(std::round(2.2 * std::log2(0.46 / median) * 0.7 * 20) / 20, -1.5, 1.5);
        if (high < 0.92f)
            a.whites = std::clamp(std::round((0.96 - high) * 160), 0.0, 40.0);
        if (low > 0.05f)
            a.blacks = -std::clamp(std::round((low - 0.02) * 260), 0.0, 40.0);
        a.contrast = 10;
        a.vibrance = 15;
        return a;
    }
}
