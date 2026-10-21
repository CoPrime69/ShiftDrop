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

    HRESULT FromWicSource(IWICImagingFactory* factory, IWICBitmapSource* source, Image& out)
    {
        Microsoft::WRL::ComPtr<IWICBitmapSource> bgra = source;
        WICPixelFormatGUID format{};
        RETURN_IF_FAILED(source->GetPixelFormat(&format));
        if (format != GUID_WICPixelFormat32bppBGRA)
        {
            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            RETURN_IF_FAILED(factory->CreateFormatConverter(&converter));
            RETURN_IF_FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                                   WICBitmapPaletteTypeCustom));
            bgra = converter;
        }
        UINT w = 0, h = 0;
        RETURN_IF_FAILED(bgra->GetSize(&w, &h));
        const UINT64 bytes = UINT64{ w } * h * 4;
        if (w == 0 || h == 0)
            return E_INVALIDARG;
        if (bytes > 0xFFFFFFFFull)
            return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        out.width = w;
        out.height = h;
        out.bgra.resize(static_cast<size_t>(bytes));
        return bgra->CopyPixels(nullptr, w * 4, static_cast<UINT>(bytes), out.bgra.data());
    }

    void Unpremultiply(Image& image)
    {
        for (size_t i = 0; i + 3 < image.bgra.size(); i += 4)
        {
            uint8_t* p = &image.bgra[i];
            const unsigned a = p[3];
            if (a == 0 || a == 255)
                continue;
            for (int c = 0; c < 3; ++c)
                p[c] = static_cast<uint8_t>(std::min(255u, (p[c] * 255u + a / 2) / a));
        }
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
