#pragma once
#include <windows.h>

#include <string>
#include <string_view>

namespace sd
{
    // A global hotkey as RegisterHotKey takes it: MOD_CONTROL / MOD_SHIFT / MOD_ALT and a virtual key.
    struct Hotkey
    {
        UINT modifiers = 0;
        UINT vk = 0;
        bool operator==(const Hotkey&) const = default;
    };

    inline constexpr Hotkey kDefaultExplorerHotkey{ MOD_CONTROL | MOD_SHIFT, VK_SPACE };

    // "Ctrl+Shift+Space", "Alt+F2", "Ctrl+Alt+S" (any case, spaces allowed around '+'). Needs Ctrl or
    // Alt, except with a function key, so a hotkey never swallows ordinary typing. The Win key is
    // refused: Windows reserves it. `error` says why a text was refused.
    bool ParseHotkey(std::wstring_view text, Hotkey& out, std::wstring* error = nullptr);
    std::wstring HotkeyText(const Hotkey& hotkey);   // canonical form, e.g. "Ctrl+Shift+Space"
}
