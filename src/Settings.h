#pragma once
#include <windows.h>

#include <array>
#include <string>

namespace sd
{
    // The three wheels and the modifier that opens each one.
    enum class Ring
    {
        Convert = 0,   // Shift by default
        Tools = 1,     // Ctrl (Tangerine uses Command)
        Files = 2,     // Alt
    };
    inline constexpr int kRingCount = 3;
    const char* RingId(Ring ring);          // "convert", "tools", "files" (as in recipe JSON)
    const wchar_t* RingTitle(Ring ring);    // "Convert", "Tools", "File actions"

    // Loaded once at startup from %LOCALAPPDATA%\ShiftDrop\settings.ini. The settings window comes in
    // M12; until then edit the file and restart.
    struct Settings
    {
        // Virtual key per ring (VK_SHIFT, VK_CONTROL, VK_MENU) or 0 when that wheel is turned off.
        std::array<UINT, kRingCount> keys{ VK_SHIFT, VK_CONTROL, VK_MENU };
        std::array<std::wstring, kRingCount> keyNames{ L"Shift", L"Ctrl", L"Alt" };
    };

    Settings LoadSettings();
    std::wstring SettingsPath();

    // How the wheel and the top panel look; changed from the tray menu, saved in [Appearance].
    enum class WheelSize { Auto, Small, Medium, Large };   // Auto: from the monitor's size and DPI
    enum class Style { Solid, Glass };                      // Glass: frosted, blurred background
    struct Appearance
    {
        WheelSize size = WheelSize::Auto;
        Style style = Style::Solid;
    };
    Appearance LoadAppearance();
    void SaveAppearance(const Appearance& appearance);
}
