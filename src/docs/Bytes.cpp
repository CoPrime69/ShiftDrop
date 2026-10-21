#include "docs/Bytes.h"
#include "Utf.h"

#include <windows.h>
#include <objbase.h>
#include <wil/resource.h>

#include <format>

namespace sd::docs
{
    bool ReadFileBytes(const std::wstring& path, std::string& bytes, std::wstring* error)
    {
        wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file)
        {
            if (error)
                *error = std::format(L"cannot open {} (error {})", path, GetLastError());
            return false;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size) || size.QuadPart > (1ll << 31))
        {
            if (error)
                *error = L"file is too large (over 2 GB)";
            return false;
        }
        bytes.resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (done < bytes.size())
        {
            DWORD read = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 1u << 24));
            if (!ReadFile(file.get(), bytes.data() + done, chunk, &read, nullptr) || read == 0)
            {
                if (error)
                    *error = std::format(L"cannot read {}", path);
                return false;
            }
            done += read;
        }
        return true;
    }

    bool WriteFileBytes(const std::wstring& path, std::string_view bytes, std::wstring* error)
    {
        // TRUNCATE_EXISTING, not CREATE_ALWAYS: the staging file already exists (StagedOutput reserved
        // it), and this must never create a file under any other name.
        wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file)
        {
            if (error)
                *error = std::format(L"cannot write the output (error {})", GetLastError());
            return false;
        }
        size_t done = 0;
        while (done < bytes.size())
        {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 1u << 24));
            if (!WriteFile(file.get(), bytes.data() + done, chunk, &written, nullptr) || written == 0)
            {
                if (error)
                    *error = std::format(L"cannot write the output (error {})", GetLastError());
                return false;
            }
            done += written;
        }
        return true;
    }

    bool IsValidUtf8(std::string_view s)
    {
        size_t i = 0;
        while (i < s.size())
        {
            const auto c = static_cast<unsigned char>(s[i]);
            size_t extra;
            if (c < 0x80)
                extra = 0;
            else if (c >= 0xC2 && c <= 0xDF)
                extra = 1;
            else if (c >= 0xE0 && c <= 0xEF)
                extra = 2;
            else if (c >= 0xF0 && c <= 0xF4)
                extra = 3;
            else
                return false;   // a continuation byte, an overlong lead (C0/C1) or past U+10FFFF
            if (i + extra >= s.size() && extra > 0)
                return false;   // truncated sequence at the end
            for (size_t k = 1; k <= extra; ++k)
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                    return false;
            i += extra + 1;
        }
        return true;
    }

    TextEncoding DetectEncoding(std::string_view b)
    {
        if (b.size() >= 3 && b.substr(0, 3) == "\xEF\xBB\xBF")
            return TextEncoding::Utf8Bom;
        if (b.size() >= 2 && b.substr(0, 2) == "\xFF\xFE")
            return TextEncoding::Utf16Le;
        if (b.size() >= 2 && b.substr(0, 2) == "\xFE\xFF")
            return TextEncoding::Utf16Be;
        // UTF-16 without a BOM: ASCII-heavy text has a NUL in every other byte.
        if (b.size() >= 4)
        {
            size_t evenNul = 0, oddNul = 0;
            const size_t n = std::min<size_t>(b.size() & ~size_t(1), 4096);
            for (size_t i = 0; i < n; i += 2)
            {
                evenNul += b[i] == '\0';
                oddNul += b[i + 1] == '\0';
            }
            if (oddNul > n / 4 && evenNul == 0)
                return TextEncoding::Utf16Le;
            if (evenNul > n / 4 && oddNul == 0)
                return TextEncoding::Utf16Be;
        }
        return IsValidUtf8(b) ? TextEncoding::Utf8 : TextEncoding::Ansi;
    }

    std::string DecodeText(std::string_view b, TextEncoding* detected)
    {
        const auto enc = DetectEncoding(b);
        if (detected)
            *detected = enc;
        switch (enc)
        {
        case TextEncoding::Utf8:
            return std::string(b);
        case TextEncoding::Utf8Bom:
            return std::string(b.substr(3));
        case TextEncoding::Utf16Le:
        case TextEncoding::Utf16Be:
        {
            const bool bom = b.size() >= 2 && (b.substr(0, 2) == "\xFF\xFE" || b.substr(0, 2) == "\xFE\xFF");
            std::wstring w;
            w.reserve(b.size() / 2);
            for (size_t i = bom ? 2 : 0; i + 1 < b.size(); i += 2)
            {
                const auto lo = static_cast<unsigned char>(b[i]), hi = static_cast<unsigned char>(b[i + 1]);
                w.push_back(static_cast<wchar_t>(enc == TextEncoding::Utf16Le ? (hi << 8 | lo) : (lo << 8 | hi)));
            }
            return Narrow(w);
        }
        case TextEncoding::Ansi:
        {
            const int n = MultiByteToWideChar(CP_ACP, 0, b.data(), static_cast<int>(b.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_ACP, 0, b.data(), static_cast<int>(b.size()), w.data(), n);
            return Narrow(w);
        }
        }
        return std::string(b);
    }

    std::string NormalizeNewlines(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '\r')
            {
                out.push_back('\n');
                if (i + 1 < s.size() && s[i + 1] == '\n')
                    ++i;
            }
            else
                out.push_back(s[i]);
        }
        return out;
    }

    std::string XmlEscape(std::string_view s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (const char ch : s)
        {
            const auto c = static_cast<unsigned char>(ch);
            switch (ch)
            {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // XML 1.0 allows only tab, LF and CR below 0x20.
                if (c >= 0x20 || ch == '\t' || ch == '\n' || ch == '\r')
                    out.push_back(ch);
                break;
            }
        }
        return out;
    }

    void AppendUtf8(std::string& out, char32_t cp)
    {
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            cp = 0xFFFD;
        if (cp < 0x80)
            out.push_back(static_cast<char>(cp));
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::string DecodeEntity(std::string_view name)
    {
        std::string out;
        if (name.size() >= 2 && name[0] == '#')
        {
            char32_t cp = 0;
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const auto digits = name.substr(hex ? 2 : 1);
            if (digits.empty() || digits.size() > 8)
                return {};
            for (const char c : digits)
            {
                int v;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (hex && c >= 'a' && c <= 'f') v = c - 'a' + 10;
                else if (hex && c >= 'A' && c <= 'F') v = c - 'A' + 10;
                else return {};
                cp = cp * (hex ? 16 : 10) + static_cast<char32_t>(v);
            }
            AppendUtf8(out, cp);
            return out;
        }
        // The named references that turn up in real documents; the rest stay as written.
        static const struct { const char* name; char32_t cp; } kNamed[] = {
            { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
            { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "hellip", 0x2026 }, { "mdash", 0x2014 },
            { "ndash", 0x2013 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
            { "laquo", 0xAB }, { "raquo", 0xBB }, { "bull", 0x2022 }, { "middot", 0xB7 }, { "deg", 0xB0 },
            { "euro", 0x20AC }, { "pound", 0xA3 }, { "yen", 0xA5 }, { "cent", 0xA2 }, { "sect", 0xA7 },
            { "para", 0xB6 }, { "times", 0xD7 }, { "divide", 0xF7 }, { "plusmn", 0xB1 }, { "frac12", 0xBD },
            { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "agrave", 0xE0 }, { "aacute", 0xE1 }, { "uuml", 0xFC },
            { "ouml", 0xF6 }, { "auml", 0xE4 }, { "szlig", 0xDF }, { "ccedil", 0xE7 }, { "ntilde", 0xF1 },
            { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "shy", 0xAD },
            { "zwj", 0x200D }, { "zwnj", 0x200C }, { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "thinsp", 0x2009 },
        };
        for (const auto& e : kNamed)
            if (name == e.name)
            {
                AppendUtf8(out, e.cp);
                return out;
            }
        return {};
    }

    std::string AsciiLower(std::string_view s)
    {
        std::string out(s);
        for (auto& c : out)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return out;
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

    bool IsMonoFontName(std::string_view face)
    {
        const auto f = AsciiLower(face);
        for (const char* mono : { "consolas", "courier", "mono", "menlo", "monaco", "lucida console", "fixedsys" })
            if (f.find(mono) != std::string::npos)
                return true;
        return false;
    }

    std::string NewUuid()
    {
        GUID g{};
        CoCreateGuid(&g);
        return std::format("urn:uuid:{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", g.Data1, g.Data2, g.Data3, g.Data4[0],
                           g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    }

    std::string UtcNowIso()
    {
        SYSTEMTIME t{};
        GetSystemTime(&t);
        return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    }
}
