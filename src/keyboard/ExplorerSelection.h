#pragma once
#include <windows.h>

#include <string>
#include <vector>

// The files selected in File Explorer, for the hotkey that opens the wheel without a drag.
namespace sd::explorer
{
    // A File Explorer window (any tab) or the desktop.
    bool IsExplorerWindow(HWND window);

    // The file-system paths selected in `window` (the foreground window when the hotkey was
    // pressed): in an Explorer window its active tab, on the desktop the desktop's selection.
    // Empty when nothing (or nothing with a path) is selected.
    //
    // Goes through the shell's automation objects (IShellWindows -> the window's IShellBrowser ->
    // its active view's IFolderView2 -> the selection). Those are cross-process COM calls into
    // explorer.exe: make them from a worker STA thread, never the UI thread.
    std::vector<std::wstring> SelectionOf(HWND window);
}
