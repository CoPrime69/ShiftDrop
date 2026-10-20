#pragma once
#include "TestUtil.h"
#include "Utf.h"
#include "pdf/Bitmap.h"
#include "pdf/PdfWriter.h"

#include <objbase.h>

#include <format>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// Shared by PdfTests.cpp and OcrTests.cpp.
namespace sd::test
{
    // Runs `fn` on a fresh MTA thread and waits: WinRT calls block, which an STA (the tests' main
    // thread, like the app's UI thread) must not do.
    inline void RunMta(const std::function<void()>& fn)
    {
        std::thread([&] {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            fn();
            if (SUCCEEDED(hr))
                CoUninitialize();
        }).join();
    }

    // A bitmap with a soft gradient and a few dark bars, so encoders have something to chew on.
    inline pdf::Bitmap Pattern(uint32_t w, uint32_t h, bool noise = false)
    {
        pdf::Bitmap b;
        b.width = w;
        b.height = h;
        b.bgra.resize(static_cast<size_t>(w) * h * 4);
        uint32_t seed = 12345;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                uint8_t* p = &b.bgra[(static_cast<size_t>(y) * w + x) * 4];
                seed = seed * 1664525u + 1013904223u;
                const uint8_t n = noise ? static_cast<uint8_t>(seed >> 24) : 0;
                p[0] = static_cast<uint8_t>((x * 255 / w) ^ n);
                p[1] = static_cast<uint8_t>((y * 255 / h) ^ n);
                p[2] = static_cast<uint8_t>(((x / 16) % 2 ? 40 : 200) ^ n);
                p[3] = 255;
            }
        return b;
    }

    // A text PDF written by hand: each entry is one page's content stream, drawn with Helvetica
    // (WinAnsiEncoding) as /F1. Optional /Rotate per page.
    inline void WriteTextPdf(const std::wstring& path, const std::vector<std::string>& contents, const std::vector<int>& rotate = {})
    {
        pdf::PdfWriter w(path);
        const int catalog = w.NewObject(), pages = w.NewObject(), font = w.NewObject();
        w.Object(font, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
        std::string kids;
        for (size_t i = 0; i < contents.size(); ++i)
        {
            const int c = w.NewObject(), page = w.NewObject();
            w.Stream(c, "", contents[i]);
            const std::string rot = i < rotate.size() && rotate[i] ? std::format(" /Rotate {}", rotate[i]) : "";
            w.Object(page, std::format("<< /Type /Page /Parent {} 0 R /MediaBox [0 0 612 792]{} /Resources << /Font << /F1 {} 0 R >> >> "
                                       "/Contents {} 0 R >>",
                                       pages, rot, font, c));
            kids += std::format(" {} 0 R", page);
        }
        w.Object(pages, std::format("<< /Type /Pages /Kids [{} ] /Count {} >>", kids, contents.size()));
        w.Object(catalog, std::format("<< /Type /Catalog /Pages {} 0 R >>", pages));
        w.Finish(catalog, 0);
    }
}
