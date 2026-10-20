#include "FileActions.h"
#include "Log.h"

#include <shellapi.h>
#include <shlobj.h>
#include <wil/resource.h>

#include <filesystem>
#include <format>

namespace sd
{
    namespace
    {
        // Puts files on the clipboard the way Explorer does: CF_HDROP plus "Preferred DropEffect", which
        // tells Explorer's Paste whether to copy or move. We always say copy.
        bool PutFilesOnClipboard(const std::vector<std::wstring>& files, HWND owner, DWORD effect)
        {
            size_t chars = 1;   // final extra terminator
            for (const auto& f : files)
                chars += f.size() + 1;
            const size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);

            wil::unique_hglobal drop(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes));
            wil::unique_hglobal effectMem(GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD)));
            if (!drop || !effectMem)
                return false;
            {
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
                *static_cast<DWORD*>(GlobalLock(effectMem.get())) = effect;
                GlobalUnlock(effectMem.get());
            }

            if (!OpenClipboard(owner))
                return false;
            auto close = wil::scope_exit([] { CloseClipboard(); });
            EmptyClipboard();
            static const UINT kPreferredEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
            // On success the clipboard owns the memory, so release our ownership only then.
            if (!SetClipboardData(CF_HDROP, drop.get()))
                return false;
            drop.release();
            if (SetClipboardData(kPreferredEffect, effectMem.get()))
                effectMem.release();
            return true;
        }

        bool PutTextOnClipboard(const std::wstring& text, HWND owner)
        {
            wil::unique_hglobal mem(GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t)));
            if (!mem)
                return false;
            memcpy(GlobalLock(mem.get()), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem.get());
            if (!OpenClipboard(owner))
                return false;
            auto close = wil::scope_exit([] { CloseClipboard(); });
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, mem.get()))
                return false;
            mem.release();
            return true;
        }

        std::wstring FolderOf(const std::wstring& path)
        {
            if (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY)
                return path;
            return std::filesystem::path(path).parent_path().wstring();
        }

        bool Launch(const wchar_t* exe, const std::wstring& args, const std::wstring& folder)
        {
            SHELLEXECUTEINFOW sei{ sizeof(sei) };
            sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            sei.lpFile = exe;
            sei.lpParameters = args.c_str();
            sei.lpDirectory = folder.c_str();
            sei.nShow = SW_SHOWNORMAL;
            return ShellExecuteExW(&sei) != FALSE;
        }
    }

    FileActionResult RunInstantFileAction(std::string_view actionId, const std::vector<std::wstring>& files, HWND owner)
    {
        FileActionResult r;
        if (files.empty())
            return r;
        const auto count = files.size() == 1 ? std::filesystem::path(files[0]).filename().wstring()
                                              : std::format(L"{} files", files.size());

        if (actionId == "file.copy")
        {
            r.ok = PutFilesOnClipboard(files, owner, DROPEFFECT_COPY);
            r.message = r.ok ? std::format(L"Copied {}. Paste in Explorer.", count) : L"Could not open the clipboard.";
        }
        else if (actionId == "file.copy_path")
        {
            // Same format as Explorer's "Copy as path": quoted, one per line.
            std::wstring text;
            for (const auto& f : files)
                text += (text.empty() ? L"" : L"\r\n") + std::format(L"\"{}\"", f);
            r.ok = PutTextOnClipboard(text, owner);
            r.message = r.ok ? std::format(L"Copied the path of {}.", count) : L"Could not open the clipboard.";
        }
        else if (actionId == "file.terminal")
        {
            const auto folder = FolderOf(files[0]);
            // Windows Terminal if installed (wt.exe is an app execution alias), else PowerShell.
            // A trailing backslash ("C:\") would escape the closing quote on the command line.
            const auto quoted = folder.ends_with(L'\\') ? folder + L"." : folder;
            r.ok = Launch(L"wt.exe", std::format(L"-d \"{}\"", quoted), folder) ||
                   Launch(L"powershell.exe", L"-NoExit", folder);
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
