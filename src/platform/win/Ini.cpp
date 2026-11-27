// The Win32 side of platform/Ini.h: the Private Profile calls.
#include "platform/Ini.h"

#include <windows.h>

namespace sd::platform
{
    std::wstring ReadIniString(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback, size_t capacity)
    {
        std::wstring value(capacity, L'\0');
        value.resize(GetPrivateProfileStringW(section, key, fallback, value.data(), static_cast<DWORD>(value.size()), path.c_str()));
        return value;
    }

    unsigned ReadIniNumber(const std::wstring& path, const wchar_t* section, const wchar_t* key, int fallback)
    {
        return GetPrivateProfileIntW(section, key, fallback, path.c_str());
    }

    bool IniSectionHasKeys(const std::wstring& path, const wchar_t* section)
    {
        wchar_t probe[2]{};
        return GetPrivateProfileStringW(section, nullptr, L"", probe, ARRAYSIZE(probe), path.c_str()) != 0;
    }

    void WriteIniValue(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* value)
    {
        WritePrivateProfileStringW(section, key, value, path.c_str());
    }

    void DeleteIniValue(const std::wstring& path, const wchar_t* section, const wchar_t* key)
    {
        WritePrivateProfileStringW(section, key, nullptr, path.c_str());
    }
}
