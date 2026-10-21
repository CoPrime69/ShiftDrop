#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

// Small pixel helpers the format hooks share: a BGRA picture in memory, its round trip to WIC, and
// the turns and mirrors that AVIF's irot/imir boxes ask for.
namespace sd::formats
{
    // 32bpp BGRA, straight (not premultiplied) alpha, rows top-down with no padding.
    struct Image
    {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> bgra;

        bool Empty() const { return width == 0 || height == 0; }
        uint8_t* Row(uint32_t y) { return bgra.data() + static_cast<size_t>(y) * width * 4; }
        const uint8_t* Row(uint32_t y) const { return bgra.data() + static_cast<size_t>(y) * width * 4; }
        bool Opaque() const;   // every alpha is 255
    };

    // The whole file in memory (Unicode path; images over 1 GB are refused).
    HRESULT ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& bytes);

    // WIC bitmap of the picture (WIC copies the pixels, so `image` may go away afterwards).
    HRESULT ToWicBitmap(IWICImagingFactory* factory, const Image& image, Microsoft::WRL::ComPtr<IWICBitmapSource>& out);
    // Pulls a source's pixels into memory as BGRA (this is where a lazy WIC chain runs).
    HRESULT FromWicSource(IWICImagingFactory* factory, IWICBitmapSource* source, Image& out);

    // Premultiplied BGRA (lunasvg, Direct2D) to straight alpha, in place.
    void Unpremultiply(Image& image);
    // Turns the picture `quarterTurns` x 90 degrees anti-clockwise (HEIF 'irot').
    void RotateAntiClockwise(Image& image, int quarterTurns);
    // HEIF 'imir': axis 0 swaps top and bottom, axis 1 swaps left and right.
    void Mirror(Image& image, int axis);

    // Worker threads a codec may use for one picture.
    int CodecThreads();
}
