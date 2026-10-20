#include "Thumbnail.h"

#include <shobjidl.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <cstdlib>

namespace sd
{
    theme::Pixels LoadThumbnail(const std::wstring& path, int px)
    {
        Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))))
            return {};
        // BIGGERSIZEOK: a cached thumbnail larger than asked beats extracting a new one; Direct2D
        // scales it down when drawing.
        wil::unique_hbitmap bitmap;
        if (FAILED(factory->GetImage(SIZE{ px, px }, SIIGBF_BIGGERSIZEOK, bitmap.put())))
            return {};
        BITMAP info{};
        if (!GetObjectW(bitmap.get(), sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight == 0)
            return {};

        theme::Pixels out;
        out.w = static_cast<UINT>(info.bmWidth);
        out.h = static_cast<UINT>(std::abs(info.bmHeight));
        out.px.resize(static_cast<size_t>(out.w) * out.h);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = info.bmWidth;
        bi.bmiHeader.biHeight = -static_cast<LONG>(out.h);   // negative: top-down rows
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        auto screen = wil::GetDC(nullptr);
        if (GetDIBits(screen.get(), bitmap.get(), 0, out.h, out.px.data(), &bi, DIB_RGB_COLORS) != static_cast<int>(out.h))
            return {};

        // Photos often come back with alpha 0 everywhere (the shell treats them as opaque); icons carry
        // real alpha, usually premultiplied, but not always: a colour above its alpha means straight.
        bool anyAlpha = false, straight = false;
        for (const uint32_t p : out.px)
        {
            const uint32_t a = p >> 24;
            anyAlpha |= a != 0;
            straight |= ((p >> 16) & 0xFF) > a || ((p >> 8) & 0xFF) > a || (p & 0xFF) > a;
        }
        for (auto& p : out.px)
        {
            if (!anyAlpha)
                p |= 0xFF000000u;
            else if (straight)
            {
                const uint32_t a = p >> 24;
                const auto mul = [a](uint32_t c) { return (c * a + 127) / 255; };
                p = (a << 24) | (mul((p >> 16) & 0xFF) << 16) | (mul((p >> 8) & 0xFF) << 8) | mul(p & 0xFF);
            }
        }
        return out;
    }
}
