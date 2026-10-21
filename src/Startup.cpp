#include "Startup.h"
#include "Brand.h"
#include "Log.h"
#include "PathUtil.h"
#include "Settings.h"
#include "Strings.h"

#include <windows.h>
#include <wil/resource.h>

#include <format>

namespace sd::startup
{
    namespace
    {
        constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        constexpr wchar_t kValue[] = SD_NAME;

        std::wstring Command()
        {
            return std::format(L"\"{}\" {}", ThisExe(), kAutostartArg);
        }

        bool ReadValue(std::wstring& out)
        {
            DWORD bytes = 0;
            if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
                return false;
            out.resize(bytes / sizeof(wchar_t));
            if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, out.data(), &bytes) != ERROR_SUCCESS)
                return false;
            out.resize(wcsnlen(out.c_str(), out.size()));
            return true;
        }
    }

    bool IsEnabled()
    {
        std::wstring value;
        return ReadValue(value);
    }

    bool PointsHere()
    {
        std::wstring value;
        return ReadValue(value) && EqualsNoCase(value, Command());
    }

    bool Enable()
    {
        const auto command = Command();
        const LSTATUS st = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kValue, REG_SZ, command.c_str(),
                                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        if (st != ERROR_SUCCESS)
            log::Warn(L"startup: could not write the Run value ({})", st);
        return st == ERROR_SUCCESS;
    }

    bool Disable()
    {
        const LSTATUS st = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValue);
        return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
    }

    bool Choice()
    {
        return GetPrivateProfileIntW(L"Startup", L"StartWithWindows", 1, SettingsPath().c_str()) != 0;
    }

    void SetChoice(bool on)
    {
        WritePrivateProfileStringW(L"Startup", L"StartWithWindows", on ? L"1" : L"0", SettingsPath().c_str());
        const bool ok = on ? Enable() : Disable();
        log::Info(L"startup: start with Windows turned {} ({})", on ? L"on" : L"off", ok ? L"ok" : L"failed");
    }

    void ApplyAtLaunch()
    {
        wchar_t saved[8]{};
        GetPrivateProfileStringW(L"Startup", L"StartWithWindows", L"", saved, ARRAYSIZE(saved), SettingsPath().c_str());
        if (!saved[0])
        {
            SetChoice(true);   // first run: on by default, as the spec asks; the tray menu turns it off
            return;
        }
        if (Choice() && !PointsHere())
        {
            // Rebuilt or copied somewhere else: make login start the exe the user is running now.
            log::Info(L"startup: updating the Run value to {}", Command());
            Enable();
        }
        else if (!Choice() && IsEnabled())
        {
            Disable();
        }
        log::Info(L"startup: start with Windows is {}", Choice() ? L"on" : L"off");
    }
}
