#pragma once
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// Image formats beyond WIC's built-ins plug into the image pipeline (ImagePipeline.cpp) here: a hook
// decodes some file types itself (AVIF through libavif, SVG through lunasvg), writes one image.encode
// format (AVIF, or HEIC through Windows' optional HEIF codec), or explains a file type that needs a
// Windows codec the PC lacks. The hooks live in src/formats; ImageFormatHooks() lists them.
namespace sd
{
    class IImageFormatHook
    {
    public:
        virtual ~IImageFormatHook() = default;

        // ---- Decoding: source extensions (".avif") this hook reads instead of WIC.
        virtual std::vector<std::wstring> DecodedExtensions() const { return {}; }
        // Decodes the whole picture, upright, to 32bpp BGRA with straight alpha. Called on a worker.
        virtual HRESULT Decode(IWICImagingFactory*, const std::wstring&, std::stop_token,
                               Microsoft::WRL::ComPtr<IWICBitmapSource>&, std::wstring& error) const
        {
            error = L"not a decoder";
            return E_NOTIMPL;
        }
        // The upright size from the file's header, without decoding pixels (the preview card).
        virtual bool ReadSize(const std::wstring&, uint32_t&, uint32_t&) const { return false; }

        // ---- Encoding: the image.encode format id ("avif") this hook writes, or nullptr.
        virtual const char* EncodedFormat() const { return nullptr; }
        // Can it write that format on this PC now? `why` names what to install when it can't.
        // Called on the UI thread while the wheel opens, so it must be quick.
        virtual bool CanEncode(std::wstring* why) const
        {
            (void)why;
            return true;
        }
        // `pixels` is 32bpp BGRA with straight alpha, or 24bpp BGR already flattened on white for a
        // format without alpha (ImageFormatInfo::keepsAlpha); `quality` 1..100.
        virtual HRESULT Encode(IWICImagingFactory*, IWICBitmapSource* pixels, int quality, IStream* out, std::wstring& error) const
        {
            (void)pixels, (void)quality, (void)out;
            error = L"not an encoder";
            return E_NOTIMPL;
        }

        // ---- Files WIC opens only with an optional Windows codec (".heic"). Fills a short label for
        // the wheel's centre and the full reason, and returns true, for extensions it knows.
        virtual bool CodecHint(const std::wstring& lowerExtension, std::wstring* label, std::wstring* why) const
        {
            (void)lowerExtension, (void)label, (void)why;
            return false;
        }
    };

    // Every hook, in src/formats/Formats.cpp. Plain objects that live as long as the program.
    const std::vector<const IImageFormatHook*>& ImageFormatHooks();

    // The hook that decodes this extension, the one that writes this format, or nullptr.
    const IImageFormatHook* DecodeHookFor(const std::wstring& lowerExtension);
    const IImageFormatHook* EncodeHookFor(std::string_view formatId);
}
