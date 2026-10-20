#pragma once
#include "Action.h"
#include "Recipe.h"

#include <windows.h>
#include <objidl.h>

#include <cstdint>
#include <functional>
#include <set>
#include <stop_token>
#include <string>

namespace sd
{
    // What this PC can decode and encode. Built once at startup: WIC decoders depend on optional
    // Store codecs (HEIC, AVIF, ...), so the list is read at runtime. WebP is always available
    // through libwebp, whether or not the WebP Image Extension is installed.
    class ImageCodecs
    {
    public:
        ImageCodecs();   // needs COM initialised on the calling thread
        bool CanDecode(const std::wstring& lowerExtension) const;
        bool CanEncode(const ImageFormatInfo& format) const;
        const std::wstring& Summary() const { return m_summary; }

    private:
        std::set<std::wstring> m_decodable;
        std::wstring m_summary;
    };

    // The fused image pipeline: decode -> EXIF orientation -> transforms -> encode, all as a chain of
    // IWICBitmapSource objects, so a multi-step recipe never writes an intermediate file (ADR 0004).
    // Writes the encoded bytes to `out`. `error` gets a readable reason on failure.
    HRESULT RunImageRecipe(const Recipe& recipe, const std::wstring& source, IStream* out, std::stop_token stop,
                           std::wstring& error);

    // The highest quality in [low, high] whose encoded size (from `sizeAt`, -1 on error) is at most
    // `target`, or -1 if none fits. Used by image.compress; exposed for tests.
    int BestQualityUnder(int64_t target, int low, int high, const std::function<int64_t(int)>& sizeAt);
    // How much to scale each side by when even the lowest quality came out `got` bytes for `target`.
    double ShrinkFactor(int64_t target, int64_t got);

    // "0x80070005 Access is denied."
    std::wstring HResultText(HRESULT hr);
}
