#pragma once
#include "RecipeStore.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <gtest/gtest.h>
#include <webp/decode.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>

namespace sd::test
{
    // A fresh folder under %TEMP% per test, removed afterwards.
    class TempDir
    {
    public:
        TempDir()
        {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            m_path = std::filesystem::temp_directory_path() /
                     std::format(L"sdtest-{}-{}", GetCurrentProcessId(), info ? Widen(info->name()) : L"x");
            std::filesystem::remove_all(m_path);
            std::filesystem::create_directories(m_path);
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(m_path, ec);
        }
        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;

        std::wstring operator/(const std::wstring& name) const { return (m_path / name).wstring(); }
        const std::filesystem::path& path() const { return m_path; }

    private:
        static std::wstring Widen(const char* s) { return std::wstring(s, s + strlen(s)); }
        std::filesystem::path m_path;
    };

    inline void WriteText(const std::wstring& path, const std::string& text)
    {
        std::ofstream(std::filesystem::path(path), std::ios::binary) << text;
    }

    inline std::string ReadAll(const std::wstring& path)
    {
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }

    // Writes a width x height 32bpp PNG with a gradient and a transparent left half.
    inline HRESULT WritePng(const std::wstring& path, UINT width, UINT height)
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        std::string pixels(static_cast<size_t>(width) * height * 4, '\0');
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
            {
                auto* p = reinterpret_cast<BYTE*>(&pixels[(static_cast<size_t>(y) * width + x) * 4]);
                p[0] = static_cast<BYTE>(x * 255 / width);
                p[1] = static_cast<BYTE>(y * 255 / height);
                p[2] = 128;
                p[3] = x < width / 2 ? 0 : 255;
            }
        if (SUCCEEDED(hr)) hr = frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()), reinterpret_cast<BYTE*>(pixels.data()));
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        return hr;
    }

    // Size of an image: WebP through libwebp (not every Windows has a WebP decoder for WIC), the rest
    // as WIC decodes it.
    inline bool ImageSize(const std::wstring& path, UINT& width, UINT& height)
    {
        using Microsoft::WRL::ComPtr;
        if (std::filesystem::path(path).extension() == L".webp")
        {
            const auto bytes = ReadAll(path);
            int w = 0, h = 0;
            if (!WebPGetInfo(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &w, &h))
                return false;
            width = static_cast<UINT>(w);
            height = static_cast<UINT>(h);
            return true;
        }
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
               SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
               SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height));
    }

    // Built-in recipes plus the seeded example, loaded from a throwaway recipes folder.
    struct Recipes
    {
        explicit Recipes(const TempDir& dir) { store.Load(registry, dir / L"recipes"); }
        const Recipe& Get(std::string_view id) const
        {
            const auto* r = store.Find(id);
            EXPECT_NE(r, nullptr) << std::string(id);
            return *r;
        }
        ActionRegistry registry;
        RecipeStore store;
    };

    inline size_t CountFiles(const TempDir& dir, const std::wstring& extension)
    {
        size_t n = 0;
        for (const auto& e : std::filesystem::directory_iterator(dir.path()))
            if (e.path().extension() == extension)
                ++n;
        return n;
    }
}
