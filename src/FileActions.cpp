#include "FileActions.h"
#include "Clipboard.h"
#include "Log.h"

#include <shellapi.h>
#include <shlobj.h>
#include <wil/resource.h>

#include <cwctype>
#include <filesystem>
#include <format>
#include <map>

namespace sd
{
    namespace
    {
        std::wstring FolderOf(const std::wstring& path)
        {
            if (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY)
                return path;
            return std::filesystem::path(path).parent_path().wstring();
        }

        bool Launch(const TerminalLaunch& t)
        {
            SHELLEXECUTEINFOW sei{ sizeof(sei) };
            sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            sei.lpFile = t.exe.c_str();
            sei.lpParameters = t.args.c_str();
            sei.lpDirectory = t.folder.c_str();
            sei.nShow = SW_SHOWNORMAL;
            return ShellExecuteExW(&sei) != FALSE;
        }

        struct TerminalSetting
        {
            TerminalChoice choice = TerminalChoice::WindowsTerminal;
            std::wstring custom;
        };
        TerminalSetting& Terminal()
        {
            static TerminalSetting setting;   // UI thread only
            return setting;
        }

        std::wstring_view Trim(std::wstring_view s)
        {
            while (!s.empty() && std::iswspace(s.front()))
                s.remove_prefix(1);
            while (!s.empty() && std::iswspace(s.back()))
                s.remove_suffix(1);
            return s;
        }
    }

    std::vector<TerminalLaunch> TerminalCommands(TerminalChoice choice, std::wstring_view custom, const std::wstring& folder)
    {
        // A trailing backslash ("C:\") would escape the closing quote on the command line.
        const auto quotable = folder.ends_with(L'\\') ? folder + L"." : folder;
        std::vector<TerminalLaunch> out;
        const auto powershell = [&] {
            out.push_back({ L"pwsh.exe", L"-NoExit", folder });
            out.push_back({ L"powershell.exe", L"-NoExit", folder });
        };
        const auto command = Trim(custom);
        if (choice == TerminalChoice::Custom && !command.empty())
        {
            // The program is the first word, or the quoted part ("C:\Program Files\...\app.exe").
            std::wstring_view program, rest;
            if (command.front() == L'"')
            {
                const size_t close = command.find(L'"', 1);
                program = close == std::wstring_view::npos ? command.substr(1) : command.substr(1, close - 1);
                rest = close == std::wstring_view::npos ? std::wstring_view{} : command.substr(close + 1);
            }
            else
            {
                const size_t space = command.find_first_of(L" \t");
                program = command.substr(0, space);
                rest = space == std::wstring_view::npos ? std::wstring_view{} : command.substr(space);
            }
            std::wstring args(Trim(rest));
            constexpr std::wstring_view kPlaceholder = L"{folder}";
            for (size_t at = args.find(kPlaceholder); at != std::wstring::npos; at = args.find(kPlaceholder, at))
            {
                const bool quoted = at > 0 && args[at - 1] == L'"';
                const auto value = quoted ? quotable : L"\"" + quotable + L"\"";
                args.replace(at, kPlaceholder.size(), value);
                at += value.size();
            }
            if (!Trim(program).empty())
            {
                out.push_back({ std::wstring(Trim(program)), std::move(args), folder });
                return out;
            }
        }
        switch (choice)
        {
        case TerminalChoice::PowerShell:
            powershell();
            break;
        case TerminalChoice::CommandPrompt:
            out.push_back({ L"cmd.exe", L"/K", folder });
            break;
        default:
            // Windows Terminal if installed (wt.exe is an app execution alias), else PowerShell.
            out.push_back({ L"wt.exe", std::format(L"-d \"{}\"", quotable), folder });
            powershell();
            break;
        }
        return out;
    }

    void SetTerminalChoice(TerminalChoice choice, std::wstring custom)
    {
        Terminal() = TerminalSetting{ choice, std::move(custom) };
    }

    wil::unique_hglobal MakeDropFiles(const std::vector<std::wstring>& files)
    {
        size_t chars = 1;   // final extra terminator
        for (const auto& f : files)
            chars += f.size() + 1;
        wil::unique_hglobal drop(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t)));
        if (!drop)
            return drop;
        auto* df = static_cast<DROPFILES*>(GlobalLock(drop.get()));
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        auto* p = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
        for (const auto& f : files)
        {
            std::copy(f.begin(), f.end(), p);
            p += f.size() + 1;   // GMEM_ZEROINIT supplies each terminator and the final one
        }
        GlobalUnlock(drop.get());
        return drop;
    }

    namespace
    {
        std::map<std::string, InstantFileAction, std::less<>>& ModuleActions()
        {
            static std::map<std::string, InstantFileAction, std::less<>> actions;   // UI thread only
            return actions;
        }
    }

    void SetInstantFileAction(std::string actionId, InstantFileAction run)
    {
        if (run)
            ModuleActions()[std::move(actionId)] = std::move(run);
        else
            ModuleActions().erase(actionId);
    }

    FileActionResult RunInstantFileAction(std::string_view actionId, const std::vector<std::wstring>& files, HWND owner)
    {
        FileActionResult r;
        if (files.empty())
            return r;
        const auto count = files.size() == 1 ? std::filesystem::path(files[0]).filename().wstring()
                                              : std::format(L"{} files", files.size());

        if (const auto it = ModuleActions().find(actionId); it != ModuleActions().end())
        {
            r = it->second(files, owner);
        }
        else if (actionId == "file.copy")
        {
            r.ok = clipboard::SetFiles(owner, files, DROPEFFECT_COPY);
            r.message = r.ok ? std::format(L"Copied {}. Paste in Explorer.", count) : L"Could not open the clipboard.";
        }
        else if (actionId == "file.copy_path")
        {
            // Same format as Explorer's "Copy as path": quoted, one per line.
            std::wstring text;
            for (const auto& f : files)
                text += (text.empty() ? L"" : L"\r\n") + std::format(L"\"{}\"", f);
            r.ok = clipboard::SetText(owner, text);
            r.message = r.ok ? std::format(L"Copied the path of {}.", count) : L"Could not open the clipboard.";
        }
        else if (actionId == "file.terminal")
        {
            const auto folder = FolderOf(files[0]);
            const auto& setting = Terminal();
            for (const auto& t : TerminalCommands(setting.choice, setting.custom, folder))
                if ((r.ok = Launch(t)))
                    break;
            r.message = r.ok ? std::format(L"Opened a terminal in {}.", folder) : L"Could not start a terminal.";
        }
        else
        {
            r.message = L"Unknown file action.";
        }
        log::Info(L"file action {}: {}", std::wstring(actionId.begin(), actionId.end()), r.message);
        return r;
    }
}
