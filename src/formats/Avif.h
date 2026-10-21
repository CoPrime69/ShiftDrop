#pragma once
#include "ImageHooks.h"
#include "formats/Pixels.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// AVIF in and out through libavif: dav1d decodes (multi-threaded), aom encodes (tiled, multi-threaded).
// Both are BSD-licensed and linked statically.
namespace sd::formats
{
    // aom's speed (0 slowest .. 10 fastest). On a 12 MP photo at quality 60, 4 threads, speed 8 took
    // 0.28 s on the Windows CI runner against 1.01 s at libavif's default 6, for a file about 8%
    // bigger. Speed 7 took 0.93 s and speed 9 0.23 s, so 9 saves little more.
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

    // An animated AVIF (an image sequence), one frame at a time so a long animation is never all
    // in memory at once. Every frame must have the first one's size.
    class AvifSequence
    {
    public:
        explicit AvifSequence(const AvifOptions& options);
        ~AvifSequence();
        AvifSequence(const AvifSequence&) = delete;
        AvifSequence& operator=(const AvifSequence&) = delete;

        HRESULT Add(const Image& frame, int durationMs, std::wstring& error);
        HRESULT Finish(std::vector<uint8_t>& out, std::wstring& error);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };

    // Decodes the first frame, upright (irot/imir applied), 8-bit BGRA.
    HRESULT DecodeAvif(const uint8_t* data, size_t size, Image& out, std::wstring& error, int threads = 0);
    // The upright size from the header only.
    bool ReadAvifSize(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height);

    const IImageFormatHook& AvifHook();
}
