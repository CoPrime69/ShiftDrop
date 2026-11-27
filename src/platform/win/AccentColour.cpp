// The Win32 side of Accent.h: the user's Windows accent colour, from the registry.
#include "Accent.h"

#include <windows.h>

namespace sd::accent
{
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
