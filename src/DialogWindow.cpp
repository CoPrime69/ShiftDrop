#include "DialogWindow.h"
#include "Strings.h"
#include "resource.h"

#include <commctrl.h>
#include <uxtheme.h>

namespace sd
{
    DialogWindow::DialogWindow(HINSTANCE instance, const Frame& frame) : m_instance(instance), m_frame(frame) {}

    DialogWindow::~DialogWindow()
    {
        if (m_hwnd)
        {
            // The derived window is gone already: its messages from here on go to DefWindowProc.
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
            DestroyWindow(m_hwnd);
        }
    }

    bool DialogWindow::Create(const wchar_t* title)
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        if (!GetClassInfoExW(m_instance, m_frame.className, &wc))
        {
            wc = WNDCLASSEXW{ sizeof(wc) };
            wc.lpfnWndProc = WndProc;
            wc.hInstance = m_instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(m_instance, MAKEINTRESOURCEW(IDI_APPICON));
            wc.lpszClassName = m_frame.className;   // no background brush: WM_ERASEBKGND paints m_colors.background
            if (!RegisterClassExW(&wc))
                return false;
        }
        // WS_EX_CONTROLPARENT lets IsDialogMessage move focus with Tab between our child controls.
        m_hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, m_frame.className, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 m_frame.width, m_frame.height, nullptr, nullptr, m_instance, this);
        if (!m_hwnd)
            return false;
        CreateChildren();
        // Size the client area for this monitor's DPI; CreateWindow took the size in physical pixels.
        const UINT dpi = GetDpiForWindow(m_hwnd);
        RECT r{ 0, 0, MulDiv(m_frame.width, static_cast<int>(dpi), 96), MulDiv(m_frame.height, static_cast<int>(dpi), 96) };
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT, dpi);
        SetWindowPos(m_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyDpi(dpi);
        ApplyTheme();
        return true;
    }

    void DialogWindow::Present(HWND focus)
    {
        ApplyTheme();   // the theme may have changed while it was hidden
        ShowWindow(m_hwnd, IsIconic(m_hwnd) ? SW_RESTORE : SW_SHOWNORMAL);
        SetForegroundWindow(m_hwnd);
        SetFocus(focus);
    }

    HWND DialogWindow::Control(const wchar_t* windowClass, const wchar_t* text, DWORD style, int id, DWORD exStyle) const
    {
        return CreateWindowExW(exStyle, windowClass, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, m_hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance, nullptr);
    }

    void DialogWindow::ApplyDpi(UINT dpi)
    {
        m_dpi = dpi;
        // The message font ("Segoe UI 9pt") as the user configured it, at this window's DPI.
        NONCLIENTMETRICSW metrics{ sizeof(metrics) };
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
            m_font.reset(CreateFontIndirectW(&metrics.lfMessageFont));
        for (HWND child = GetWindow(m_hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(m_font.get()), TRUE);
        OnDpiChanged();
        Layout();
    }

    void DialogWindow::ApplyTheme()
    {
        m_colors = theme::Dialog(m_frame.lightBackground);
        theme::ApplyDarkCaption(m_hwnd, m_colors.dark);
        m_background.reset(CreateSolidBrush(m_colors.background));
        m_field.reset(CreateSolidBrush(m_colors.field));
        OnThemeChanged();
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }

    void DialogWindow::SetTheme(std::initializer_list<HWND> controls, const wchar_t* dark, const wchar_t* light) const
    {
        for (HWND control : controls)
            SetWindowTheme(control, m_colors.dark ? dark : light, nullptr);
    }

    void DialogWindow::ThemeList(HWND list, const wchar_t* light) const
    {
        SetTheme({ list }, L"DarkMode_Explorer", light);
        SetTheme({ ListView_GetHeader(list) }, L"DarkMode_ItemsView");
        ListView_SetBkColor(list, m_colors.field);
        ListView_SetTextBkColor(list, m_colors.field);
        ListView_SetTextColor(list, m_colors.text);
    }

    std::wstring DialogWindow::Text(HWND control)
    {
        std::wstring text(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
        text.resize(static_cast<size_t>(GetWindowTextW(control, text.data(), static_cast<int>(text.size()))));
        return text;
    }

    bool DialogWindow::PreTranslate(MSG& msg)
    {
        if (!m_hwnd || (msg.hwnd != m_hwnd && !IsChild(m_hwnd, msg.hwnd)))
            return false;
        return IsDialogMessageW(m_hwnd, &msg) != FALSE;
    }

    LRESULT CALLBACK DialogWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* self = reinterpret_cast<DialogWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self || self->m_hwnd != hwnd)
            return DefWindowProcW(hwnd, msg, wParam, lParam);   // messages before CreateWindowExW returns
        if (msg == WM_NCDESTROY)
        {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->m_hwnd = nullptr;
            self->OnNcDestroy();
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        return self->Handle(msg, wParam, lParam);
    }

    LRESULT DialogWindow::Handle(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = { Px(m_frame.minWidth), Px(m_frame.minHeight) };
            return 0;
        }
        case WM_DPICHANGED:
        {
            // The window moved to a monitor with another scale. lParam is the size Windows suggests at the new DPI.
            const auto* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyDpi(HIWORD(wParam));
            return 0;
        }
        case WM_SETTINGCHANGE:
            // "ImmersiveColorSet": the user switched between light and dark.
            if (lParam && EqualsNoCase(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet"))
                ApplyTheme();
            break;
        case WM_ERASEBKGND:
        {
            RECT rc{};
            GetClientRect(m_hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, m_background.get());
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, StaticTextColor(reinterpret_cast<HWND>(lParam)));
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(m_background.get());
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if (m_colors.dark)
            {
                const HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, m_colors.text);
                SetBkColor(dc, m_colors.field);
                return reinterpret_cast<LRESULT>(m_field.get());
            }
            break;
        default:
            break;
        }
        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }
}
