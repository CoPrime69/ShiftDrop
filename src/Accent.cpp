#include "Accent.h"
#include "Log.h"
#include "Settings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>

namespace sd::accent
{
    namespace
    {
        // settings.ini names, in the order of Choice.
        constexpr const wchar_t* kIds[] = { L"tide", L"blue", L"purple", L"pink", L"red", L"orange", L"green", L"graphite", L"custom", L"windows" };
        constexpr const wchar_t* kNames[] = { L"Tide", L"Blue", L"Purple", L"Pink", L"Red", L"Orange", L"Green", L"Graphite",
                                              L"Custom colour", L"Windows accent colour" };
        static_assert(ARRAYSIZE(kIds) == static_cast<size_t>(Choice::Windows) + 1 && ARRAYSIZE(kNames) == ARRAYSIZE(kIds));

        // Fill, on dark, on light; each checked by Readable() in the unit tests. The light tones are the
        // brand kit's swatches (App Windows › Accent colour), the dark ones its tones on dark.
        constexpr Variants kPresets[kPresetCount] = {
            { 0x12A58E, 0x2DD4B4, 0x0B7A69 },   // Tide, ShiftDrop's teal: Tide 500, 300 and 700
            { 0x3B82F6, 0x60A5FA, 0x0067C0 },   // Blue
            { 0x7C4DFF, 0xB794F4, 0x7B3FC4 },   // Purple
            { 0xE64B9A, 0xF79AC8, 0xB4246F },   // Pink
            { 0xE8504F, 0xFF8F8F, 0xC42B2B },   // Red
            { 0xF2802B, 0xFB923C, 0xB85000 },   // Orange
            { 0x2FA84F, 0x5CD67E, 0x1F7A3A },   // Green
            { 0x6B7280, 0xB4B9C3, 0x4A4F5A },   // Graphite
        };

        Setting g_setting;

        double Luminance(uint32_t rgb)
        {
            const auto channel = [](uint32_t c) {
                const double v = static_cast<double>(c & 0xFF) / 255.0;
                return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * channel(rgb >> 16) + 0.7152 * channel(rgb >> 8) + 0.0722 * channel(rgb);
        }

        // The first colour from `from` toward `to`, in steps of 2%, that `ok` accepts; `to` if none does.
        template <typename Ok>
        uint32_t Toward(uint32_t from, uint32_t to, Ok ok)
        {
            constexpr int kSteps = 50;
            for (int i = 0; i < kSteps; ++i)
                if (const uint32_t c = Mix(from, to, static_cast<double>(i) / kSteps); ok(c))
                    return c;
            return to;
        }

        bool DarkReads(uint32_t c) { return Contrast(c, kTextOnDark) >= kMinContrast && Contrast(c, kDarkSurface) >= kMinContrast; }
        bool LightReads(uint32_t c) { return Contrast(c, kTextOnLight) >= kMinContrast && Contrast(c, kLightSurface) >= kMinContrast; }
        bool FillReads(uint32_t c) { return Contrast(c, TextOn(c)) >= kMinContrast; }
    }

    const wchar_t* Name(Choice choice)
    {
        return kNames[std::clamp(static_cast<int>(choice), 0, static_cast<int>(Choice::Windows))];
    }

    std::wstring ToHex(uint32_t rgb)
    {
        return std::format(L"#{:06X}", rgb & 0xFFFFFF);
    }

    bool ParseHex(std::wstring_view text, uint32_t& rgb)
    {
        if (!text.empty() && text.front() == L'#')
            text.remove_prefix(1);
        if (text.size() != 6)
            return false;
        uint32_t value = 0;
        for (const wchar_t ch : text)
        {
            if (!std::iswxdigit(ch))
                return false;
            value = value * 16 + static_cast<uint32_t>(std::iswdigit(ch) ? ch - L'0' : std::towlower(ch) - L'a' + 10);
        }
        rgb = value;
        return true;
    }

    Setting Load(const std::wstring& path)
    {
        Setting s;
        wchar_t custom[32]{};
        GetPrivateProfileStringW(L"Appearance", L"AccentCustom", L"", custom, ARRAYSIZE(custom), path.c_str());
        if (uint32_t rgb = 0; custom[0] && ParseHex(custom, rgb))
            s.custom = rgb;
        else if (custom[0])
            log::Warn(L"settings: [Appearance] AccentCustom='{}' is not a colour like #12A58E; ignoring it", custom);
        s.choice = static_cast<Choice>(IniChoice(path, L"Appearance", L"Accent", kIds, static_cast<int>(Choice::Windows)));
        if (s.choice == Choice::Custom && s.custom == kNoColour)
        {
            log::Warn(L"settings: [Appearance] Accent=custom without an AccentCustom colour; using windows");
            s.choice = Choice::Windows;
        }
        return s;
    }

    void Save(const Setting& s, const std::wstring& path)
    {
        WritePrivateProfileStringW(L"Appearance", L"Accent", kIds[static_cast<int>(s.choice)], path.c_str());
        WritePrivateProfileStringW(L"Appearance", L"AccentCustom", s.custom == kNoColour ? nullptr : ToHex(s.custom).c_str(), path.c_str());
    }

    double Contrast(uint32_t a, uint32_t b)
    {
        const double la = Luminance(a), lb = Luminance(b);
        return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
    }

    uint32_t TextOn(uint32_t fill)
    {
        return Contrast(fill, kTextOnDark) >= Contrast(fill, kTextOnLight) ? kTextOnDark : kTextOnLight;
    }

    uint32_t Mix(uint32_t a, uint32_t b, double t)
    {
        t = std::clamp(t, 0.0, 1.0);
        uint32_t out = 0;
        for (int shift = 0; shift <= 16; shift += 8)
        {
            const double ca = static_cast<double>((a >> shift) & 0xFF), cb = static_cast<double>((b >> shift) & 0xFF);
            out |= static_cast<uint32_t>(std::lround(ca + (cb - ca) * t)) << shift;
        }
        return out;
    }

    uint32_t Lighter(uint32_t colour, uint32_t text, double most)
    {
        constexpr int kSteps = 20;
        uint32_t best = colour;
        for (int i = 1; i <= kSteps; ++i)
        {
            const uint32_t c = Mix(colour, 0xFFFFFF, most * i / kSteps);
            if (Contrast(c, text) < kMinContrast)
                break;
            best = c;
        }
        return best;
    }

    Variants Preset(Choice choice)
    {
        const int i = static_cast<int>(choice);
        return i >= 0 && i < kPresetCount ? kPresets[i] : kPresets[0];
    }

    Variants Derive(uint32_t rgb)
    {
        rgb &= 0xFFFFFF;
        Variants v{};
        // A mid tone neither text reads on goes the way of the text that comes closest.
        v.fill = Toward(rgb, TextOn(rgb) == kTextOnDark ? 0xFFFFFF : 0x000000, FillReads);
        v.dark = Toward(rgb, 0xFFFFFF, DarkReads);
        v.light = Toward(rgb, 0x000000, LightReads);
        return v;
    }

    Variants Resolve(const Setting& s, uint32_t windowsRgb)
    {
        switch (s.choice)
        {
        case Choice::Custom: return s.custom == kNoColour ? Preset(Choice::Tide) : Derive(s.custom);
        case Choice::Windows: return Derive(windowsRgb);
        default: return Preset(s.choice);
        }
    }

    bool Readable(const Variants& v)
    {
        return FillReads(v.fill) && DarkReads(v.dark) && LightReads(v.light);
    }

    void Set(const Setting& setting)
    {
        g_setting = setting;
    }

    Variants Current()
    {
        return Resolve(g_setting, g_setting.choice == Choice::Windows ? WindowsRgb() : 0);
    }

    uint32_t WindowsRgb()
    {
        // Explorer's AccentColorMenu (0xAABBGGRR) is the colour Windows uses for selection; DWM's
        // colorization colour (0xAARRGGBB, what DwmGetColorizationColor returns) is the fallback.
        DWORD value = 0, size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentColorMenu", RRF_RT_REG_DWORD,
                         nullptr, &value, &size) == ERROR_SUCCESS)
            return ((value & 0xFF) << 16) | (value & 0xFF00) | ((value >> 16) & 0xFF);
        size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"ColorizationColor", RRF_RT_REG_DWORD, nullptr, &value,
                         &size) == ERROR_SUCCESS)
            return value & 0xFFFFFF;
        return 0x0067C0;   // Windows' default blue
    }
}
