#pragma once
#include "ImageHooks.h"
#include "formats/Pixels.h"

#include <cstdint>
#include <string>
#include <vector>

// AVIF in and out through libavif: dav1d decodes (multi-threaded), aom encodes (tiled, multi-threaded).
// Both are BSD-licensed and linked statically.
namespace sd::formats
{
    // aom's speed (0 slowest .. 10 fastest). 8 measured about 3x faster than libavif's default 6 on
    // a 12 MP picture for a file about 5% bigger at the same quality (see docs/PLAN.md).
    inline constexpr int kAvifSpeed = 8;

    struct AvifOptions
    {
        int quality = 60;          // libavif's 0..100 (100 = lossless)
        int speed = kAvifSpeed;
        int threads = 0;           // 0 = CodecThreads()
    };

    // Chroma: full (4:4:4) at quality 90 and up, where colour edges matter more than bytes; 4:2:0 below.
    bool AvifFullChroma(int quality);

    HRESULT EncodeAvif(const Image& image, const AvifOptions& options, std::vector<uint8_t>& out, std::wstring& error);
    // Decodes the first frame, upright (irot/imir applied), 8-bit BGRA.
    HRESULT DecodeAvif(const uint8_t* data, size_t size, Image& out, std::wstring& error, int threads = 0);
    // The upright size from the header only.
    bool ReadAvifSize(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height);

    const IImageFormatHook& AvifHook();
}
