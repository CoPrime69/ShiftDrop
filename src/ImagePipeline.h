#pragma once
#include "Action.h"
#include "Recipe.h"

#include <windows.h>
#include <objidl.h>

#include <cstdint>
#include <functional>
#include <set>
#include <stop_token>
#include <wincodec.h>
#include <string>

namespace sd
{
    // What this PC can decode and encode. Built once at startup: WIC decoders depend on optional
    // Store codecs (HEIC, ...), so the list is read at runtime. WebP (libwebp), AVIF (libavif) and SVG
    // (lunasvg) are always available, whatever Windows has installed (ImageHooks.h).
    class ImageCodecs
    {
    public:
        ImageCodecs();   // needs COM initialised on the calling thread
        bool CanDecode(const std::wstring& lowerExtension) const;
        // An image type the image recipes are meant for, even if this PC can't open it yet (HEIC
        // without Windows' HEIF codec): the wheel then shows its slices dimmed, with the reason.
        bool IsImage(const std::wstring& lowerExtension) const;
        // Why CanDecode is false: what to install, or "no image decoder for '.xyz' on this PC".
        std::wstring WhyNotDecodable(const std::wstring& lowerExtension) const;
        // A short line for the wheel's centre when no slice can take such a file ("Needs HEIF
        // Image Extensions"), or empty.
        std::wstring MissingCodecLabel(const std::wstring& lowerExtension) const;
        bool CanEncode(const ImageFormatInfo& format, std::wstring* why = nullptr) const;
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

    // Encodes one picture (any WIC pixel format) as `format` at `quality`, the way the last step of an
    // image recipe does: flattened on white for formats without alpha, through libwebp or a format
    // hook where WIC has no encoder. Used for each frame of a multi-frame conversion (src/formats).
    HRESULT EncodePicture(IWICBitmapSource* pixels, const ImageFormatInfo& format, int quality, IStream* out, std::wstring& error);

    // The highest quality in [low, high] whose encoded size (from `sizeAt`, -1 on error) is at most
    // `target`, or -1 if none fits. Used by image.compress; exposed for tests.
    int BestQualityUnder(int64_t target, int low, int high, const std::function<int64_t(int)>& sizeAt);
    // How much to scale each side by when even the lowest quality came out `got` bytes for `target`.
    double ShrinkFactor(int64_t target, int64_t got);

    // "0x80070005 Access is denied."
    std::wstring HResultText(HRESULT hr);
}
