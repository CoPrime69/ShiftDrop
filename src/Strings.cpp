#include "Strings.h"

#include <windows.h>

#include <cwctype>

namespace sd
{
    std::wstring Lower(std::wstring_view s)
    {
        std::wstring out(s);
        for (auto& c : out)
            c = static_cast<wchar_t>(std::towlower(c));
        return out;
    }

    std::string AsciiLower(std::string_view s)
    {
        std::string out(s);
        for (auto& c : out)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return out;
    }

    std::wstring_view Trim(std::wstring_view s)
    {
        while (!s.empty() && std::iswspace(s.front()))
            s.remove_prefix(1);
        while (!s.empty() && std::iswspace(s.back()))
            s.remove_suffix(1);
        return s;
    }

    std::string_view Trim(std::string_view s)
    {
        const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
        while (!s.empty() && ws(s.front()))
            s.remove_prefix(1);
        while (!s.empty() && ws(s.back()))
            s.remove_suffix(1);
        return s;
    }

    bool EqualsNoCase(std::wstring_view a, std::wstring_view b)
    {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    }

    bool HasExtToken(std::wstring_view tokens, std::wstring_view ext)
    {
        if (ext.empty())
            return false;
        size_t pos = 0;
        while ((pos = tokens.find(ext, pos)) != std::wstring_view::npos)
        {
            const size_t end = pos + ext.size();
            if (end == tokens.size() || tokens[end] == L'.')
                return true;
            pos = end;
        }
        return false;
    }

    std::wstring Latin1(std::span<const uint8_t> bytes)
    {
        std::wstring s;
        for (const uint8_t c : bytes)
        {
            if (!c)
                break;
            s.push_back(static_cast<wchar_t>(c));
        }
        return s;
    }

    size_t Indent(std::string_view line)
    {
        size_t n = 0;
        while (n < line.size() && line[n] == ' ')
            ++n;
        return n;
    }
}
