#pragma once
#include <string>

// Start with Windows: a value under HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
//
// HKCU needs no elevation and applies to this user only. Windows' own switch for it (Task Manager >
// Startup apps, or Settings > Apps > Startup) is stored separately under ...\Explorer\StartupApproved,
// which we never touch, so turning it off there keeps working.
namespace sd::startup
{
    inline constexpr wchar_t kAutostartArg[] = L"--autostart";   // tells the app it was started at login

    bool IsEnabled();                 // our Run value exists
    bool PointsHere();                // ...and names the exe that is running now
    bool Enable();                    // writes "<this exe>" --autostart
    bool Disable();

    // At every launch: on the very first run turn it on (the default), and later keep the Run value
    // pointing at this exe if it was moved or rebuilt elsewhere. The choice lives in settings.ini
    // ([Startup] StartWithWindows=1/0) so a deliberate "off" is never turned back on.
    void ApplyAtLaunch();
    void SetChoice(bool on);          // the tray toggle: saves the choice and updates the Run value
    bool Choice();                    // the saved choice (true when never saved)
}
