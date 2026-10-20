#pragma once
#include <windows.h>

// Global low-level mouse hook (WH_MOUSE_LL).
//
// The hook is installed on the UI thread. Windows calls a low-level hook *through the installing
// thread's message loop*, so the UI thread must keep pumping messages and must never block for
// long, otherwise every mouse move on the desktop stalls until LowLevelHooksTimeout expires and
// Windows silently removes the hook (see the watchdog in main.cpp).
//
// The callback only records state in plain globals and PostMessage()s to the main window:
//   DragBegin  primary button held and moved past SM_CXDRAG/SM_CYDRAG
//   DragArm    ...and a trigger modifier is down (GetAsyncKeyState), once per press
//   DragEnd    primary button released after a drag
// No allocation, no locks, no I/O inside the callback.
namespace sd::hook
{
    // keys: up to three trigger VKs (0 = unused). DragArm's wParam says which one was pressed.
    bool Install(HWND notify, const UINT (&keys)[3]);
    void Uninstall();
    bool Reinstall();          // used by the watchdog when the hook appears to have been dropped
    bool IsInstalled();

    // All of these are only touched on the UI thread (the hook runs on it too), so no atomics needed.
    bool IsDragging();         // primary button down and moved past the drag threshold
    bool IsArmed();            // DragArm already posted for this press
    void SetArmed(bool armed); // the UI marks/clears the arm, e.g. modifier pressed while the mouse is still
    DWORD LastEventTime();     // MSLLHOOKSTRUCT::time of the last event seen (GetTickCount base)
    POINT LastPoint();
}
