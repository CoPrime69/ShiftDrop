#pragma once
#include "toast/ToastXml.h"

#include <functional>

// Real toasts for finished batches (Windows.UI.Notifications, C++/WinRT from the Windows SDK; no
// Windows App SDK). An unpackaged app gets them only through its Start-menu shortcut, which carries
// the AppUserModelID and the COM class that a click starts (both set by the installer). Without
// that shortcut (a portable copy) Start() says no and the app keeps using balloon notifications.
//
// A click (the toast or a button) reaches the running copy through that COM class:
// INotificationActivationCallback, registered while the app runs. If the app has quit since, COM
// starts ShiftDrop.exe with kActivatedArg; ServeActivation() then handles that one click and exits.
// Implemented in WinToasts.cpp. UI thread (an STA) only.
namespace sd::toast
{
    using Activated = std::function<void(const Activation&)>;

    // Checks the shortcut, sets the process's AppUserModelID, points the COM class at this exe (per
    // user, HKCU) and registers it. False: toasts can't be shown here; use the balloon.
    bool Start(Activated onActivated);
    // False when the toast could not be shown (not started, notifications off for the app...).
    bool Show(const Content& content);
    void Stop();

    // The command line COM uses to start ShiftDrop for a click (it adds "-Embedding" to it).
    inline constexpr wchar_t kActivatedArg[] = L"-ToastActivated";
    // Started by COM for a click: registers the class, waits for that click (at most 30 seconds),
    // runs `onActivated` and returns the exit code.
    int ServeActivation(Activated onActivated);
    // "--uninstall": removes the COM class's registry key.
    void Unregister();
}
