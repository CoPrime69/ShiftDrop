#include "Settings.h"
#include "Log.h"

#include <cwctype>

namespace sd
{
    const char* RingId(Ring ring)
    {
        switch (ring)
        {
        case Ring::Convert: return "convert";
        case Ring::Tools: return "tools";
        default: return "files";
        }
    }

    const wchar_t* RingTitle(Ring ring)
    {
        switch (ring)
        {
        case Ring::Convert: return L"Convert";
        case Ring::Tools: return L"Tools";
        default: return L"File actions";
        }
    }

    std::wstring SettingsPath()
    {
        return log::AppDataFolder() + L"\\settings.ini";
    }

    namespace
    {
        bool ParseKey(std::wstring text, UINT& vk, std::wstring& name)
        {
            for (auto& c : text)
                c = static_cast<wchar_t>(std::towlower(c));
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
            WritePrivateProfileStringW(L"Gesture", L"Tools", L"Ctrl", path.c_str());
            WritePrivateProfileStringW(L"Gesture", L"Files", L"Alt", path.c_str());
            WritePrivateProfileStringW(L"Gesture", L"; Allowed", L"Shift | Ctrl | Alt | None  (the Win key is not supported)", path.c_str());
        }

        Settings s;
        const wchar_t* keys[kRingCount] = { L"Convert", L"Tools", L"Files" };
        for (int i = 0; i < kRingCount; ++i)
        {
            wchar_t buf[32]{};
            GetPrivateProfileStringW(L"Gesture", keys[i], L"", buf, ARRAYSIZE(buf), path.c_str());
            // M0/M1 files used TriggerKey for the only wheel there was.
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
        log::Info(L"settings: Convert={}, Tools={}, Files={} ({})", s.keyNames[0], s.keyNames[1], s.keyNames[2], path);
        return s;
    }

    Appearance LoadAppearance()
    {
        const auto path = SettingsPath();
        wchar_t size[16]{}, style[16]{};
        GetPrivateProfileStringW(L"Appearance", L"Size", L"auto", size, ARRAYSIZE(size), path.c_str());
        GetPrivateProfileStringW(L"Appearance", L"Style", L"solid", style, ARRAYSIZE(style), path.c_str());
        Appearance a;
        const auto is = [](const wchar_t* value, const wchar_t* name) { return CompareStringOrdinal(value, -1, name, -1, TRUE) == CSTR_EQUAL; };
        if (is(size, L"small"))
            a.size = WheelSize::Small;
        else if (is(size, L"medium"))
            a.size = WheelSize::Medium;
        else if (is(size, L"large"))
            a.size = WheelSize::Large;
        a.style = is(style, L"glass") ? Style::Glass : Style::Solid;
        a.hideCursor = GetPrivateProfileIntW(L"Appearance", L"HideCursor", 1, path.c_str()) != 0;
        return a;
    }

    void SaveAppearance(const Appearance& a)
    {
        const auto path = SettingsPath();
        const wchar_t* sizes[] = { L"auto", L"small", L"medium", L"large" };
        WritePrivateProfileStringW(L"Appearance", L"Size", sizes[static_cast<int>(a.size)], path.c_str());
        WritePrivateProfileStringW(L"Appearance", L"Style", a.style == Style::Glass ? L"glass" : L"solid", path.c_str());
        WritePrivateProfileStringW(L"Appearance", L"HideCursor", a.hideCursor ? L"1" : L"0", path.c_str());
    }
}
