#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Small text helpers every area shares: case, whitespace, case-insensitive comparison and the
// ".ext.ext" token lists. One copy each, so "the same name" means the same thing everywhere.
namespace sd
{
    // Lower-case copy with towlower, in the C locale the app runs in: file names, extensions, key names.
    std::wstring Lower(std::wstring_view s);
    // Lower-case copy of the ASCII letters only (A-Z): format names, XML and HTML keywords.
    std::string AsciiLower(std::string_view s);

    // Without whitespace at either end (iswspace / ASCII space, tab, CR, LF, FF, VT).
    std::wstring_view Trim(std::wstring_view s);
    std::string_view Trim(std::string_view s);

    // Equal ignoring case the way NTFS compares names (ordinal, no locale).
    bool EqualsNoCase(std::wstring_view a, std::wstring_view b);

    // Is `ext` (".mp4") a whole token of a run like ".mp4.m4v"? ".mp" is not.
    bool HasExtToken(std::wstring_view tokens, std::wstring_view ext);

    // Latin-1 bytes as text, up to the first NUL (EXIF and PNG text chunks).
    std::wstring Latin1(std::span<const uint8_t> bytes);

    // The number of spaces a line starts with (ffmpeg's indented stream listing).
    size_t Indent(std::string_view line);
}
