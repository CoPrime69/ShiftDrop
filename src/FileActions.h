#pragma once
#include <windows.h>
#include <wil/resource.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sd
{
    // The instant file actions of the Alt wheel. They run on the UI thread (the clipboard belongs to a
    // window) and write nothing to disk. file.duplicate and file.move are not here: they change files,
    // so they run on workers and get a history entry and undo (PipelineRunner, FileMove).
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

    // A CF_HDROP block (DROPFILES followed by the wide paths), as the clipboard and drag and drop
    // expect it. Null if out of memory.
    wil::unique_hglobal MakeDropFiles(const std::vector<std::wstring>& files);
}
