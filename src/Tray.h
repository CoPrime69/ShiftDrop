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

    // A submenu with one radio-checked choice a feature adds (shown under Style).
    struct TrayChoice
    {
        std::wstring text;                 // the submenu's name
        UINT baseId = 0;                   // item i sends baseId + i
        std::vector<std::wstring> items;
        int checked = 0;
    };

    // What features add to the tray menu.
    struct TrayExtras
    {
        bool shelf = false;                // "Show Float shelf"
        bool shelfOn = false;              // ...ticked: the shelf is switched on
        std::vector<TrayChoice> choices;
        std::vector<TrayToggle> toggles;
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
        void ShowMenu(POINT anchor, bool startWithWindows, const Appearance& appearance, const TrayExtras& extras = {});

    private:
        NOTIFYICONDATAW Base(UINT flags) const;
        HWND m_owner = nullptr;
        HICON m_icon = nullptr;   // owned by the App
        std::wstring m_tip;
        bool m_added = false;
    };
}
