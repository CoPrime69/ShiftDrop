#pragma once
#include "ImageHooks.h"
#include "formats/Pixels.h"

#include <cstdint>
#include <string>
#include <string_view>

// SVG input through lunasvg (MIT): drawn once into a transparent BGRA picture, which then goes
// through the normal image converters, so SVG becomes PNG, JPG, WebP, AVIF, PDF and the rest.
namespace sd::formats
{
    inline constexpr uint32_t kSvgDefaultLongSide = 2048;   // for an SVG that states no size
    inline constexpr uint32_t kSvgMaxSide = 16384;          // a stated size larger than this is scaled down

    struct SvgSize
    {
        uint32_t width = 0, height = 0;
    };

    // The pixel size an SVG is drawn at. With its own size (an absolute width and/or height on the
    // root element; lunasvg resolves units and fills the other side from the viewBox), that size.
    // Without one, kSvgDefaultLongSide on the long side, in the shape of the viewBox (`intrinsic*`),
    // else of the drawing (`box*`), else a square.
    SvgSize SvgRasterSize(bool ownSize, float intrinsicWidth, float intrinsicHeight, float boxWidth, float boxHeight);
    // Does this width/height attribute value give a size ("200", "10cm", "48px"), not "100%" or nothing?
    bool IsAbsoluteLength(std::string_view value);

    HRESULT RenderSvg(const char* data, size_t size, Image& out, std::wstring& error);
    bool ReadSvgSize(const char* data, size_t size, uint32_t& width, uint32_t& height);

    const IImageFormatHook& SvgHook();
}
