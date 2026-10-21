#pragma once
#include <windows.h>

// Helpers for code that runs on the UI thread.
namespace sd
{
    // Brings `hwnd` to the front from a drop. A process that did not receive the last input event
    // cannot take the foreground, and a drop comes through Explorer's drag loop, not our input queue:
    // attaching to the foreground thread's input for a moment lets the window (or a dialog it owns)
    // open in front instead of flashing in the taskbar.
    inline void BringToFront(HWND hwnd)
    {
        const HWND fg = GetForegroundWindow();
        const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        const DWORD self = GetCurrentThreadId();
        const bool attach = fgThread && fgThread != self && AttachThreadInput(self, fgThread, TRUE);
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        if (attach)
            AttachThreadInput(self, fgThread, FALSE);
    }
}
