#include "PathUtil.h"
#include "Strings.h"

#include <windows.h>

#include <filesystem>

namespace sd
{
    std::wstring FileName(std::wstring_view path)
    {
        return std::filesystem::path(path).filename().wstring();
    }

    bool PathExists(const std::wstring& path)
    {
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    bool IsFolder(const std::wstring& path)
    {
        const DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool IsFile(const std::wstring& path)
    {
        const DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    uint64_t FileSize(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            return 0;
        return FileSizeOf(data);
    }

    bool IsWithin(std::wstring_view path, std::wstring_view folder, bool allowSame)
    {
        if (folder.empty() || path.size() < folder.size() || !EqualsNoCase(path.substr(0, folder.size()), folder))
            return false;
        if (path.size() == folder.size())
            return allowSame;
        const auto slash = [](wchar_t c) { return c == L'\\' || c == L'/'; };
        return slash(folder.back()) || slash(path[folder.size()]);   // "C:\" holds "C:\x"; "C:\a" does not hold "C:\ab"
    }

    std::wstring ThisExe()
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (n == 0)
                return {};
            if (n < path.size())
            {
                path.resize(n);
                return path;
            }
            path.resize(path.size() * 2);   // truncated: a long path
        }
    }

    std::wstring EngineExe(const wchar_t* envVar, std::wstring_view name)
    {
        if (const DWORD n = GetEnvironmentVariableW(envVar, nullptr, 0); n > 1)
        {
            std::wstring env(n, L'\0');
            env.resize(GetEnvironmentVariableW(envVar, env.data(), n));
            return PathExists(env) ? env : std::wstring();
        }
        const auto self = ThisExe();
        if (self.empty())
            return {};
        const auto exe = (std::filesystem::path(self).parent_path() / L"engines" / name).wstring();
        return PathExists(exe) ? exe : std::wstring();
    }
}
