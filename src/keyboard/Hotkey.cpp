#include "keyboard/Hotkey.h"
#include "Strings.h"

#include <cwctype>
#include <format>
#include <vector>

namespace sd
{
    namespace
    {
        struct KeyName
        {
            const wchar_t* name;
            UINT vk;
        };
        // Named keys besides letters, digits and F1..F24.
        constexpr KeyName kKeys[] = {
            { L"Space", VK_SPACE },     { L"Enter", VK_RETURN },   { L"Tab", VK_TAB },
            { L"Insert", VK_INSERT },   { L"Delete", VK_DELETE },  { L"Home", VK_HOME },
            { L"End", VK_END },         { L"PageUp", VK_PRIOR },   { L"PageDown", VK_NEXT },
            { L"Up", VK_UP },           { L"Down", VK_DOWN },      { L"Left", VK_LEFT },
            { L"Right", VK_RIGHT },     { L"Pause", VK_PAUSE },    { L"Backspace", VK_BACK },
        };

        bool IsFunctionKey(UINT vk) { return vk >= VK_F1 && vk <= VK_F24; }

        bool KeyOf(std::wstring_view token, UINT& vk)
        {
            const auto lower = Lower(token);
            if (lower.size() == 1 && ((lower[0] >= L'a' && lower[0] <= L'z') || (lower[0] >= L'0' && lower[0] <= L'9')))
            {
                vk = static_cast<UINT>(std::towupper(lower[0]));   // VK_A..VK_Z and VK_0..VK_9 are the ASCII codes
                return true;
            }
            if (lower.size() >= 2 && lower[0] == L'f')
            {
                int n = 0;
                for (size_t i = 1; i < lower.size(); ++i)
                {
                    if (lower[i] < L'0' || lower[i] > L'9')
                        return false;
                    n = n * 10 + (lower[i] - L'0');
                }
                if (n >= 1 && n <= 24)
                {
                    vk = static_cast<UINT>(VK_F1 + n - 1);
                    return true;
                }
                return false;
            }
            for (const auto& k : kKeys)
                if (Lower(k.name) == lower)
                {
                    vk = k.vk;
                    return true;
                }
            return false;
        }
    }

    bool ParseHotkey(std::wstring_view text, Hotkey& out, std::wstring* error)
    {
        const auto fail = [&](std::wstring why) {
            if (error)
                *error = std::move(why);
            return false;
        };
        Hotkey key;
        size_t start = 0;
        std::vector<std::wstring_view> tokens;
        while (start <= text.size())
        {
            const size_t plus = text.find(L'+', start);
            const auto token = Trim(text.substr(start, plus == std::wstring_view::npos ? std::wstring_view::npos : plus - start));
            tokens.push_back(token);
            if (plus == std::wstring_view::npos)
                break;
            start = plus + 1;
        }
        for (size_t i = 0; i < tokens.size(); ++i)
        {
            const auto lower = Lower(tokens[i]);
            if (lower.empty())
                return fail(L"empty key name");
            if (lower == L"ctrl" || lower == L"control")
                key.modifiers |= MOD_CONTROL;
            else if (lower == L"shift")
                key.modifiers |= MOD_SHIFT;
            else if (lower == L"alt")
                key.modifiers |= MOD_ALT;
            else if (lower == L"win" || lower == L"windows")
                return fail(L"the Win key is not supported (Windows reserves it)");
            else if (i + 1 != tokens.size())
                return fail(std::format(L"'{}' must come last", tokens[i]));
            else if (!KeyOf(tokens[i], key.vk))
                return fail(std::format(L"unknown key '{}'", tokens[i]));
        }
        if (!key.vk)
            return fail(L"no key after the modifiers");
        if (!(key.modifiers & (MOD_CONTROL | MOD_ALT)) && !IsFunctionKey(key.vk))
            return fail(L"needs Ctrl or Alt (or a function key), so it does not take over typing");
        out = key;
        return true;
    }

    std::wstring HotkeyText(const Hotkey& hotkey)
    {
        std::wstring text;
        if (hotkey.modifiers & MOD_CONTROL)
            text += L"Ctrl+";
        if (hotkey.modifiers & MOD_SHIFT)
            text += L"Shift+";
        if (hotkey.modifiers & MOD_ALT)
            text += L"Alt+";
        const UINT vk = hotkey.vk;
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
            text += static_cast<wchar_t>(vk);
        else if (IsFunctionKey(vk))
            text += std::format(L"F{}", vk - VK_F1 + 1);
        else
        {
            for (const auto& k : kKeys)
                if (k.vk == vk)
                    return text + k.name;
            text += std::format(L"0x{:02X}", vk);
        }
        return text;
    }
}
