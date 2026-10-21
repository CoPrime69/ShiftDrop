#pragma once
#include "Ffmpeg.h"
#include "OutputPath.h"
#include "PathUtil.h"
#include "RecipeStore.h"
#include "Utf.h"
#include "Wic.h"
#include "formats/Pixels.h"
#include "sandbox/Sandbox.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <gtest/gtest.h>
#include <webp/decode.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
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
        std::filesystem::path m_path;
    };

    // Sets the output naming style for one test and puts the default back afterwards.
    struct NamingScope
    {
        explicit NamingScope(OutputNaming naming) { SetOutputNaming(naming); }
        ~NamingScope() { SetOutputNaming(OutputNaming::Parentheses); }
        NamingScope(const NamingScope&) = delete;
        NamingScope& operator=(const NamingScope&) = delete;
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

    // Writes a width x height 32bpp PNG with a gradient and a transparent left half. `noise` adds
    // per-pixel noise (opaque), so lossy encodes come out large, like a real photo.
    inline HRESULT WritePng(const std::wstring& path, UINT width, UINT height, bool noise = false)
    {
        using Microsoft::WRL::ComPtr;
        const auto factory = wic::Factory();
        HRESULT hr = factory ? S_OK : REGDB_E_CLASSNOTREG;
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
                if (noise)
                {
                    UINT n = x * 0x9E3779B9u ^ y * 0x85EBCA6Bu;   // repeatable hash (murmur3 finaliser)
                    n ^= n >> 16;
                    n *= 0x85EBCA6Bu;
                    n ^= n >> 13;
                    n *= 0xC2B2AE35u;
                    n ^= n >> 16;
                    p[0] = static_cast<BYTE>(p[0] / 2 + (n >> 24) / 2);
                    p[1] = static_cast<BYTE>(p[1] / 2 + ((n >> 16) & 0xFF) / 2);
                    p[2] = static_cast<BYTE>((n >> 8) & 0xFF);
                    p[3] = 255;
                }
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
        const auto factory = wic::Factory();
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        return factory &&
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

    // The real ffmpeg when CI (or a developer) points SHIFTDROP_FFMPEG at ffmpeg.exe; the tests that
    // need it check Available() and are skipped otherwise. One engine for the whole run.
    inline std::shared_ptr<const FfmpegEngine> TestFfmpeg()
    {
        static const auto engine = std::make_shared<const FfmpegEngine>(FfmpegEngine::Locate());
        return engine;
    }

    // Archives are read by the sandboxed worker; in the tests the worker is this exe (TestMain.cpp
    // hands "--sd-worker" runs to sandbox::WorkerMain).
    inline void UseTestWorker()
    {
        sandbox::SetWorkerPath(ThisExe());
    }

    // Any file WIC reads, as straight BGRA.
    inline bool ReadPixels(const std::wstring& path, formats::Image& out)
    {
        using Microsoft::WRL::ComPtr;
        const auto factory = wic::Factory();
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        return factory &&
               SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
               SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic::CopyBgra(factory.Get(), frame.Get(), out.width, out.height, out.bgra));
    }

    // A w x h PNG (WritePng's gradient) as bytes; the file it was written to is deleted.
    inline std::string PngBytes(const TempDir& dir, UINT w = 64, UINT h = 48)
    {
        const auto path = dir / L"made.png";
        EXPECT_HRESULT_SUCCEEDED(WritePng(path, w, h));
        auto bytes = ReadAll(path);
        DeleteFileW(path.c_str());
        return bytes;
    }

    inline size_t CountFiles(const TempDir& dir, const std::wstring& extension)
    {
        size_t n = 0;
        for (const auto& e : std::filesystem::directory_iterator(dir.path()))
            if (e.path().extension() == extension)
                ++n;
        return n;
    }
}
