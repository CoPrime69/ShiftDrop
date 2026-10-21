#include "Settings.h"
#include "Log.h"
#include "PanelLayout.h"
#include "Strings.h"

#include <algorithm>

namespace sd
{
    const char* RingId(Ring ring)
    {
        switch (ring)
        {
        case Ring::Convert: return "convert";
        case Ring::Tools: return "tools";
        case Ring::Reduce: return "reduce";
        default: return "files";
        }
    }

    const wchar_t* RingTitle(Ring ring)
    {
        switch (ring)
        {
        case Ring::Convert: return L"Convert";
        case Ring::Tools: return L"Tools";
        case Ring::Reduce: return L"Reduce size";
        default: return L"File actions";
        }
    }

    Ring RingFromId(std::string_view id)
    {
        for (int i = 0; i < kRingCount; ++i)
            if (id == RingId(static_cast<Ring>(i)))
                return static_cast<Ring>(i);
        return Ring::Convert;
    }

    std::wstring SettingsPath()
    {
        return log::AppDataFolder() + L"\\settings.ini";
    }

    namespace
    {
        bool ParseKey(std::wstring_view key, UINT& vk, std::wstring& name)
        {
            const auto text = Lower(key);
            if (text == L"shift") { vk = VK_SHIFT; name = L"Shift"; return true; }
            if (text == L"ctrl" || text == L"control") { vk = VK_CONTROL; name = L"Ctrl"; return true; }
            if (text == L"alt") { vk = VK_MENU; name = L"Alt"; return true; }
            if (text == L"none" || text == L"off") { vk = 0; name = L"off"; return true; }
            return false;   // includes "win": the Win key is not supported (it opens Start on release)
        }
    }

    Settings LoadSettings()
    {
        const auto path = SettingsPath();
        wchar_t probe[2]{};
        if (!GetPrivateProfileStringW(L"Gesture", nullptr, L"", probe, ARRAYSIZE(probe), path.c_str()))
        {
            // First run (the installer may already have written [Startup]): write the defaults so the
            // user can see what is configurable.
            WritePrivateProfileStringW(L"Gesture", L"Convert", L"Shift", path.c_str());
            WritePrivateProfileStringW(L"Gesture", L"Tools", L"None", path.c_str());
            WritePrivateProfileStringW(L"Gesture", L"Files", L"None", path.c_str());
            WritePrivateProfileStringW(L"Gesture", L"; Allowed", L"Shift | Ctrl | Alt | None  (the Win key is not supported)", path.c_str());
            WritePrivateProfileStringW(L"Gesture", L"KeysVersion", L"2", path.c_str());
        }
        else if (GetPrivateProfileIntW(L"Gesture", L"KeysVersion", 1, path.c_str()) < 2)
        {
            // Files from 0.5.0 and earlier wrote Tools=Ctrl and Files=Alt. Shift is now the only key
            // (sections switch by scrolling), so those two are turned off once; a later hand edit stays.
            WritePrivateProfileStringW(L"Gesture", L"Tools", L"None", path.c_str());
            WritePrivateProfileStringW(L"Gesture", L"Files", L"None", path.c_str());
            WritePrivateProfileStringW(L"Gesture", L"KeysVersion", L"2", path.c_str());
            log::Info(L"settings: Ctrl and Alt no longer open wheels; Shift and scrolling reach every section");
        }

        Settings s;
        const wchar_t* keys[kRingCount] = { L"Convert", L"Tools", L"Reduce", L"Files" };
        for (int i = 0; i < kRingCount; ++i)
        {
            wchar_t buf[32]{};
            GetPrivateProfileStringW(L"Gesture", keys[i], L"", buf, ARRAYSIZE(buf), path.c_str());
            // The first versions used TriggerKey for the only wheel there was.
            if (i == 0 && !buf[0])
                GetPrivateProfileStringW(L"Gesture", L"TriggerKey", L"", buf, ARRAYSIZE(buf), path.c_str());
            if (!buf[0])
                continue;   // keep the default
            UINT vk = 0;
            std::wstring name;
            if (ParseKey(buf, vk, name))
            {
                s.keys[i] = vk;
                s.keyNames[i] = name;
            }
            else
            {
                log::Warn(L"settings: {}='{}' is not supported (use Shift, Ctrl, Alt or None); keeping {}", keys[i], buf, s.keyNames[i]);
            }
        }
        // The same key cannot open two wheels: the first ring keeps it.
        for (int i = 0; i < kRingCount; ++i)
            for (int j = i + 1; j < kRingCount; ++j)
                if (s.keys[i] && s.keys[i] == s.keys[j])
                {
                    log::Warn(L"settings: {} is set for both {} and {}; turning {} off", s.keyNames[j], keys[i], keys[j], keys[j]);
                    s.keys[j] = 0;
                    s.keyNames[j] = L"off";
                }
        log::Info(L"settings: Convert={}, Tools={}, Reduce={}, Files={} ({})", s.keyNames[0], s.keyNames[1], s.keyNames[2], s.keyNames[3], path);
        return s;
    }

    Appearance LoadAppearance() { return LoadAppearance(SettingsPath()); }

    Appearance LoadAppearance(const std::wstring& path)
    {
        wchar_t size[16]{}, style[16]{};
        GetPrivateProfileStringW(L"Appearance", L"Size", L"auto", size, ARRAYSIZE(size), path.c_str());
        GetPrivateProfileStringW(L"Appearance", L"Style", L"solid", style, ARRAYSIZE(style), path.c_str());
        Appearance a;
        if (EqualsNoCase(size, L"small"))
            a.size = WheelSize::Small;
        else if (EqualsNoCase(size, L"medium"))
            a.size = WheelSize::Medium;
        else if (EqualsNoCase(size, L"large"))
            a.size = WheelSize::Large;
        a.style = EqualsNoCase(style, L"glass") ? Style::Glass : Style::Solid;
        return a;
    }

    void SaveAppearance(const Appearance& a) { SaveAppearance(a, SettingsPath()); }

    void SaveAppearance(const Appearance& a, const std::wstring& path)
    {
        const wchar_t* sizes[] = { L"auto", L"small", L"medium", L"large" };
        WritePrivateProfileStringW(L"Appearance", L"Size", sizes[static_cast<int>(a.size)], path.c_str());
        WritePrivateProfileStringW(L"Appearance", L"Style", a.style == Style::Glass ? L"glass" : L"solid", path.c_str());
        // Older versions could hide the pointer over the wheel; the setting is gone, so is its key.
        WritePrivateProfileStringW(L"Appearance", L"HideCursor", nullptr, path.c_str());
    }

    float SizeScale(WheelSize size, float dpiScale, float monitorHeightPx)
    {
        dpiScale = std::max(dpiScale, 0.5f);
        switch (size)
        {
        case WheelSize::Small: return dpiScale * 0.85f;
        case WheelSize::Medium: return dpiScale;
        case WheelSize::Large: return dpiScale * 1.2f;
        default:
            // 1080 rows at 100% is the reference; 1440p at 100% gets 1.3x (1.33 clamped).
            return dpiScale * std::clamp(monitorHeightPx / dpiScale / 1080.f, 0.9f, 1.3f);
        }
    }

    namespace
    {
        constexpr const wchar_t* kVirtualOutputs[] = { L"desktop", L"downloads", L"ask" };
    }

    VirtualOutput LoadVirtualOutput()
    {
        wchar_t value[16]{};
        GetPrivateProfileStringW(L"Output", L"VirtualFiles", L"desktop", value, ARRAYSIZE(value), SettingsPath().c_str());
        for (int i = 0; i < 3; ++i)
            if (EqualsNoCase(value, kVirtualOutputs[i]))
                return static_cast<VirtualOutput>(i);
        log::Warn(L"settings: [Output] VirtualFiles='{}' is not desktop, downloads or ask; using desktop", value);
        return VirtualOutput::Desktop;
    }

    void SaveVirtualOutput(VirtualOutput choice)
    {
        WritePrivateProfileStringW(L"Output", L"VirtualFiles", kVirtualOutputs[static_cast<int>(choice)], SettingsPath().c_str());
    }

    namespace
    {
        constexpr const wchar_t* kThemes[] = { L"system", L"light", L"dark" };
        constexpr const wchar_t* kTerminals[] = { L"wt", L"powershell", L"cmd", L"custom" };
        constexpr const wchar_t* kNamings[] = { L"parentheses", L"dash", L"underscore", L"subfolder" };

        // The index of `value` in `names` (ignoring case), or `fallback` with a warning.
        template <size_t N>
        int Pick(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* const (&names)[N], int fallback)
        {
            wchar_t value[32]{};
            GetPrivateProfileStringW(section, key, names[fallback], value, ARRAYSIZE(value), path.c_str());
            for (size_t i = 0; i < N; ++i)
                if (EqualsNoCase(value, names[i]))
                    return static_cast<int>(i);
            log::Warn(L"settings: [{}] {}='{}' is not known; using {}", section, key, value, names[fallback]);
            return fallback;
        }
    }

    Preferences LoadPreferences() { return LoadPreferences(SettingsPath()); }

    Preferences LoadPreferences(const std::wstring& path)
    {
        Preferences p;
        p.theme = static_cast<ThemeChoice>(Pick(path, L"Appearance", L"Theme", kThemes, 0));
        p.terminal = static_cast<TerminalChoice>(Pick(path, L"FileActions", L"Terminal", kTerminals, 0));
        p.naming = static_cast<OutputNaming>(Pick(path, L"Output", L"Naming", kNamings, 0));
        std::wstring command(2048, L'\0');
        command.resize(GetPrivateProfileStringW(L"FileActions", L"TerminalCommand", L"", command.data(), static_cast<DWORD>(command.size()),
                                                path.c_str()));
        p.terminalCommand = std::move(command);
        const int size = static_cast<int>(GetPrivateProfileIntW(L"Appearance", L"PanelSize", panel::kSizeMinPercent, path.c_str()));
        p.panelSize = panel::SizePercent(size);
        if (p.panelSize != size)
            log::Warn(L"settings: [Appearance] PanelSize={} is not {} to {} in steps of {}; using {}", size, panel::kSizeMinPercent,
                      panel::kSizeMaxPercent, panel::kSizeStepPercent, p.panelSize);
        return p;
    }

    void SavePreferences(const Preferences& p) { SavePreferences(p, SettingsPath()); }

    void SavePreferences(const Preferences& p, const std::wstring& path)
    {
        WritePrivateProfileStringW(L"Appearance", L"Theme", kThemes[static_cast<int>(p.theme)], path.c_str());
        WritePrivateProfileStringW(L"FileActions", L"Terminal", kTerminals[static_cast<int>(p.terminal)], path.c_str());
        // A value enclosed in quotes loses them when read back ("C:\term.exe" -d "{folder}"), so such
        // a command is stored inside another pair.
        const auto& c = p.terminalCommand;
        const bool enclosed = c.size() >= 2 && c.front() == c.back() && (c.front() == L'"' || c.front() == L'\'');
        const auto command = enclosed ? L"\"" + c + L"\"" : c;
        WritePrivateProfileStringW(L"FileActions", L"TerminalCommand", command.c_str(), path.c_str());
        WritePrivateProfileStringW(L"Output", L"Naming", kNamings[static_cast<int>(p.naming)], path.c_str());
        WritePrivateProfileStringW(L"Appearance", L"PanelSize", std::to_wstring(panel::SizePercent(p.panelSize)).c_str(), path.c_str());
    }
}
