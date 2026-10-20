#pragma once
#include <windows.h>

#include <array>
#include <string>

namespace sd
{
    // The three wheels and the modifier that opens each one.
    enum class Ring
    {
        Convert = 0,   // Shift opens the wheel here, on the best fit for the file type
        Tools = 1,     // reached by scrolling (no key of its own by default)
        Files = 2,     // reached by scrolling
    };
    inline constexpr int kRingCount = 3;
    const char* RingId(Ring ring);          // "convert", "tools", "files" (as in recipe JSON)
    const wchar_t* RingTitle(Ring ring);    // "Convert", "Tools", "File actions"

    // Loaded once at startup from %LOCALAPPDATA%\ShiftDrop\settings.ini. The settings window comes in
    // M12; until then edit the file and restart.
    struct Settings
    {
        // Virtual key that opens each ring (VK_SHIFT, VK_CONTROL, VK_MENU) or 0 for none. Only Shift
        // by default: every section is reached by scrolling, so Ctrl and Alt keep their usual meaning.
        std::array<UINT, kRingCount> keys{ VK_SHIFT, 0, 0 };
        std::array<std::wstring, kRingCount> keyNames{ L"Shift", L"off", L"off" };
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
        bool hideCursor = true;   // no mouse pointer over the wheel; the dragged item's picture marks the spot
    };
    Appearance LoadAppearance();
    void SaveAppearance(const Appearance& appearance);
}
