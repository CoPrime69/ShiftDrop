#pragma once
#include <windows.h>
#include <string>

namespace sd
{
    // Loaded once at startup from %LOCALAPPDATA%\ShiftDrop\settings.ini.
    // A settings window comes in a later milestone; until then edit the file and restart.
    struct Settings
    {
        UINT triggerVk = VK_SHIFT;          // VK_SHIFT, VK_CONTROL or VK_MENU (Alt). The Win key is refused.
        std::wstring triggerName = L"Shift";
    };

    Settings LoadSettings();
    std::wstring SettingsPath();
}
