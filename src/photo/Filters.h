#pragma once
#include "canvas/Raster.h"
#include "photo/Adjust.h"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <vector>

// Edit Photos on the CPU: the dehaze analysis (always here: it is a small statistical pass), and the
// whole pipeline as the reference the GPU effects are checked against and the fallback when the GPU
// can't run them. The per-pixel maths is Shade.h, shared with the shaders.
namespace sd::photo
{
    // One float per pixel, row by row.
    struct Plane
    {
        unsigned width = 0, height = 0;
        std::vector<float> v;
        Plane() = default;
        Plane(unsigned w, unsigned h, float fill = 0) : width(w), height(h), v(static_cast<size_t>(w) * h, fill) {}
        float& At(unsigned x, unsigned y) { return v[static_cast<size_t>(y) * width + x]; }
        float At(unsigned x, unsigned y) const { return v[static_cast<size_t>(y) * width + x]; }
        // Bilinear at a pixel position (pixel centres at x + 0.5), clamped at the edges.
        float Sample(double x, double y) const;
    };

    // Runs fn(begin, end) over [0, count) split across the cores.
    void ParallelFor(unsigned count, const std::function<void(unsigned, unsigned)>& fn);

    // Box mean over (2r+1)^2 pixels, divided by the pixels inside the picture (no darkening at edges).
    Plane BoxMean(const Plane& in, int radius);
    // Minimum over (2r+1)^2 pixels.
    Plane MinFilter(const Plane& in, int radius);
    // Gaussian blur: exact kernels for small sigmas, three box passes (the same to within a few
    // percent) for large ones; normalised at the edges like Direct2D's hard border mode.
    Plane Blur(const Plane& in, double sigma);
    // The guided filter (He, Sun and Tang): `p` smoothed along the edges of `guide`. Returns the
    // averaged coefficients so q = a * guide + b can be evaluated at any resolution.
    void GuidedCoefficients(const Plane& guide, const Plane& p, int radius, double eps, Plane& a, Plane& b);

    // The dark channel prior's model of a picture's haze: the atmospheric light and the fast guided
    // filter's coefficients on a small copy, so haze = a * luma + b follows the full picture's edges.
    struct HazeModel
    {
        float atmosphere[3] = { 1, 1, 1 };
        Plane a, b;
        bool Empty() const { return a.v.empty(); }
        // The haze (0..1) at a pixel of a `width` x `height` rendering whose luma there is `luma`.
        float At(double x, double y, unsigned width, unsigned height, float luma) const;
    };
    // `picture` at any size (it is reduced to about 512 px first).
    HazeModel AnalyseHaze(const canvas::Raster& picture);
    // The raw dark channel of a picture at its size (min over colour, then over the patch), for tests.
    Plane DarkChannel(const canvas::Raster& picture, int radius);

    // The whole Edit Photos pipeline on the CPU, in place: noise reduction, dehaze and develop,
    // clarity and sharpening, vignette and grain. `scale` is this raster's pixels per full-size pixel
    // (1 for an export). Returns false when cancelled.
    bool ApplyAdjustments(canvas::Raster& image, const Adjustments& a, const HazeModel* haze, double scale, std::stop_token stop = {});

    // A starting point from the picture's histogram: exposure to a pleasant mid-grey, whites and
    // blacks stretched to the ends, a touch of contrast and vibrance. No models, only statistics.
    Adjustments AutoAdjust(const canvas::Raster& picture);
}
