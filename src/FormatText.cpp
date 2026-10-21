#include "FormatText.h"

#include <algorithm>
#include <format>

namespace sd
{
    std::wstring SizeText(uint64_t bytes)
    {
        if (bytes < 1024)
            return bytes == 1 ? std::wstring(L"1 byte") : std::format(L"{} bytes", bytes);
        const double kb = static_cast<double>(bytes) / 1024.0;
        if (kb < 1000.0)
            return std::format(L"{:.0f} KB", kb);
        const double mb = kb / 1024.0;
        if (mb < 1000.0)
            return mb < 10.0 ? std::format(L"{:.1f} MB", mb) : std::format(L"{:.0f} MB", mb);
        return std::format(L"{:.1f} GB", mb / 1024.0);
    }

    std::wstring ExactSizeText(uint64_t bytes)
    {
        return bytes < 1024 ? SizeText(bytes) : std::format(L"{} ({} bytes)", SizeText(bytes), Thousands(bytes));
    }

    std::wstring Thousands(uint64_t n)
    {
        std::wstring digits = std::to_wstring(n), out;
        for (size_t i = 0; i < digits.size(); ++i)
        {
            if (i > 0 && (digits.size() - i) % 3 == 0)
                out += L',';
            out += digits[i];
        }
        return out;
    }

    std::wstring Timecode(int64_t us, bool padHours)
    {
        const int64_t ms = (std::max<int64_t>(us, 0) + 500) / 1000;
        return padHours ? std::format(L"{:02}:{:02}:{:02}.{:03}", ms / 3'600'000, ms / 60'000 % 60, ms / 1000 % 60, ms % 1000)
                        : std::format(L"{}:{:02}:{:02}.{:03}", ms / 3'600'000, ms / 60'000 % 60, ms / 1000 % 60, ms % 1000);
    }
}
