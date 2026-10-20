#pragma once
#include <format>
#include <string>
#include <string_view>

// Thread-safe file log in %LOCALAPPDATA%\ShiftDrop\logs\shiftdrop-YYYYMMDD.log (UTF-8).
// Never call from the mouse hook callback: it takes a lock and does file I/O.
namespace sd::log
{
    void Init();
    const std::wstring& AppDataFolder();   // %LOCALAPPDATA%\ShiftDrop
    const std::wstring& Folder();          // %LOCALAPPDATA%\ShiftDrop\logs
    void Write(std::wstring_view level, std::wstring_view text);

    template <class... Args>
    void Info(std::wformat_string<Args...> fmt, Args&&... args)
    {
        Write(L"INFO", std::format(fmt, std::forward<Args>(args)...));
    }
    template <class... Args>
    void Warn(std::wformat_string<Args...> fmt, Args&&... args)
    {
        Write(L"WARN", std::format(fmt, std::forward<Args>(args)...));
    }
    template <class... Args>
    void Error(std::wformat_string<Args...> fmt, Args&&... args)
    {
        Write(L"ERROR", std::format(fmt, std::forward<Args>(args)...));
    }
}
