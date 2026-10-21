#pragma once
#include "ErrorText.h"

#include <winrt/base.h>

#include <format>
#include <string>
#include <string_view>

namespace sd
{
    // A Windows Runtime error as the user reads it: its own message, or "<fallback> (0x80070005)"
    // when it has none. C++/WinRT only, so it lives apart from ErrorText.h.
    inline std::wstring WinRtMessage(const winrt::hresult_error& e, std::wstring_view fallback)
    {
        auto text = TrimMessage(std::wstring(e.message()));
        return text.empty() ? std::format(L"{} ({})", fallback, HexCode(e.code())) : text;
    }
}
