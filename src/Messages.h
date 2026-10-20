#pragma once
#include <windows.h>
#include <windowsx.h>

// Private messages posted to the hidden main window. The WM_APP range is reserved for the
// application itself, so nothing else in the system sends these.
namespace sd::msg
{
    constexpr UINT DragBegin = WM_APP + 1;       // lParam: cursor (screen px) when the drag threshold was crossed
    constexpr UINT DragArm = WM_APP + 2;         // wParam: trigger VK, lParam: cursor (screen px)
    constexpr UINT DragEnd = WM_APP + 3;         // lParam: cursor (screen px) at button-up
    constexpr UINT TrayCallback = WM_APP + 10;   // Shell_NotifyIcon callback (NOTIFYICON_VERSION_4 layout)
    constexpr UINT BatchDone = WM_APP + 20;      // lParam: BatchResult*, ownership passes to the receiver
    constexpr UINT RunDrop = WM_APP + 21;        // a drop queued by the overlay, handled after OLE's Drop returns
    constexpr UINT AnotherInstance = WM_APP + 30;
    constexpr UINT QuitRequest = WM_APP + 31;      // "ShiftDrop.exe --quit" (the installer) asks the running copy to exit

    // Screen coordinates can be negative on multi-monitor setups but always fit in 16 bits,
    // so they travel in an LPARAM like WM_MOUSEMOVE's. Unpack with GET_X_LPARAM / GET_Y_LPARAM.
    inline LPARAM PackPoint(POINT pt) noexcept
    {
        return MAKELPARAM(static_cast<SHORT>(pt.x), static_cast<SHORT>(pt.y));
    }
    inline POINT UnpackPoint(LPARAM lp) noexcept
    {
        return POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    }
}
