#include "ImageHooks.h"
#include "formats/Avif.h"
#include "formats/Heic.h"
#include "formats/Svg.h"

// The image formats beyond WIC's built-ins (ImageHooks.h): AVIF and SVG through bundled libraries,
// HEIC through Windows' optional codec.
namespace sd
{
    const std::vector<const IImageFormatHook*>& ImageFormatHooks()
    {
        static const std::vector<const IImageFormatHook*> hooks = {
            &formats::AvifHook(),
            &formats::SvgHook(),
            &formats::HeicHook(),
        };
        return hooks;
    }

    const IImageFormatHook* DecodeHookFor(const std::wstring& ext)
    {
        for (const auto* hook : ImageFormatHooks())
            for (const auto& own : hook->DecodedExtensions())
                if (own == ext)
                    return hook;
        return nullptr;
    }

    const IImageFormatHook* EncodeHookFor(std::string_view formatId)
    {
        for (const auto* hook : ImageFormatHooks())
            if (const char* id = hook->EncodedFormat(); id && formatId == id)
                return hook;
        return nullptr;
    }
}
