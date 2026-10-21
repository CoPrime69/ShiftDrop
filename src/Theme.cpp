#include "Theme.h"
#include "Accent.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <shobjidl.h>
#include <wil/resource.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

namespace sd::theme
{
    namespace
    {
        ThemeChoice g_choice = ThemeChoice::System;

        bool AppsUseLightTheme()
        {
            DWORD value = 0, size = sizeof(value);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                             L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
                return false;
            return value != 0;
        }

        bool HasFamily(IDWriteFactory* dwrite, const wchar_t* family)
        {
            ComPtr<IDWriteFontCollection> fonts;
            UINT32 index = 0;
            BOOL exists = FALSE;
            return dwrite && SUCCEEDED(dwrite->GetSystemFontCollection(&fonts)) &&
                   SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
        }

        // Separable box blur on premultiplied BGRA; three passes approximate a Gaussian.
        void BoxBlur(std::vector<uint32_t>& px, UINT w, UINT h, int radius)
        {
            std::vector<uint32_t> tmp(px.size());
            const auto pass = [&](const std::vector<uint32_t>& src, std::vector<uint32_t>& dst, bool horizontal) {
                const int len = static_cast<int>(horizontal ? w : h), lines = static_cast<int>(horizontal ? h : w);
                const auto index = [&](int line, int i) {
                    return horizontal ? static_cast<size_t>(line) * w + static_cast<size_t>(i)
                                      : static_cast<size_t>(i) * w + static_cast<size_t>(line);
                };
                for (int line = 0; line < lines; ++line)
                    for (int i = 0; i < len; ++i)
                    {
                        uint32_t sum[4]{};
                        for (int k = -radius; k <= radius; ++k)
                        {
                            const uint32_t p = src[index(line, std::clamp(i + k, 0, len - 1))];
                            for (int c = 0; c < 4; ++c)
                                sum[c] += (p >> (8 * c)) & 0xFF;
                        }
                        uint32_t out = 0;
                        for (int c = 0; c < 4; ++c)
                            out |= (sum[c] / static_cast<uint32_t>(2 * radius + 1)) << (8 * c);
                        dst[index(line, i)] = out;
                    }
            };
            for (int i = 0; i < 3; ++i)
            {
                pass(px, tmp, true);
                pass(tmp, px, false);
            }
        }
    }

    void SetChoice(ThemeChoice choice)
    {
        g_choice = choice;
    }

    bool IsDark()
    {
        return g_choice == ThemeChoice::System ? !AppsUseLightTheme() : g_choice == ThemeChoice::Dark;
    }

    Palette Current(Style style)
    {
        return MakePalette(style, IsDark(), accent::Current());
    }

    EditorPalette EditorColors()
    {
        return MakeEditorColors(IsDark(), accent::Current());
    }

    void ApplyDarkCaption(HWND hwnd, bool dark, COLORREF caption)
    {
        const BOOL on = dark;
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof(on));
        if (caption != CLR_INVALID)
            DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR, Windows 11 */, &caption, sizeof(caption));
    }

    float TextWidth(IDWriteFactory* dwrite, std::wstring_view text, IDWriteTextFormat* format, IDWriteTypography* typography)
    {
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        if (text.empty() || !dwrite || !format ||
            FAILED(dwrite->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, 10000.f, 100.f, &layout)))
            return 0.f;
        if (typography)
            layout->SetTypography(typography, { 0, static_cast<UINT32>(text.size()) });
        if (FAILED(layout->GetMetrics(&metrics)))
            return 0.f;
        return metrics.widthIncludingTrailingWhitespace;
    }

    DialogColors Dialog(int lightBackground)
    {
        return MakeDialog(IsDark(), accent::Current(), lightBackground);
    }

    float ScaleFor(HMONITOR monitor, WheelSize size)
    {
        UINT dpiX = 96, dpiY = 96;
        GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(monitor, &mi);
        return SizeScale(size, static_cast<float>(dpiX) / 96.f, static_cast<float>(mi.rcMonitor.bottom - mi.rcMonitor.top));
    }

    bool AnimationsOn()
    {
        BOOL on = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
        return on != FALSE;
    }

    Pixels CaptureBlurred(const RECT& rect, int step)
    {
        Pixels out;
        const int w = rect.right - rect.left, h = rect.bottom - rect.top;
        if (w <= 0 || h <= 0 || step <= 0)
            return out;
        const UINT sw = static_cast<UINT>((w + step - 1) / step), sh = static_cast<UINT>((h + step - 1) / step);
        auto screen = wil::GetDC(nullptr);
        wil::unique_hdc mem(CreateCompatibleDC(screen.get()));
        if (!screen || !mem)
            return out;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = static_cast<LONG>(sw);
        bi.bmiHeader.biHeight = -static_cast<LONG>(sh);   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        wil::unique_hbitmap dib(CreateDIBSection(mem.get(), &bi, DIB_RGB_COLORS, &bits, nullptr, 0));
        if (!dib)
            return out;
        {
            auto select = wil::SelectObject(mem.get(), dib.get());
            // HALFTONE averages while shrinking. The screen DC is in physical pixels (the process is
            // per-monitor-v2 aware). Without CAPTUREBLT, layered windows such as Explorer's drag image
            // are left out, which is what we want.
            SetStretchBltMode(mem.get(), HALFTONE);
            SetBrushOrgEx(mem.get(), 0, 0, nullptr);
            if (!StretchBlt(mem.get(), 0, 0, static_cast<int>(sw), static_cast<int>(sh), screen.get(), rect.left, rect.top, w, h, SRCCOPY))
                return out;
        }
        GdiFlush();
        const auto* begin = static_cast<const uint32_t*>(bits);
        out.px.assign(begin, begin + static_cast<size_t>(sw) * sh);
        for (auto& p : out.px)
            p |= 0xFF000000;   // GDI leaves alpha undefined; the snapshot is opaque
        BoxBlur(out.px, sw, sh, 2);
        out.w = sw;
        out.h = sh;
        return out;
    }

    ComPtr<ID2D1Bitmap> ToBitmap(ID2D1RenderTarget* rt, const Pixels& pixels)
    {
        ComPtr<ID2D1Bitmap> bitmap;
        if (!rt || pixels.px.empty())
            return bitmap;
        const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        rt->CreateBitmap(D2D1::SizeU(pixels.w, pixels.h), pixels.px.data(), pixels.w * 4, &props, &bitmap);
        return bitmap;
    }

    Pixels ShellThumbnail(const std::wstring& path, UINT px)
    {
        Pixels out;
        ComPtr<IShellItemImageFactory> factory;
        HBITMAP raw = nullptr;
        const SIZE size{ static_cast<LONG>(px), static_cast<LONG>(px) };
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->GetImage(size, SIIGBF_RESIZETOFIT, &raw)) || !raw)
            return out;
        wil::unique_hbitmap bitmap(raw);
        BITMAP info{};
        if (!GetObjectW(bitmap.get(), sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight == 0)
            return out;
        const UINT w = static_cast<UINT>(info.bmWidth), h = static_cast<UINT>(std::abs(info.bmHeight));
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = static_cast<LONG>(w);
        bi.bmiHeader.biHeight = -static_cast<LONG>(h);   // top-down, whatever the source is
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        out.px.resize(static_cast<size_t>(w) * h);
        wil::unique_hdc dc(CreateCompatibleDC(nullptr));
        if (!dc || GetDIBits(dc.get(), bitmap.get(), 0, h, out.px.data(), &bi, DIB_RGB_COLORS) != static_cast<int>(h))
            return Pixels{};
        // Photos come back with alpha 0 everywhere (no alpha at all): make them opaque. Pictures with
        // alpha may or may not be premultiplied: a colour above its alpha means straight alpha, which
        // is premultiplied here, so Direct2D always gets premultiplied pixels.
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
        out.w = w;
        out.h = h;
        return out;
    }

    std::wstring TextFamily(IDWriteFactory* dwrite)
    {
        return HasFamily(dwrite, L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    }

    std::wstring IconFamily(IDWriteFactory* dwrite)
    {
        return HasFamily(dwrite, L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }

    float EaseOutCubic(float t)
    {
        t = 1.f - std::clamp(t, 0.f, 1.f);
        return 1.f - t * t * t;
    }

    float EaseInOutCubic(float t)
    {
        t = std::clamp(t, 0.f, 1.f);
        return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) / 2.f;
    }

    float Spring(float t)
    {
        t = std::clamp(t, 0.f, 1.f);
        if (t >= 1.f)
            return 1.f;
        // Damped oscillation, normalised so it starts at 0 and has settled by t = 1.
        return 1.f - std::exp(-6.f * t) * std::cos(7.5f * t);
    }
}
