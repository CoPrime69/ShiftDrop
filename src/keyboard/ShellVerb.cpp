#include "keyboard/ShellVerb.h"
#include "Brand.h"
#include "Log.h"

#include <shlobj.h>
#include <wil/resource.h>

#include <format>

namespace sd::shellverb
{
    namespace
    {
        constexpr const wchar_t* kVerbKeys[] = { L"*\\shell\\" SD_NAME, L"Directory\\shell\\" SD_NAME };

        // HKCU\Software\Classes when the caller gave no root. Writing there (not HKEY_CLASSES_ROOT,
        // which merges in HKLM) keeps everything per user.
        wil::unique_hkey OpenClasses(HKEY classes)
        {
            wil::unique_hkey key;
            if (classes)
                RegOpenKeyExW(classes, nullptr, 0, KEY_READ | KEY_WRITE, &key);   // a handle of our own to the same key
            else
                RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr);
            return key;
        }

        bool SetString(HKEY key, const wchar_t* name, const std::wstring& value)
        {
            return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                  static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        }

        void Changed(HKEY classes)
        {
            // Explorer caches verbs; this tells it the associations changed. Only for the real key.
            if (!classes)
                SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        }
    }

    std::wstring CurrentExe()
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (n < path.size())
            {
                path.resize(n);
                return path;
            }
            path.resize(path.size() * 2);
        }
    }

    std::wstring CommandFor(const std::wstring& exe)
    {
        return std::format(L"\"{}\" --wheel \"%1\"", exe);
    }

    bool Register(const std::wstring& exe, HKEY classes)
    {
        auto root = OpenClasses(classes);
        if (!root)
            return false;
        bool ok = true;
        for (const wchar_t* path : kVerbKeys)
        {
            wil::unique_hkey verb, command;
            if (RegCreateKeyExW(root.get(), path, 0, nullptr, 0, KEY_WRITE, nullptr, &verb, nullptr) != ERROR_SUCCESS ||
                RegCreateKeyExW(verb.get(), L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &command, nullptr) != ERROR_SUCCESS)
            {
                ok = false;
                continue;
            }
            const std::wstring label = SD_NAME L"…";
            ok &= SetString(verb.get(), nullptr, label);
            ok &= SetString(verb.get(), L"MUIVerb", label);
            ok &= SetString(verb.get(), L"Icon", std::format(L"\"{}\",0", exe));
            ok &= SetString(verb.get(), L"MultiSelectModel", L"Player");
            ok &= SetString(command.get(), nullptr, CommandFor(exe));
        }
        Changed(classes);
        log::Info(L"shell: right-click entry {} for {}", ok ? L"registered" : L"partly registered", exe);
        return ok;
    }

    bool Unregister(HKEY classes)
    {
        auto root = OpenClasses(classes);
        if (!root)
            return false;
        bool ok = true;
        for (const wchar_t* path : kVerbKeys)
        {
            const LSTATUS s = RegDeleteTreeW(root.get(), path);
            ok &= s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
        }
        Changed(classes);
        log::Info(L"shell: right-click entry removed{}", ok ? L"" : L" (with errors)");
        return ok;
    }

    bool IsRegistered(HKEY classes)
    {
        auto root = OpenClasses(classes);
        if (!root)
            return false;
        wchar_t value[2]{};
        DWORD size = sizeof(value);
        // Any command at all counts: ERROR_MORE_DATA means one is there.
        const LSTATUS s = RegGetValueW(root.get(), L"*\\shell\\" SD_NAME L"\\command", nullptr, RRF_RT_REG_SZ, nullptr, value, &size);
        return s == ERROR_SUCCESS || s == ERROR_MORE_DATA;
    }
}
