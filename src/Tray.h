#pragma once
#include "Settings.h"

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <string_view>
#include <vector>

namespace sd
{
    // A checkable tray-menu entry a feature adds (shown above "Start with Windows").
    struct TrayToggle
    {
        UINT id = 0;
        std::wstring text;
        bool checked = false;
    };

    // The notification-area icon, its context menu and balloon notifications.
    // On Windows 10/11 a balloon (NIF_INFO) is shown as a toast attributed to this app.
    class Tray
    {
    public:
        bool Add(HWND owner, HICON icon, std::wstring_view tip);
        bool Readd();   // after Explorer restarts (the "TaskbarCreated" broadcast)
        void Remove();
        void SetTip(std::wstring_view tip);
        void Balloon(std::wstring_view title, std::wstring_view text, DWORD infoFlags = NIIF_INFO);
        // WM_COMMAND with IDM_* goes to the owner window.
        void ShowMenu(POINT anchor, bool startWithWindows, const Appearance& appearance, const std::vector<TrayToggle>& toggles = {});

    private:
        NOTIFYICONDATAW Base(UINT flags) const;
        HWND m_owner = nullptr;
        HICON m_icon = nullptr;   // owned by the App
        std::wstring m_tip;
        bool m_added = false;
    };
}
