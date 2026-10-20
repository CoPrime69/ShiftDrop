#include "Brand.h"
#include "Log.h"

#include <windows.h>
#include <shlobj.h>
#include <wil/resource.h>

#include <mutex>

namespace sd::log
{
    namespace
    {
        std::mutex g_mutex;
        std::wstring g_appData;
        std::wstring g_folder;
        wil::unique_hfile g_file;
        WORD g_fileDay = 0;   // day-of-month the open file belongs to; 0 = none open

        std::wstring LocalAppData()
        {
            wil::unique_cotaskmem_string path;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path)))
                return L".";
            return path.get();
        }

        void OpenForToday(const SYSTEMTIME& now)
        {
            const auto name = std::format(L"{}\\shiftdrop-{:04}{:02}{:02}.log", g_folder, now.wYear, now.wMonth, now.wDay);
            // FILE_APPEND_DATA without FILE_WRITE_DATA makes every WriteFile an atomic append.
            // FILE_SHARE_READ | FILE_SHARE_WRITE lets you `Get-Content -Wait` the log while we run.
            g_file.reset(CreateFileW(name.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            g_fileDay = g_file ? now.wDay : 0;
        }
    }

    void Init()
    {
        std::scoped_lock lock(g_mutex);
        g_appData = LocalAppData() + L"\\" SD_APPDATA_FOLDER;
        g_folder = g_appData + L"\\logs";
        CreateDirectoryW(g_appData.c_str(), nullptr);
        CreateDirectoryW(g_folder.c_str(), nullptr);
    }

    const std::wstring& AppDataFolder() { return g_appData; }
    const std::wstring& Folder() { return g_folder; }

    void Write(std::wstring_view level, std::wstring_view text)
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const auto line = std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{:5}] [t{}] {}\r\n",
                                      now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                                      now.wMilliseconds, level, GetCurrentThreadId(), text);
        OutputDebugStringW(line.c_str());

        const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
        if (bytes <= 0)
            return;
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(), bytes, nullptr, nullptr);

        std::scoped_lock lock(g_mutex);
        if (g_folder.empty())
            return;
        if (g_fileDay != now.wDay)
            OpenForToday(now);
        if (g_file)
        {
            DWORD written = 0;
            WriteFile(g_file.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }
}
