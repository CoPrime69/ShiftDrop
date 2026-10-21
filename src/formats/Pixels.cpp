#include "formats/Pixels.h"

#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <thread>

namespace sd::formats
{
    bool Image::Opaque() const
    {
        for (size_t i = 3; i < bgra.size(); i += 4)
            if (bgra[i] != 255)
                return false;
        return true;
    }

    HRESULT ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& bytes)
    {
        wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file)
            return HRESULT_FROM_WIN32(GetLastError());
        LARGE_INTEGER size{};
        RETURN_IF_WIN32_BOOL_FALSE(GetFileSizeEx(file.get(), &size));
        if (size.QuadPart > (1ll << 30))
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        RETURN_IF_WIN32_BOOL_FALSE(ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr));
        bytes.resize(read);
        return S_OK;
    }

    HRESULT ToWicBitmap(IWICImagingFactory* factory, const Image& image, Microsoft::WRL::ComPtr<IWICBitmapSource>& out)
    {
        if (image.Empty())
            return E_INVALIDARG;
        const UINT64 bytes = UINT64{ image.width } * image.height * 4;
        if (bytes > 0xFFFFFFFFull)
            return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        RETURN_IF_FAILED(factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppBGRA, image.width * 4,
                                                         static_cast<UINT>(bytes), const_cast<BYTE*>(image.bgra.data()), &bitmap));
        out = bitmap;
        return S_OK;
    }

    std::vector<uint8_t> FlattenToBgr(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t& stride)
    {
        stride = (width * 3 + 3) & ~3u;
        std::vector<uint8_t> out(static_cast<size_t>(stride) * height);
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* s = bgra + static_cast<size_t>(y) * width * 4;
            uint8_t* d = out.data() + static_cast<size_t>(y) * stride;
            for (uint32_t x = 0; x < width; ++x, s += 4, d += 3)
                for (int c = 0; c < 3; ++c)
                    d[c] = OnWhite(s[c], s[3]);
        }
        return out;
    }

    void Unpremultiply(std::vector<uint8_t>& bgra)
    {
        for (size_t i = 0; i + 3 < bgra.size(); i += 4)
            UnpremultiplyPixel(&bgra[i]);
    }

    void RotateAntiClockwise(Image& image, int quarterTurns)
    {
        quarterTurns = ((quarterTurns % 4) + 4) % 4;
        if (quarterTurns == 0 || image.Empty())
            return;
        const uint32_t w = image.width, h = image.height;
        Image turned;
        const bool swap = quarterTurns != 2;
        turned.width = swap ? h : w;
        turned.height = swap ? w : h;
        turned.bgra.resize(image.bgra.size());
        auto* dst = reinterpret_cast<uint32_t*>(turned.bgra.data());
        const auto* src = reinterpret_cast<const uint32_t*>(image.bgra.data());
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                // Where source pixel (x, y) lands after the turn.
                uint32_t nx = 0, ny = 0;
                switch (quarterTurns)
                {
                case 1: nx = y; ny = w - 1 - x; break;            // 90 anti-clockwise
                case 2: nx = w - 1 - x; ny = h - 1 - y; break;    // 180
                default: nx = h - 1 - y; ny = x; break;           // 270 anti-clockwise = 90 clockwise
                }
                dst[static_cast<size_t>(ny) * turned.width + nx] = src[static_cast<size_t>(y) * w + x];
            }
        image = std::move(turned);
    }

    void Mirror(Image& image, int axis)
    {
        auto* px = reinterpret_cast<uint32_t*>(image.bgra.data());
        const uint32_t w = image.width, h = image.height;
        if (axis == 0)   // top <-> bottom
        {
            for (uint32_t y = 0; y < h / 2; ++y)
                std::swap_ranges(px + static_cast<size_t>(y) * w, px + static_cast<size_t>(y + 1) * w, px + static_cast<size_t>(h - 1 - y) * w);
        }
        else             // left <-> right
        {
            for (uint32_t y = 0; y < h; ++y)
                std::reverse(px + static_cast<size_t>(y) * w, px + static_cast<size_t>(y + 1) * w);
        }
    }

    int CodecThreads()
    {
        // Several files may convert at once on the pool, but most drops are one file: use every core.
        return static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 64u));
    }
}
