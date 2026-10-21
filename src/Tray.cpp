#include "Brand.h"
#include "Tray.h"
#include "Log.h"
#include "Messages.h"
#include "resource.h"

#include <wil/resource.h>

#include <algorithm>

namespace sd
{
    namespace
    {
        // Identify the icon by (hwnd, uID) rather than by GUID: a GUID-registered icon is bound to the
        // exe's path, and running a second build from another folder would then fail to add it.
        constexpr UINT kIconId = 1;

        template <size_t N>
        void Copy(wchar_t (&dst)[N], std::wstring_view src)
        {
            const size_t n = std::min(src.size(), N - 1);
            std::copy_n(src.data(), n, dst);
            dst[n] = L'\0';
        }
    }

    NOTIFYICONDATAW Tray::Base(UINT flags) const
    {
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = m_owner;
        nid.uID = kIconId;
        nid.uFlags = flags;
        return nid;
    }

    bool Tray::Add(HWND owner, HICON icon, std::wstring_view tip)
    {
        m_owner = owner;
        m_icon = icon;
        m_tip = tip;
        return Readd();
    }

    bool Tray::Readd()
    {
        auto nid = Base(NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
        nid.uCallbackMessage = msg::TrayCallback;
        nid.hIcon = m_icon;
        Copy(nid.szTip, m_tip);
        Shell_NotifyIconW(NIM_DELETE, &nid);   // harmless if not present
        m_added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        if (m_added)
        {
            // Version 4: the callback's LOWORD(lParam) is the event (WM_CONTEXTMENU, NIN_BALLOONUSERCLICK...)
            // and wParam carries the anchor point.
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
        }
        else
        {
            log::Warn(L"tray: NIM_ADD failed (Explorer not ready yet?)");
        }
        return m_added;
    }

    void Tray::Remove()
    {
        if (!m_added)
            return;
        auto nid = Base(0);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        m_added = false;
    }

    void Tray::SetTip(std::wstring_view tip)
    {
        m_tip = tip;
        auto nid = Base(NIF_TIP | NIF_SHOWTIP);
        Copy(nid.szTip, m_tip);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void Tray::Balloon(std::wstring_view title, std::wstring_view text, DWORD infoFlags)
    {
        auto nid = Base(NIF_INFO);
        Copy(nid.szInfoTitle, title);   // 63 chars max
        Copy(nid.szInfo, text);         // 255 chars max
        nid.dwInfoFlags = infoFlags;
        if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
            log::Warn(L"tray: balloon failed");
    }

    void Tray::ShowMenu(POINT anchor, bool startWithWindows, const Appearance& appearance, const TrayExtras& extras)
    {
        wil::unique_hmenu menu(CreatePopupMenu());
        if (!menu)
            return;
        AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, L"" SD_NAME L": drag a file, press Shift, scroll for sections");
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu.get(), MF_STRING, IDM_SETTINGS, L"Settings\x2026");
        AppendMenuW(menu.get(), MF_STRING, IDM_HISTORY, L"History\x2026");
        if (extras.shelf)
            AppendMenuW(menu.get(), MF_STRING | (extras.shelfOn ? MF_CHECKED : MF_UNCHECKED), IDM_SHOW_SHELF, L"Show Float shelf");
        AppendMenuW(menu.get(), MF_STRING, IDM_OPEN_RECIPES, L"Open recipes folder");
        AppendMenuW(menu.get(), MF_STRING, IDM_OPEN_LOGS, L"Open logs folder");
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        // Size and Style submenus, each with a radio-style check on the current choice.
        wil::unique_hmenu size(CreatePopupMenu()), style(CreatePopupMenu());
        const wchar_t* sizes[] = { L"Auto (fits the screen)", L"Small", L"Medium", L"Large" };
        for (int i = 0; i < 4; ++i)
            AppendMenuW(size.get(), MF_STRING, IDM_SIZE_BASE + i, sizes[i]);
        CheckMenuRadioItem(size.get(), IDM_SIZE_BASE, IDM_SIZE_BASE + 3, IDM_SIZE_BASE + static_cast<UINT>(appearance.size), MF_BYCOMMAND);
        AppendMenuW(style.get(), MF_STRING, IDM_STYLE_BASE + 0, L"Solid");
        AppendMenuW(style.get(), MF_STRING, IDM_STYLE_BASE + 1, L"Glass");
        CheckMenuRadioItem(style.get(), IDM_STYLE_BASE, IDM_STYLE_BASE + 1, IDM_STYLE_BASE + static_cast<UINT>(appearance.style), MF_BYCOMMAND);
        // The menu takes ownership of submenus appended with MF_POPUP and destroys them with itself.
        AppendMenuW(menu.get(), MF_POPUP, reinterpret_cast<UINT_PTR>(size.release()), L"Size");
        AppendMenuW(menu.get(), MF_POPUP, reinterpret_cast<UINT_PTR>(style.release()), L"Style");
        for (const auto& c : extras.choices)
        {
            wil::unique_hmenu sub(CreatePopupMenu());
            if (!sub || c.items.empty())
                continue;
            for (size_t i = 0; i < c.items.size(); ++i)
                AppendMenuW(sub.get(), MF_STRING, c.baseId + static_cast<UINT>(i), c.items[i].c_str());
            CheckMenuRadioItem(sub.get(), c.baseId, c.baseId + static_cast<UINT>(c.items.size()) - 1, c.baseId + static_cast<UINT>(c.checked), MF_BYCOMMAND);
            AppendMenuW(menu.get(), MF_POPUP, reinterpret_cast<UINT_PTR>(sub.release()), c.text.c_str());
        }
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        for (const auto& t : extras.toggles)
            AppendMenuW(menu.get(), MF_STRING | (t.checked ? MF_CHECKED : MF_UNCHECKED), t.id, t.text.c_str());
        AppendMenuW(menu.get(), MF_STRING | (startWithWindows ? MF_CHECKED : MF_UNCHECKED), IDM_START_WITH_WINDOWS, L"Start with Windows");
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu.get(), MF_STRING, IDM_QUIT, L"Quit");

        // Known shell quirk: the owner must be foreground or the menu will not close when you click
        // elsewhere, and posting WM_NULL afterwards makes the second right-click work.
        SetForegroundWindow(m_owner);
        TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, anchor.x, anchor.y, m_owner, nullptr);
        PostMessageW(m_owner, WM_NULL, 0, 0);
    }
}
