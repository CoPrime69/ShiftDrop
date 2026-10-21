#include "TempPath.h"
#include "Brand.h"

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <random>

namespace sd
{
    std::wstring TempDirectory()
    {
        wchar_t temp[MAX_PATH + 1]{};
        const DWORD n = GetTempPathW(MAX_PATH + 1, temp);
        return n == 0 || n > MAX_PATH ? std::wstring() : std::wstring(temp, n);
    }

    std::wstring TempFilePath(std::wstring_view tag, std::wstring_view extension)
    {
        GUID g{};
        CoCreateGuid(&g);
        return std::format(L"{}" SD_NAME L"-{}-{:08x}{:04x}{}", TempDirectory(), tag, g.Data1, g.Data2, extension);
    }

    TempFolder::TempFolder()
    {
        const auto temp = TempDirectory();
        if (temp.empty())
            return;
        static std::atomic<unsigned> counter{ 0 };
        thread_local std::mt19937 rng{ std::random_device{}() };
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const auto candidate = std::format(L"{}" SD_NAME L"-{}-{}-{:06x}", temp, GetCurrentProcessId(), counter++, rng() & 0xFFFFFF);
            if (CreateDirectoryW(candidate.c_str(), nullptr))
            {
                m_path = candidate;
                return;
            }
        }
    }

    TempFolder::~TempFolder()
    {
        if (m_path.empty())
            return;
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
}
