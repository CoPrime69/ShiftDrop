#pragma once
#include <string>
#include <string_view>

namespace sd
{
    // UTF-8 <-> UTF-16. JSON, SQLite and the log use UTF-8; every Win32 call uses UTF-16.
    // Neither direction fails: an invalid UTF-8 sequence, or an unpaired surrogate in a file name,
    // comes out as U+FFFD. The Win32 versions are in platform/win/Text.cpp.
    std::wstring Widen(std::string_view s);
    std::string Narrow(std::wstring_view s);
}
