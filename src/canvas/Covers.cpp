#include "canvas/Covers.h"

#include <algorithm>
#include <cmath>

namespace sd::canvas
{
    namespace
    {
        // Box blur of one line of `len` pixels (4 channels each, `step` bytes apart), edges extended.
        void BoxLine(uint8_t* line, size_t step, int len, int r, std::vector<uint8_t>& tmp)
        {
            if (len <= 1 || r <= 0)
                return;
            tmp.resize(static_cast<size_t>(len) * 4);
            for (int i = 0; i < len; ++i)
                std::copy_n(line + static_cast<size_t>(i) * step, 4, tmp.data() + static_cast<size_t>(i) * 4);
            const int window = 2 * r + 1;
            for (int c = 0; c < 4; ++c)
            {
                const auto at = [&](int i) { return static_cast<int>(tmp[static_cast<size_t>(std::clamp(i, 0, len - 1)) * 4 + c]); };
                int sum = 0;
                for (int k = -r; k <= r; ++k)
                    sum += at(k);
                for (int i = 0; i < len; ++i)
                {
                    line[static_cast<size_t>(i) * step + c] = static_cast<uint8_t>((sum + window / 2) / window);
                    sum += at(i + r + 1) - at(i - r);
                }
            }
        }

        void Premultiply(uint8_t* p)
        {
            const unsigned a = p[3];
            for (int c = 0; c < 3; ++c)
                p[c] = static_cast<uint8_t>((p[c] * a + 127) / 255);
        }

        void Unpremultiply(uint8_t* p)
        {
            const unsigned a = p[3];
            if (a == 0 || a == 255)
                return;
            for (int c = 0; c < 3; ++c)
                p[c] = static_cast<uint8_t>(std::min(255u, (p[c] * 255 + a / 2) / a));
        }

        int BoxRadius(double sigma)
        {
            // Three box passes of width w have the variance of a Gaussian with sigma^2 = (w^2 - 1) / 4.
            const double w = std::sqrt(4 * sigma * sigma + 1);
            return std::max(1, static_cast<int>(std::lround((w - 1) / 2)));
        }

        // Blurs `area` of `image` and writes the result back only inside `target` (inside `area`). The
        // area must reach far enough past the target (3 radii) for the result to equal a blur of the
        // whole picture.
        void BlurInto(Raster& image, const RectI& area, const RectI& target, double sigma)
        {
            if (area.w <= 0 || area.h <= 0)
                return;
            const int r = BoxRadius(sigma);
            std::vector<uint8_t> work(static_cast<size_t>(area.w) * area.h * 4);
            for (int y = 0; y < area.h; ++y)
            {
                std::copy_n(image.At(static_cast<UINT>(area.x), static_cast<UINT>(area.y + y)), static_cast<size_t>(area.w) * 4,
                            work.data() + static_cast<size_t>(y) * area.w * 4);
            }
            for (size_t i = 0; i < work.size(); i += 4)
                Premultiply(work.data() + i);
            std::vector<uint8_t> tmp;
            for (int pass = 0; pass < 3; ++pass)
            {
                for (int y = 0; y < area.h; ++y)
                    BoxLine(work.data() + static_cast<size_t>(y) * area.w * 4, 4, area.w, r, tmp);
                for (int x = 0; x < area.w; ++x)
                    BoxLine(work.data() + static_cast<size_t>(x) * 4, static_cast<size_t>(area.w) * 4, area.h, r, tmp);
            }
            for (int y = target.y; y < target.y + target.h; ++y)
                for (int x = target.x; x < target.x + target.w; ++x)
                {
                    uint8_t* d = image.At(static_cast<UINT>(x), static_cast<UINT>(y));
                    const uint8_t* s = work.data() + (static_cast<size_t>(y - area.y) * area.w + static_cast<size_t>(x - area.x)) * 4;
                    std::copy_n(s, 4, d);
                    Unpremultiply(d);
                }
        }

        void PixelateInto(Raster& image, const RectI& target, int block)
        {
            block = std::max(1, block);
            const int bx0 = target.x / block * block, by0 = target.y / block * block;
            for (int by = by0; by < target.y + target.h; by += block)
                for (int bx = bx0; bx < target.x + target.w; bx += block)
                {
                    // The average of the whole block (premultiplied, so transparent pixels don't darken it).
                    const int x1 = std::min<int>(bx + block, static_cast<int>(image.width)), y1 = std::min<int>(by + block, static_cast<int>(image.height));
                    uint64_t sum[4]{};
                    uint64_t n = 0;
                    for (int y = by; y < y1; ++y)
                        for (int x = bx; x < x1; ++x)
                        {
                            const uint8_t* p = image.At(static_cast<UINT>(x), static_cast<UINT>(y));
                            for (int c = 0; c < 3; ++c)
                                sum[c] += uint64_t{ p[c] } * p[3];
                            sum[3] += p[3];
                            ++n;
                        }
                    if (n == 0)
                        continue;
                    uint8_t avg[4];
                    avg[3] = static_cast<uint8_t>((sum[3] + n / 2) / n);
                    for (int c = 0; c < 3; ++c)
                        avg[c] = sum[3] ? static_cast<uint8_t>(std::min<uint64_t>(255, (sum[c] + sum[3] / 2) / sum[3])) : 0;
                    for (int y = std::max(by, target.y); y < std::min(y1, target.y + target.h); ++y)
                        for (int x = std::max(bx, target.x); x < std::min(x1, target.x + target.w); ++x)
                            std::copy_n(avg, 4, image.At(static_cast<UINT>(x), static_cast<UINT>(y)));
                }
        }

        RectI Clip(const RectI& r, const Raster& image)
        {
            const int l = std::clamp(r.x, 0, static_cast<int>(image.width)), t = std::clamp(r.y, 0, static_cast<int>(image.height));
            const int rr = std::clamp(r.x + r.w, 0, static_cast<int>(image.width)), b = std::clamp(r.y + r.h, 0, static_cast<int>(image.height));
            return { l, t, std::max(0, rr - l), std::max(0, b - t) };
        }
    }

    double BlurSigma(UINT w, UINT h)
    {
        return std::max(8.0, std::max(w, h) / 100.0);
    }

    int PixelBlock(UINT w, UINT h)
    {
        return std::max(10, static_cast<int>(std::max(w, h) / 60));
    }

    void ApplyCover(Raster& image, const Cover& cover)
    {
        const RectI r = Clip(cover.rect, image);
        if (r.w <= 0 || r.h <= 0)
            return;
        switch (cover.kind)
        {
        case CoverKind::Solid:
        {
            const uint8_t fill[4] = { static_cast<uint8_t>(cover.rgb & 0xFF), static_cast<uint8_t>((cover.rgb >> 8) & 0xFF),
                                      static_cast<uint8_t>((cover.rgb >> 16) & 0xFF), 255 };
            for (int y = r.y; y < r.y + r.h; ++y)
                for (int x = r.x; x < r.x + r.w; ++x)
                    std::copy_n(fill, 4, image.At(static_cast<UINT>(x), static_cast<UINT>(y)));
            break;
        }
        case CoverKind::Blur:
        {
            const double sigma = BlurSigma(image.width, image.height);
            const int margin = 3 * BoxRadius(sigma) + 1;
            BlurInto(image, Clip({ r.x - margin, r.y - margin, r.w + 2 * margin, r.h + 2 * margin }, image), r, sigma);
            break;
        }
        case CoverKind::Pixelate:
            PixelateInto(image, r, PixelBlock(image.width, image.height));
            break;
        }
    }

    void ApplyCovers(Raster& image, const std::vector<Cover>& covers)
    {
        // Solid covers first, so a blur next to one samples the fill and not what it hides; then blur
        // and pixelate; then the solid covers again, so one overlapping a blur stays solid.
        for (const auto& c : covers)
            if (c.kind == CoverKind::Solid)
                ApplyCover(image, c);
        for (const auto& c : covers)
            if (c.kind != CoverKind::Solid)
                ApplyCover(image, c);
        for (const auto& c : covers)
            if (c.kind == CoverKind::Solid)
                ApplyCover(image, c);
    }

    void BlurAll(Raster& image, double sigma)
    {
        const RectI all{ 0, 0, static_cast<int>(image.width), static_cast<int>(image.height) };
        BlurInto(image, all, all, sigma);
    }

    void PixelateAll(Raster& image, int block)
    {
        PixelateInto(image, { 0, 0, static_cast<int>(image.width), static_cast<int>(image.height) }, block);
    }
}
