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
        D2D1_COLOR_F Rgba(UINT rgb, float a) { return D2D1::ColorF(rgb, a); }

        ThemeChoice g_choice = ThemeChoice::System;

        bool AppsUseLightTheme()
        {
            DWORD value = 0, size = sizeof(value);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                             L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
                return false;
            return value != 0;
        }

        D2D1_COLOR_F Rgb(uint32_t rgb) { return D2D1::ColorF(rgb, 1.f); }

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

    Palette Current(Style style)
    {
        Palette p{};
        p.dark = g_choice == ThemeChoice::System ? !AppsUseLightTheme() : g_choice == ThemeChoice::Dark;
        // The accent's dark tone with dark text in dark mode; in light mode the colour itself, with
        // whichever text reads on it. The highlight toward the slice's rim stays readable too.
        const auto tones = accent::Current();
        const uint32_t fill = p.dark ? tones.dark : tones.fill, onFill = p.dark ? accent::kTextOnDark : accent::TextOn(tones.fill);
        p.accent = Rgb(fill);
        p.accentHi = Rgb(accent::Lighter(fill, onFill, p.dark ? 0.28 : 0.20));
        p.textOnAccent = Rgb(onFill);
        p.ok = Rgba(0x6CCB5F, 1.f);
        p.warn = Rgba(0xFCE100, 1.f);
        p.error = Rgba(0xFF99A4, 1.f);
        const bool glass = style == Style::Glass;
        if (p.dark)
        {
            p.text = Rgba(0xF7F7F8, 1.f);
            p.textDim = Rgba(0xF7F7F8, 0.32f);
            p.shadow = Rgba(0x000000, 0.42f);
            p.tile = glass ? Rgba(0xFFFFFF, 0.07f) : Rgba(0x3B3B3F, 0.97f);
            p.tileLine = Rgba(0xFFFFFF, glass ? 0.10f : 0.13f);
            p.centre = glass ? Rgba(0x000000, 0.25f) : Rgba(0x232326, 0.98f);
            p.centreLine = Rgba(0xFFFFFF, 0.10f);
            p.glassTint = Rgba(0x1E1E22, 0.66f);
        }
        else
        {
            p.text = Rgba(0x1A1A1E, 1.f);
            p.textDim = Rgba(0x1A1A1E, 0.32f);
            p.shadow = Rgba(0x000000, 0.20f);
            p.tile = glass ? Rgba(0xFFFFFF, 0.42f) : Rgba(0xF4F4F6, 0.98f);
            p.tileLine = Rgba(0x000000, glass ? 0.07f : 0.10f);
            p.centre = glass ? Rgba(0xFFFFFF, 0.55f) : Rgba(0xFFFFFF, 0.99f);
            p.centreLine = Rgba(0x000000, 0.08f);
            p.glassTint = Rgba(0xF6F6F8, 0.62f);
        }
        // The top panel is a dark island in both themes (Solid), like a notch; Glass follows the theme.
        if (glass)
        {
            p.panelAccent = p.accent;
            p.panelTextOnAccent = p.textOnAccent;
            p.panel = p.glassTint;
            p.panelLine = p.dark ? Rgba(0xFFFFFF, 0.16f) : Rgba(0x000000, 0.10f);
            p.panelText = p.text;
            p.panelTextDim = Mix(p.text, D2D1::ColorF(0, 0, 0, 0), 0.4f);
            p.keycap = p.dark ? Rgba(0xFFFFFF, 0.12f) : Rgba(0x000000, 0.07f);
        }
        else
        {
            p.panelAccent = Rgb(tones.dark);
            p.panelTextOnAccent = Rgb(accent::kTextOnDark);
            p.panel = Rgba(0x050506, 0.97f);
            p.panelLine = Rgba(0xFFFFFF, 0.08f);
            p.panelText = Rgba(0xF7F7F8, 1.f);
            p.panelTextDim = Rgba(0xF7F7F8, 0.58f);
            p.keycap = Rgba(0xFFFFFF, 0.13f);
        }
        if (!p.dark && !glass)
        {
            p.ok = Rgba(0x6CCB5F, 1.f);
            p.error = Rgba(0xFF99A4, 1.f);
        }
        return p;
    }

    EditorPalette EditorColors()
    {
        const auto t = Current(Style::Solid);
        const auto tones = accent::Current();
        EditorPalette c{};
        c.dark = t.dark;
        // Buttons, switches and accent text: the dark tone with dark text on it in dark mode, the light
        // tone with white text in light mode; hover a little lighter while its text still reads.
        const uint32_t accentRgb = c.dark ? tones.dark : tones.light, onAccent = c.dark ? accent::kTextOnDark : accent::kTextOnLight;
        c.accent = Rgb(accentRgb);
        c.accentHover = Rgb(accent::Lighter(accentRgb, onAccent, c.dark ? 0.34 : 0.12));
        c.onAccent = Rgb(onAccent);
        if (c.dark)
        {
            c.window = Rgba(0x202023, 1.f);
            c.panel = Rgba(0x28282C, 1.f);
            c.stage = Rgba(0x141416, 1.f);
            c.track = Rgba(0x18181B, 1.f);
            c.line = Rgba(0xFFFFFF, 0.08f);
            c.text = Rgba(0xF3F3F5, 1.f);
            c.textDim = Rgba(0xF3F3F5, 0.66f);
            c.textFaint = Rgba(0xF3F3F5, 0.38f);
            c.control = Rgba(0x36363C, 1.f);
            c.controlHover = Rgba(0x404047, 1.f);
            c.controlDown = Rgba(0x2E2E33, 1.f);
            c.controlLine = Rgba(0xFFFFFF, 0.07f);
            c.danger = Rgba(0xFF7A7A, 1.f);
            c.shadow = Rgba(0x000000, 0.5f);
        }
        else
        {
            c.window = Rgba(0xF3F3F5, 1.f);
            c.panel = Rgba(0xFBFBFC, 1.f);
            c.stage = Rgba(0xE2E2E7, 1.f);
            c.track = Rgba(0xE9E9ED, 1.f);
            c.line = Rgba(0x000000, 0.09f);
            c.text = Rgba(0x1A1A1E, 1.f);
            c.textDim = Rgba(0x1A1A1E, 0.66f);
            c.textFaint = Rgba(0x1A1A1E, 0.40f);
            c.control = Rgba(0xFFFFFF, 1.f);
            c.controlHover = Rgba(0xF5F5F7, 1.f);
            c.controlDown = Rgba(0xEBEBEF, 1.f);
            c.controlLine = Rgba(0x000000, 0.12f);
            c.danger = Rgba(0xC42B1C, 1.f);
            c.shadow = Rgba(0x000000, 0.18f);
        }
        c.accentSoft = WithAlpha(c.accent, c.dark ? 0.22f : 0.16f);
        c.dangerSoft = WithAlpha(c.danger, 0.24f);
        c.focus = c.text;
        return c;
    }

    COLORREF ToColorRef(const D2D1_COLOR_F& c)
    {
        return RGB(static_cast<int>(c.r * 255), static_cast<int>(c.g * 255), static_cast<int>(c.b * 255));
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
        DialogColors c;
        c.dark = Current(Style::Solid).dark;
        c.background = c.dark ? RGB(32, 32, 32) : GetSysColor(lightBackground);
        c.text = c.dark ? RGB(240, 240, 240) : GetSysColor(COLOR_WINDOWTEXT);
        c.field = c.dark ? RGB(45, 45, 45) : GetSysColor(COLOR_WINDOW);
        return c;
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

    D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t)
    {
        return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
    }

    D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& c, float alphaFactor)
    {
        return D2D1::ColorF(c.r, c.g, c.b, c.a * alphaFactor);
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
