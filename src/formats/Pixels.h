#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// Small pixel helpers the image areas share: a BGRA picture in memory, its round trip to WIC,
// flattening on white, unpremultiplying, and the turns and mirrors that AVIF's irot/imir boxes ask for.
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

    // Image files over 1 GB are refused (FileIo.h ReadFileBytes reads them).
    inline constexpr uint64_t kMaxImageFileBytes = 1ull << 30;

    // WIC bitmap of the picture (WIC copies the pixels, so `image` may go away afterwards).
    HRESULT ToWicBitmap(IWICImagingFactory* factory, const Image& image, Microsoft::WRL::ComPtr<IWICBitmapSource>& out);

    // One colour channel of a straight-alpha pixel composited over white, rounded to nearest.
    constexpr uint8_t OnWhite(unsigned channel, unsigned alpha)
    {
        return static_cast<uint8_t>((channel * alpha + 255u * (255u - alpha) + 127u) / 255u);
    }
    // Straight BGRA to 24bpp BGR composited over white, rows padded to 4 bytes (`stride`), for the
    // formats without alpha (JPEG, BMP, HEIC).
    std::vector<uint8_t> FlattenToBgr(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t& stride);

    // One premultiplied BGRA pixel (lunasvg, Direct2D, a blur's working copy) to straight alpha.
    inline void UnpremultiplyPixel(uint8_t* p)
    {
        const unsigned a = p[3];
        if (a == 0 || a == 255)
            return;
        for (int c = 0; c < 3; ++c)
            p[c] = static_cast<uint8_t>(std::min(255u, (p[c] * 255u + a / 2) / a));
    }
    // Every pixel of a premultiplied BGRA buffer to straight alpha, in place.
    void Unpremultiply(std::vector<uint8_t>& bgra);
    // Turns the picture `quarterTurns` x 90 degrees anti-clockwise (HEIF 'irot').
    void RotateAntiClockwise(Image& image, int quarterTurns);
    // HEIF 'imir': axis 0 swaps top and bottom, axis 1 swaps left and right.
    void Mirror(Image& image, int axis);

    // Worker threads a codec may use for one picture.
    int CodecThreads();
}
