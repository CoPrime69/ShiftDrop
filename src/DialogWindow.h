#pragma once
#include "Theme.h"

#include <windows.h>
#include <wil/resource.h>

#include <initializer_list>

namespace sd
{
    // The frame every window built from standard controls shares: Settings, History, Rename and the
    // metadata inspector. A plain top-level window (screen readers read its controls as they are),
    // laid out by hand in device pixels from its DPI. Under per-monitor-v2 Windows scales the frame,
    // but the controls' font and places are ours to scale, so WM_DPICHANGED rebuilds the font and
    // lays everything out again. It is light or dark like the rest of the app (theme::Dialog), with
    // a dark title bar and dark control themes, and follows a switch while it is open.
    //
    // A window derives from it, creates its controls in CreateChildren() and places them in Layout();
    // its Handle() takes the messages it cares about and passes the rest to DialogWindow::Handle.
    class DialogWindow
    {
    public:
        virtual ~DialogWindow();
        DialogWindow(const DialogWindow&) = delete;
        DialogWindow& operator=(const DialogWindow&) = delete;

        // Tab, Enter and Esc for the app's message loop (IsDialogMessage); true when consumed.
        bool PreTranslate(MSG& msg);

    protected:
        struct Frame
        {
            const wchar_t* className;
            int width, height;         // the client area when it opens, DIPs
            int minWidth, minHeight;   // the smallest the user can make the window, DIPs
            int lightBackground = COLOR_WINDOW;
        };
        DialogWindow(HINSTANCE instance, const Frame& frame);

        // Creates the window, hidden: its controls, the client area sized for its monitor's DPI and
        // the message font on every control. False when it could not be created.
        bool Create(const wchar_t* title);
        // Shows it in front (restored if minimised), in the current light or dark, with the focus on
        // `focus`. Allowed to come forward: the user just asked for it.
        void Present(HWND focus);
        // Light or dark from theme::Dialog(), for the frame and then OnThemeChanged().
        void ApplyTheme();

        virtual void CreateChildren() = 0;
        virtual void Layout() = 0;
        virtual void OnDpiChanged() {}     // after the new font is set, before Layout(): list columns, row heights
        virtual void OnThemeChanged() {}   // after m_colors: the controls' themes and colours
        // The text colour of a static control, check box or a disabled edit box.
        virtual COLORREF StaticTextColor(HWND) const { return m_colors.text; }
        // After WM_NCDESTROY: m_hwnd is null. A window that deletes itself does it here.
        virtual void OnNcDestroy() {}
        virtual LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);

        // A child control (WS_CHILD | WS_VISIBLE | `style`), placed by Layout() later.
        HWND Control(const wchar_t* windowClass, const wchar_t* text, DWORD style, int id = 0, DWORD exStyle = 0) const;
        int Px(int dip) const { return MulDiv(dip, static_cast<int>(m_dpi), 96); }
        // The visual style the controls draw with: `dark` in dark mode, `light` (null: the default
        // one) in light mode. "DarkMode_Explorer" is the dark theme Explorer's own lists and buttons
        // use, "DarkMode_CFD" (the common file dialog's) has the edit and combo boxes.
        void SetTheme(std::initializer_list<HWND> controls, const wchar_t* dark, const wchar_t* light = nullptr) const;
        // A report-view ListView in the current colours: its rows, its header and its theme.
        void ThemeList(HWND list, const wchar_t* light = nullptr) const;

        HINSTANCE m_instance;
        HWND m_hwnd = nullptr;   // the controls are destroyed with it
        UINT m_dpi = 96;
        theme::DialogColors m_colors;

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        void ApplyDpi(UINT dpi);

        Frame m_frame;
        wil::unique_hfont m_font;
        wil::unique_hbrush m_background;
        wil::unique_hbrush m_field;   // edit box and drop-down list backgrounds in dark mode
    };
}
