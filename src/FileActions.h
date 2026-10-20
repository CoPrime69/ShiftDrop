#pragma once
#include <windows.h>

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
}
