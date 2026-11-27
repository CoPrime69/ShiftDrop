#pragma once
#include <cstddef>
#include <string>

// settings.ini, one value at a time. On Windows these are the Private Profile calls
// (GetPrivateProfileString and friends), with their rules: section and key names ignore case, and a
// value enclosed in quotes loses them when it is read back.
namespace sd::platform
{
    // [section] key, or `fallback` when the file, the section or the key is missing. At most
    // `capacity` - 1 characters come back, as from a buffer of `capacity` characters: each caller
    // keeps the size it has always read with.
    std::wstring ReadIniString(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback, size_t capacity);
    // [section] key as a whole number, or `fallback` when it is missing. Text that is not a number
    // reads as 0, and so does a negative number.
    unsigned ReadIniNumber(const std::wstring& path, const wchar_t* section, const wchar_t* key, int fallback);
    // Does [section] hold at least one key? (LoadSettings: has this file been set up before?)
    bool IniSectionHasKeys(const std::wstring& path, const wchar_t* section);
    // Sets [section] key to `value`, adding the file, the section or the key as needed.
    void WriteIniValue(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* value);
    // Removes [section] key, if it is there.
    void DeleteIniValue(const std::wstring& path, const wchar_t* section, const wchar_t* key);
}
