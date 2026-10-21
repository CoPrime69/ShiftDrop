#pragma once
#include "pdf/Bitmap.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// WIC helpers for the PDF tools: decode an image file the way the wheel's converters see it, encode
// rendered pages, and the JPEG round trips that Compress PDF needs. WIC is free-threaded; call from
// any thread with COM initialised.
namespace sd::pdf
{
    // Decodes `path` upright (EXIF orientation applied) to BGRA. Formats WIC lacks but the app reads
    // anyway (WebP) go through the core image pipeline.
    bool DecodeImageFile(const std::wstring& path, Bitmap& out, std::wstring& error);

    // The file's resolution in dots per inch, or false when it has none (or WIC can't read it).
    bool ReadResolution(const std::wstring& path, double& dpiX, double& dpiY);
    // EXIF orientation (1-8) of a JPEG or TIFF file; 1 when there is none.
    int ReadOrientation(const std::wstring& path);

    // Writes an opaque bitmap as PNG or JPEG (quality 1-100) with `dpi` in its header.
    HRESULT EncodeImageFile(const Bitmap& image, const std::wstring& path, bool jpeg, int quality, double dpi);

    // Raw 8-bit samples: 1 channel (gray) or 3 (RGB, in that order, as PDF images store them); 4
    // (RGBA) only for EncodePng.
    struct Samples
    {
        uint32_t width = 0, height = 0, channels = 0;
        std::vector<uint8_t> data;   // rows top-down, no padding
    };
    HRESULT DecodeJpeg(const std::string& bytes, Samples& out);
    HRESULT EncodeJpeg(const Samples& in, int quality, std::string& out);
    // PNG in memory from 1 (gray), 3 (RGB) or 4 (RGB plus straight alpha) channel samples.
    HRESULT EncodePng(const Samples& in, std::string& out);
    // High-quality downscale (WIC Fant) to width x height.
    HRESULT ResizeSamples(Samples& samples, uint32_t width, uint32_t height);
}
