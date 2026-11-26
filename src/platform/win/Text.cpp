// The Win32 side of Utf.h and Strings.h: UTF-8 <-> UTF-16 and comparing names ignoring case. Kept
// here so the shared files make no Win32 calls of their own; another platform has its own versions.
#include "Strings.h"
#include "Utf.h"

#include <windows.h>

namespace sd
{
    std::wstring Widen(std::string_view s)
    {
        if (s.empty())
            return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
        return out;
    }

    std::string Narrow(std::wstring_view s)
    {
        if (s.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    bool EqualsNoCase(std::wstring_view a, std::wstring_view b)
    {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    }
}
