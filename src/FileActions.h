#pragma once
#include "Settings.h"

#include <windows.h>
#include <wil/resource.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sd
{
    // The instant file actions of the File actions wheel. They run on the UI thread (the clipboard belongs to a
    // window) and write nothing to disk. file.duplicate, file.move and file.rename are not here: they
    // change files, so they run on workers and get a history entry and undo (PipelineRunner, FileMove).
    //
    // Returns a one-line message for the notification, or an error message with ok = false.
    struct FileActionResult
    {
        bool ok = false;
        std::wstring message;
    };
    FileActionResult RunInstantFileAction(std::string_view actionId, const std::vector<std::wstring>& files, HWND owner);

    // A feature module's own instant file action (the Float shelf's "file.float"), run by
    // RunInstantFileAction on the UI thread. An empty function removes it.
    using InstantFileAction = std::function<FileActionResult(const std::vector<std::wstring>& files, HWND owner)>;
    void SetInstantFileAction(std::string actionId, InstantFileAction run);

    // File actions › Terminal, as chosen in Settings: one program to start in a folder.
    struct TerminalLaunch
    {
        std::wstring exe;      // "wt.exe", or the custom command's program
        std::wstring args;
        std::wstring folder;   // its working folder
    };
    // What to try, in order, to open a terminal in `folder`: Windows Terminal falls back to PowerShell,
    // PowerShell is PowerShell 7 (pwsh) when installed. A custom command line has its {folder}
    // replaced by the folder (quoted, unless the command already quotes it); an empty one means
    // Windows Terminal.
    std::vector<TerminalLaunch> TerminalCommands(TerminalChoice choice, std::wstring_view custom, const std::wstring& folder);
    // The choice RunInstantFileAction uses. UI thread.
    void SetTerminalChoice(TerminalChoice choice, std::wstring custom);

    // A CF_HDROP block (DROPFILES followed by the wide paths), as the clipboard and drag and drop
    // expect it. Null if out of memory.
    wil::unique_hglobal MakeDropFiles(const std::vector<std::wstring>& files);
}
