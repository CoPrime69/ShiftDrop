#include "Settings.h"
#include "Log.h"

#include <cwctype>

namespace sd
{
    std::wstring SettingsPath()
    {
        return log::AppDataFolder() + L"\\settings.ini";
    }

    Settings LoadSettings()
    {
        const auto path = SettingsPath();
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            // First run: write the defaults so the user can see what is configurable.
            WritePrivateProfileStringW(L"Gesture", L"TriggerKey", L"Shift", path.c_str());
            WritePrivateProfileStringW(L"Gesture", L"; Allowed", L"Shift | Ctrl | Alt  (the Win key is not supported)", path.c_str());
        }

        wchar_t buf[32]{};
        GetPrivateProfileStringW(L"Gesture", L"TriggerKey", L"Shift", buf, ARRAYSIZE(buf), path.c_str());
        std::wstring key(buf);
        for (auto& c : key)
            c = static_cast<wchar_t>(std::towlower(c));

        Settings s;
        if (key == L"shift")
        {
            s.triggerVk = VK_SHIFT;
            s.triggerName = L"Shift";
        }
        else if (key == L"ctrl" || key == L"control")
        {
            s.triggerVk = VK_CONTROL;
            s.triggerName = L"Ctrl";
        }
        else if (key == L"alt")
        {
            s.triggerVk = VK_MENU;
            s.triggerName = L"Alt";
        }
        else
        {
            log::Warn(L"settings: TriggerKey='{}' is not supported (use Shift, Ctrl or Alt); using Shift", buf);
        }
        log::Info(L"settings: trigger key = {} ({})", s.triggerName, path);
        return s;
    }
}
