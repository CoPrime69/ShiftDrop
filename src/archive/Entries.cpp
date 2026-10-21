#include "archive/Entries.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>

namespace sd::archive
{
    namespace
    {
        constexpr size_t kMaxPartLength = 255;   // NTFS's limit for one name
        constexpr size_t kMaxDepth = 128;

        bool IsForbidden(wchar_t c)
        {
            return c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'|' || c == L'?' || c == L'*';
        }

        // CON, PRN, AUX, NUL, COM0-9, LPT0-9 (also with an extension: "nul.txt" is the NUL device too).
        bool IsDeviceName(const std::wstring& part)
        {
            std::wstring base = part.substr(0, part.find(L'.'));
            while (!base.empty() && base.back() == L' ')
                base.pop_back();
            for (auto& c : base)
                c = static_cast<wchar_t>(std::towupper(c));
            static constexpr std::array kNames{ L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$" };
            for (const auto* n : kNames)
                if (base == n)
                    return true;
            if (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT")))
            {
                const wchar_t d = base[3];
                return (d >= L'0' && d <= L'9') || d == 0x00B9 || d == 0x00B2 || d == 0x00B3;   // also COM¹ COM² COM³
            }
            return false;
        }
    }

    bool SafeParts(std::wstring_view name, std::vector<std::wstring>& parts, std::wstring* why)
    {
        parts.clear();
        const auto refuse = [&](const wchar_t* reason) {
            if (why)
                *why = reason;
            parts.clear();
            return false;
        };
        std::wstring n(name);
        for (auto& c : n)
            if (c == L'\\')
                c = L'/';   // archives made on Windows sometimes use backslashes
        if (n.starts_with(L'/'))
            return refuse(L"an absolute path");
        if (n.size() >= 2 && n[1] == L':')
            return refuse(L"a drive letter");

        size_t start = 0;
        while (start <= n.size())
        {
            const size_t end = std::min(n.find(L'/', start), n.size());
            std::wstring part = n.substr(start, end - start);
            start = end + 1;
            if (part.empty() || part == L".")
                continue;
            if (part == L"..")
                return refuse(L"a \"..\" path that leads outside the folder");
            // Windows drops trailing dots and spaces from a name: ". ." or "..." would end up as "..".
            const bool dotted = part.find_first_not_of(L". ") == std::wstring::npos && std::count(part.begin(), part.end(), L'.') >= 2;
            while (!part.empty() && (part.back() == L'.' || part.back() == L' '))
                part.pop_back();
            if (part.empty())
            {
                if (dotted)
                    return refuse(L"a dotted name that Windows reads as \"..\"");
                part = L"_";
            }
            for (auto& c : part)
                if (IsForbidden(c))
                    c = L'_';   // includes ':' so no name can reach an alternate data stream
            if (IsDeviceName(part))
                part.insert(part.begin(), L'_');
            if (part.size() > kMaxPartLength)
            {
                // Keep the extension when a long name is cut.
                const auto dot = part.find_last_of(L'.');
                const std::wstring ext = dot != std::wstring::npos && part.size() - dot <= 16 ? part.substr(dot) : L"";
                part = part.substr(0, kMaxPartLength - ext.size()) + ext;
            }
            parts.push_back(std::move(part));
            if (parts.size() > kMaxDepth)
                return refuse(L"a path nested too deeply");
        }
        return true;
    }

    std::wstring JoinParts(const std::vector<std::wstring>& parts, wchar_t separator)
    {
        std::wstring out;
        for (const auto& p : parts)
        {
            if (!out.empty())
                out += separator;
            out += p;
        }
        return out;
    }

    std::wstring DecodeName(std::string_view raw, unsigned codePage)
    {
        if (raw.empty())
            return {};
        const int size = static_cast<int>(raw.size());
        // MB_ERR_INVALID_CHARS makes the UTF-8 attempt fail on anything that is not valid UTF-8.
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), size, nullptr, 0);
        UINT cp = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        if (n <= 0)
        {
            cp = codePage ? codePage : CP_OEMCP;
            flags = 0;
            n = MultiByteToWideChar(cp, 0, raw.data(), size, nullptr, 0);
        }
        if (n <= 0)
            return std::wstring(raw.begin(), raw.end());   // byte = code point, as a last resort
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(cp, flags, raw.data(), size, out.data(), n);
        return out;
    }

    std::wstring LongPath(const std::wstring& path)
    {
        if (path.starts_with(L"\\\\?\\"))
            return path;
        if (path.starts_with(L"\\\\"))
            return L"\\\\?\\UNC\\" + path.substr(2);
        return L"\\\\?\\" + path;
    }

    namespace
    {
        constexpr int64_t kUnixEpochTicks = 116444736000000000LL;   // 1970-01-01 in FILETIME ticks
    }

    int64_t TicksFromUnix(int64_t seconds, long nanoseconds)
    {
        return kUnixEpochTicks + seconds * 10'000'000 + nanoseconds / 100;
    }

    void UnixFromTicks(int64_t ticks, int64_t& seconds, long& nanoseconds)
    {
        const int64_t since = ticks - kUnixEpochTicks;
        seconds = since / 10'000'000;
        int64_t rest = since % 10'000'000;
        if (rest < 0)
        {
            rest += 10'000'000;
            --seconds;
        }
        nanoseconds = static_cast<long>(rest * 100);
    }
}
