#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <string_view>
#include <vector>

// The WIC basics every image area shares: the imaging factory, a frame's EXIF orientation, the WIC
// container of an output format, and a source's pixels pulled into memory. WIC is free-threaded;
// call from any thread with COM initialised. (Not pdf/Wic.h, which is the PDF tools' encoders.)
namespace sd::wic
{
    // A new imaging factory, or null when WIC is unavailable.
    Microsoft::WRL::ComPtr<IWICImagingFactory> Factory();

    // The frame's EXIF orientation, 1..8 (1 when it has none, e.g. a BMP). Cameras store the pixels
    // unrotated and set this tag; metadata is not carried to outputs, so the pixels must be turned.
    // Read through the metadata policy name first (WIC maps it to the container's EXIF or XMP), then
    // the raw tag 274 paths of JPEG and TIFF for codecs that don't implement the policy.
    int ExifOrientation(IWICBitmapFrameDecode* frame);
    // The flip and turn that show the frame upright.
    WICBitmapTransformOptions ExifTransform(IWICBitmapFrameDecode* frame);

    // The WIC container of an output format id ("png", "jpg", "tiff", "bmp"); GUID_NULL for the formats
    // written without a WIC encoder (WebP through libwebp, AVIF and HEIC through the format hooks).
    GUID ContainerFor(std::string_view formatId);

    // Pulls `source` into memory as 32bpp BGRA (converted when it is another format), rows top-down
    // with no padding: this is where a lazy WIC chain actually runs. Refuses an empty picture and one
    // of more than `maxPixels`.
    inline constexpr uint64_t kMaxPixels = 0xFFFFFFFFull / 4;   // 4 GB of pixels, as much as CopyPixels takes
    HRESULT CopyBgra(IWICImagingFactory* factory, IWICBitmapSource* source, uint32_t& width, uint32_t& height, std::vector<uint8_t>& pixels,
                     uint64_t maxPixels = kMaxPixels);
}
