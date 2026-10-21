#pragma once
#include "OutputPath.h"

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

    // Loaded once at startup from %LOCALAPPDATA%\ShiftDrop\settings.ini. The keys are not in the
    // Settings window: edit the file and restart.
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
    };
    Appearance LoadAppearance();
    void SaveAppearance(const Appearance& appearance);
    // The same for a given settings.ini (tests).
    Appearance LoadAppearance(const std::wstring& path);
    void SaveAppearance(const Appearance& appearance, const std::wstring& path);
    // Pixels per DIP for the wheel at `size` on a monitor with this DPI scale (1 = 100%) and height in
    // physical pixels (theme::ScaleFor reads both from the monitor). Auto is 1 on a 1080p screen at
    // 100% and grows with the screen's height in DIPs, between 0.9 and 1.3 times the DPI scale.
    float SizeScale(WheelSize size, float dpiScale, float monitorHeightPx);

    // Where the outputs of files dragged out of a browser or a mail app go (they have no folder of
    // their own, see VirtualDrop.h); [Output] VirtualFiles=desktop|downloads|ask, set from the tray.
    enum class VirtualOutput { Desktop, Downloads, Ask };
    VirtualOutput LoadVirtualOutput();
    void SaveVirtualOutput(VirtualOutput choice);

    // The rest of the Settings window (the slices are in Features.h). Each applies at once.
    enum class ThemeChoice { System, Light, Dark };   // [Appearance] Theme=system|light|dark
    // File actions › Terminal: [FileActions] Terminal=wt|powershell|cmd|custom, TerminalCommand=...
    enum class TerminalChoice { WindowsTerminal, PowerShell, CommandPrompt, Custom };
    struct Preferences
    {
        ThemeChoice theme = ThemeChoice::System;
        TerminalChoice terminal = TerminalChoice::WindowsTerminal;
        std::wstring terminalCommand;                        // Custom: a command line, {folder} for the folder
        OutputNaming naming = OutputNaming::Parentheses;     // [Output] Naming=parentheses|dash|underscore|subfolder
        bool operator==(const Preferences&) const = default;
    };
    Preferences LoadPreferences();
    void SavePreferences(const Preferences& preferences);
}
