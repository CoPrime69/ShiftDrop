#pragma once
#include <windows.h>

#include <array>
#include <string>
#include <string_view>

namespace sd
{
    // The wheel's sections, in the order scrolling visits them, and the key that opens each one.
    enum class Ring
    {
        Convert = 0,   // Shift opens the wheel here, on the best fit for the file type
        Tools = 1,     // the others are reached by scrolling (no key of their own by default)
        Reduce = 2,    // Reduce size: 90% to 20% smaller
        Files = 3,
    };
    inline constexpr int kRingCount = 4;
    const char* RingId(Ring ring);          // "convert", "tools", "reduce", "files" (as in recipe JSON)
    const wchar_t* RingTitle(Ring ring);    // "Convert", "Tools", "Reduce size", "File actions"
    Ring RingFromId(std::string_view id);   // the reverse of RingId; Convert for anything else

    // Loaded once at startup from %LOCALAPPDATA%\ShiftDrop\settings.ini. The settings window comes in
    // M12; until then edit the file and restart.
    struct Settings
    {
        // Virtual key that opens each ring (VK_SHIFT, VK_CONTROL, VK_MENU) or 0 for none. Only Shift
        // by default: every section is reached by scrolling, so Ctrl and Alt keep their usual meaning.
        std::array<UINT, kRingCount> keys{ VK_SHIFT, 0, 0, 0 };
        std::array<std::wstring, kRingCount> keyNames{ L"Shift", L"off", L"off", L"off" };
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

    // Where the outputs of files dragged out of a browser or a mail app go (they have no folder of
    // their own, see VirtualDrop.h); [Output] VirtualFiles=desktop|downloads|ask, set from the tray.
    enum class VirtualOutput { Desktop, Downloads, Ask };
    VirtualOutput LoadVirtualOutput();
    void SaveVirtualOutput(VirtualOutput choice);
}
