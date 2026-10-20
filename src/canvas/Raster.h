#pragma once
#include "Action.h"

#include <windows.h>
#include <objidl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sd::canvas
{
    // A decoded picture in memory: what the image editors (Crop, Redact, later the M8 photo tools)
    // edit and what their engines render. 32 bpp BGRA with straight (not premultiplied) alpha,
    // top-down rows, no padding.
    struct Raster
    {
        UINT width = 0, height = 0;
        std::vector<uint8_t> px;
        double dpiX = 0, dpiY = 0;     // carried to the output (0 = unknown)
        std::vector<uint8_t> icc;      // the colour profile, carried so colours don't shift (not about the person)

        bool Empty() const { return width == 0 || height == 0; }
        uint8_t* At(UINT x, UINT y) { return px.data() + (static_cast<size_t>(y) * width + x) * 4; }
        const uint8_t* At(UINT x, UINT y) const { return px.data() + (static_cast<size_t>(y) * width + x) * 4; }
        bool Allocate(UINT w, UINT h);   // false if it would not fit in memory
    };

    // Decodes the first frame of `path` with its EXIF orientation applied, as the user sees it: WIC
    // for everything Windows decodes, libwebp for WebP, the format hooks (ImageHooks.h) for AVIF and
    // SVG. `maxEdge` > 0 scales it down (high-quality)
    // so its longer side is at most that, for previews. Needs COM on the calling thread.
    HRESULT LoadRaster(const std::wstring& path, Raster& out, std::wstring& error, UINT maxEdge = 0);

    // The format an edited copy of a file is written in: the source's own (JPEG, PNG, WebP, AVIF,
    // TIFF, BMP); JPEG for camera formats Windows can read but we don't always write (HEIC, JXR, raw);
    // PNG for the rest (GIF, ICO, SVG...).
    const ImageFormatInfo& EditedFormatFor(const std::wstring& lowerExtension);
    // JPEG 92, WebP 90, AVIF 80; ignored by lossless formats.
    int EditedQuality(const ImageFormatInfo& format);

    // Encodes into a brand-new container that holds only the pixels, the resolution and the colour
    // profile: no EXIF, GPS, XMP, IPTC, thumbnail or text. JPEG and BMP are flattened on white.
    HRESULT SaveRaster(const Raster& image, const ImageFormatInfo& format, int quality, IStream* out, std::wstring& error);
    HRESULT SaveRasterToFile(const Raster& image, const ImageFormatInfo& format, int quality, const std::wstring& path, std::wstring& error);
    // The same, into memory (for a container edit before the bytes go to disk).
    HRESULT EncodeRaster(const Raster& image, const ImageFormatInfo& format, int quality, std::vector<uint8_t>& bytes, std::wstring& error);
}
