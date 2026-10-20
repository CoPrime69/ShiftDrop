#pragma once
#include "canvas/Geometry.h"
#include "canvas/Raster.h"

#include <cstdint>
#include <vector>

// Covers painted into a picture's pixels: what Redact Photos (and later Redact Video's stills and
// the photo tools) burn in. Plain CPU code on a Raster, so the output is exact and testable: inside a
// solid cover every pixel is the fill colour, and no pixel outside any cover ever changes.
namespace sd::canvas
{
    enum class CoverKind { Solid, Blur, Pixelate };

    struct Cover
    {
        RectI rect;
        CoverKind kind = CoverKind::Solid;
        uint32_t rgb = 0x000000;   // Solid: 0xRRGGBB, drawn opaque
    };

    // How strong a blur or pixelation is on an image of this size: in proportion to the picture, so
    // the editor's scaled-down preview looks like the result, and strong enough that text and faces
    // can't be read back. The same for every cover of an image.
    double BlurSigma(UINT imageWidth, UINT imageHeight);
    int PixelBlock(UINT imageWidth, UINT imageHeight);

    void ApplyCover(Raster& image, const Cover& cover);
    void ApplyCovers(Raster& image, const std::vector<Cover>& covers);

    // The same effects over a whole raster, for the editor's preview layers. A cover's pixels are
    // exactly these layers' pixels inside it (pixelation blocks line up with the image's corner).
    void BlurAll(Raster& image, double sigma);
    void PixelateAll(Raster& image, int block);
}
