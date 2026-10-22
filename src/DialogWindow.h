#pragma once
#include "EditorFrame.h"
#include "Theme.h"

#include <windows.h>
#include <commctrl.h>
#include <oleacc.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sd
{
    // The frame every window built from standard controls shares: Settings, History, Rename and the
    // metadata inspector. A plain top-level window (screen readers read its controls as they are),
    // laid out by hand in device pixels from its DPI. Under per-monitor-v2 Windows scales the frame,
    // but the controls' font and places are ours to scale, so WM_DPICHANGED rebuilds the font and
    // lays everything out again. It is light or dark like the rest of the app (theme::Dialog), with
    // a dark title bar and dark control themes, and follows a switch while it is open.
    //
    // The look of the App Windows page (cards, navigation, toggles, segmented choices, chips, tabs,
    // drop-downs, text boxes) is drawn here, with the control kit (EditorFrame.h), on real controls:
    // every control stays a standard button, static, combo box, edit or trackbar, so Tab, the arrow
    // keys, Space and Enter, focus, and screen readers work as in any dialog, and only the drawing is
    // ours. A window makes them with the helpers below (KitButton, Label, DropDown, TextField, Slider);
    // the frame draws them, the window paints its cards behind them in PaintSurface.
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
        // Shows (the keyboard moved the focus) or hides (a click) every focus ring in the window: its
        // UI state (WM_CHANGEUISTATE), which the kit reads before drawing a ring. PreTranslate and the
        // window's clicks do it; a window that moves the focus with keys of its own calls it.
        void ShowFocusCues(bool show);

    protected:
        struct Frame
        {
            const wchar_t* className;
            int width, height;         // the client area when it opens, DIPs
            int minWidth, minHeight;   // the smallest the user can make the window, DIPs
            int lightBackground = COLOR_WINDOW;
            bool scrolls = false;      // a vertical scroll bar for a column taller than the window (SetScroll)
            bool kitFont = false;      // edits in the kit's font (Segoe UI Variable, 13 DIPs), not the message font
            bool fixed = false;        // a question: a dialog frame the user doesn't resize (SetClientSize sizes it)
        };
        DialogWindow(HINSTANCE instance, const Frame& frame);

        // Creates the window, hidden: its controls, the client area sized for its monitor's DPI and
        // the message font on every control. False when it could not be created.
        bool Create(const wchar_t* title);
        // The same, owned by `owner` (it stays over it, and has no taskbar button of its own).
        bool Create(const wchar_t* title, HWND owner);
        // The client area `width` x `height` DIPs at the window's DPI (a question sized to its text).
        void SetClientSize(int width, int height);
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

        // Changes what the window shows (another page or tab, a card folded open) with its drawing
        // held: `change` fills and lays out the new content (Layout()), then the window and every
        // control are painted once, from the new content, so no frame in between ever shows: no
        // control moved but not yet drawn, nothing of the page before. Calls nest; only the outermost
        // paints. While the window is hidden it only calls `change`.
        void Rebuild(const std::function<void()>& change);
        // In Layout() while ScrollTo() scrolls: the column's pixels and its controls have moved already
        // (ScrollWindowEx), so Layout() only updates where things are and invalidates nothing itself.
        bool Scrolling() const { return m_scrolling; }
        // In Layout(): shows the controls of `all` that are in `placed` and hides the others, so Tab and
        // screen readers skip what the page doesn't show. It goes by each control's own WS_VISIBLE:
        // IsWindowVisible() says no for every control while the window is hidden or its drawing is held,
        // which left the controls of the tab before on top of the new ones.
        static void ShowOnly(std::span<const HWND> all, std::span<const HWND> placed);

        // A child control (WS_CHILD | WS_VISIBLE | `style`), placed by Layout() later.
        HWND Control(const wchar_t* windowClass, const wchar_t* text, DWORD style, int id = 0, DWORD exStyle = 0) const;
        int Px(int dip) const { return MulDiv(dip, static_cast<int>(m_dpi), 96); }
        float Dip(int px) const { return static_cast<float>(px) * 96.f / static_cast<float>(m_dpi); }
        D2D1_RECT_F DipRect(const RECT& r) const { return D2D1::RectF(Dip(r.left), Dip(r.top), Dip(r.right), Dip(r.bottom)); }
        // The visual style the controls draw with: `dark` in dark mode, `light` (null: the default
        // one) in light mode. "DarkMode_Explorer" is the dark theme Explorer's own lists and buttons
        // use, "DarkMode_CFD" (the common file dialog's) has the edit and combo boxes.
        void SetTheme(std::initializer_list<HWND> controls, const wchar_t* dark, const wchar_t* light = nullptr) const;
        // A report-view ListView in the current colours: its rows, its header and its theme.
        void ThemeList(HWND list, const wchar_t* light = nullptr) const;
        // Text for what can't be used or went wrong (a name Rename refuses, a failed run), and for
        // what no longer counts (an undone run), readable in the current light or dark.
        COLORREF ProblemColor() const { return m_colors.problem; }
        COLORREF MutedColor() const { return m_colors.muted; }
        // An edit box's (or any control's) text.
        static std::wstring Text(HWND control);
        // A button the window draws itself, from its NM_CUSTOMDRAW, with the editors' control kit
        // (EditorFrame.h) over the window's background: `paint` draws in DIPs, `r` being the button's
        // client area and `s` its state (focused only while focus cues show). The WM_NOTIFY result.
        using KitPaint = std::function<void(ControlKit& kit, const D2D1_RECT_F& r, ControlKit::ControlState s)>;
        LRESULT CustomDraw(const NMCUSTOMDRAW& draw, const KitPaint& paint);

        // ---- The kit's controls -------------------------------------------------------------------
        // What a control sits on: its drawing starts from that colour.
        enum class Surface { Window, Card };
        enum class Look
        {
            Button,     // a push button
            Primary,    // the one push button in the accent colour (BS_DEFPUSHBUTTON: Enter presses it)
            Icon,       // a glyph alone (`icon`); its text is its name for screen readers
            Toggle,     // a check box drawn as "On"/"Off" and a switch; its text is the setting's name
            Chip,       // a check box drawn as a chip
            Segment,    // a radio button drawn as one option of a segmented choice (`part` says which)
            Tab,        // a radio button drawn as a tab
            Nav,        // a radio button drawn as a navigation item (`icon` before its text)
            Expander,   // a push button drawn as a card's header that folds open (`detail`, `trailing`, `open`)
            Radio,      // a radio button with `detail` after its text
            Custom,     // a radio button drawn by the window's DrawControl
        };
        struct Style
        {
            Look look = Look::Button;
            Surface on = Surface::Card;
            wchar_t icon = 0;
            ControlKit::Part part = ControlKit::Part::Only;
            std::wstring detail, trailing;   // Expander: the line under the title, the text before the chevron; Radio: detail
            bool open = false;               // Expander: folded open
        };
        // A button (push, check box or radio button by its look) drawn by the kit. `first`: it starts
        // a group (WS_GROUP | WS_TABSTOP): Tab stops on a group once, the arrow keys move inside it.
        HWND KitButton(Look look, const wchar_t* text, int id, Surface on = Surface::Card, wchar_t icon = 0, bool first = true);
        Style& StyleOf(HWND control);   // to change an Expander's text or a Segment's part; Redraw() after
        // Destroys a control the window made, and forgets how to draw it.
        void DestroyControl(HWND control);
        static void Redraw(HWND control) { InvalidateRect(control, nullptr, FALSE); }
        // Checks button `index` of a radio group (none for -1) and moves the group's tab stop to it, as
        // dialogs do: Tab lands on the checked one.
        static void CheckOne(std::span<const HWND> group, int index);
        static bool Checked(HWND button) { return SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED; }

        // Text the kit draws (an owner-drawn static, read by screen readers like any static).
        enum class TextRole
        {
            Heading,   // a page's title, 28 DIPs semibold
            Group,     // a heading over cards, 14 semibold
            Title,     // a card's title, 14
            Body,      // 13, wraps
            Caption,   // a card's description, 12 in the subtle colour, wraps
            Hint,      // 12 in the faint colour, wraps
            Error,     // 12 in the error colour, wraps
            Strong,    // 13 semibold
            Value,     // 13 semibold, at the right ("100%")
            AppName,   // 16 semibold
            Small,     // 11.5 in the faint colour (a slider's ends)
            Centred,   // 12 in the subtle colour, centred, wraps
            Question,  // a question's heading, 20 semibold, wraps
            SmallEnd,  // Small, at the right (a slider's far end)
        };
        HWND Label(const wchar_t* text, TextRole role, Surface on = Surface::Card);
        void SetRole(HWND label, TextRole role);
        // The label's height for its text wrapped at `widthPx`, pixels.
        int LabelHeight(HWND label, int widthPx);

        // A drop-down list (a combo box) drawn by the kit; its items and its open list too. Screen
        // readers name it from Describe() (a combo box has no text of its own).
        HWND DropDown(int id, std::span<const std::wstring> items, Surface on = Surface::Card);
        // A text field: a native edit (typing, selection, the clipboard and screen readers as Windows
        // has them) inside a box the kit draws. `search`: a magnifier at its start.
        HWND TextField(int id, DWORD style, const wchar_t* cue, bool search = false, Surface on = Surface::Card);
        // Places the field's box at `box` (client pixels) and its edit inside it.
        void PlaceField(HWND edit, const RECT& box);
        void SetInvalid(HWND edit, bool invalid);
        // A slider (a trackbar) drawn by the kit.
        HWND Slider(int id, int low, int high, int step, Surface on = Surface::Card);
        // Places a slider at `box` (client pixels) and the labels of its two ends under it (Small and
        // SmallEnd, either may be null), at the two ends of the bar it draws: the far one ends where the
        // bar ends. Returns the labels' bottom.
        int PlaceSlider(HWND slider, const RECT& box, HWND low, HWND high);

        // A list (a report-view ListView) whose rows the kit draws: `rowDip` tall, the selected one in
        // the selection colour with the accent's bar, the hovered one lightly, the focused one with a
        // ring; the window draws each row's content in DrawRow. The columns stay (screen readers read
        // a row's cells from them); pass LVS_NOCOLUMNHEADER in `style` to hide their header.
        HWND KitList(int id, DWORD style, std::initializer_list<const wchar_t*> columns, int rowDip, Surface on = Surface::Card);
        // Row `row` of a KitList, in `r` (DIPs), over its background.
        virtual void DrawRow(HWND /*list*/, int /*row*/, ControlKit&, const D2D1_RECT_F& /*r*/, bool /*selected*/) {}

        // The name and the description screen readers say for a control (Dynamic Annotation), for
        // controls whose text doesn't say it (a drop-down, an icon button, a chip's longer name).
        void Describe(HWND control, const std::wstring& name, const std::wstring& description = {});

        // What the window paints behind its controls (cards, a picture), in DIPs over the window's
        // background colour; then GDI on the same DC (icons).
        virtual void PaintSurface(ControlKit&, const D2D1_RECT_F& /*client*/) {}
        virtual void PaintGdi(HDC) {}
        // A Look::Custom control: draw it in `r` (DIPs).
        virtual void DrawControl(HWND, ControlKit&, const D2D1_RECT_F& /*r*/, ControlKit::ControlState) {}
        D2D1_COLOR_F SurfaceColor(Surface on) const;
        ControlKit& Kit();   // the kit, for measuring text in Layout()

        // A column that scrolls (Frame::scrolls): the part of the client area it fills and how tall its
        // content is, pixels. Layout() places the column's controls ScrollY() higher than their place in
        // it; a control the keyboard focuses is scrolled into view.
        void SetScroll(const RECT& area, int contentHeight);
        int ScrollY() const { return m_scrollY; }
        void ScrollTo(int y);
        void ScrollIntoView(const RECT& controlRect);

        HINSTANCE m_instance;
        HWND m_hwnd = nullptr;   // the controls are destroyed with it
        UINT m_dpi = 96;
        theme::DialogColors m_colors;

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        static LRESULT CALLBACK DropDownProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR self);
        static LRESULT CALLBACK SliderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR self);
        // The messages the kit's controls send the window, taken before Handle(): true when handled.
        bool KitMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result);
        void ApplyDpi(UINT dpi);
        // Draws into `dc` over `rc` (pixels) with the kit, double-buffered: from `behind`, then, for a
        // control `over` (`rc` in its client area) or the window itself, the window's surface under it.
        bool Paint(HDC dc, const RECT& rc, HWND over, const D2D1_COLOR_F& behind, const std::function<void(ControlKit&, const D2D1_RECT_F&)>& paint);
        bool EnsureTarget();
        void ResetTarget();
        HDC Buffer(int width, int height);             // the memory DC Paint() draws in, at least this large
        bool EnsureBackdrop();                         // the surface, drawn again when WM_PAINT asked
        void PaintBackdrop(ControlKit& kit, const D2D1_RECT_F& client);   // PaintSurface and the field boxes
        void PaintWindow();
        LRESULT DrawStyled(const NMCUSTOMDRAW& draw, const Style& style);
        void DrawLabel(const DRAWITEMSTRUCT& item);
        void DrawDropDownItem(const DRAWITEMSTRUCT& item);
        void PaintDropDown(HWND combo);
        LRESULT DrawSlider(const NMCUSTOMDRAW& draw);
        LRESULT DrawListRow(const NMLVCUSTOMDRAW& draw);
        ControlKit::ControlState StateOf(HWND control, bool pressed) const;

        struct KitTarget;              // the kit and its target, made on first use
        std::unique_ptr<KitTarget> m_kit;
        Frame m_frame;
        wil::unique_hfont m_font;
        wil::unique_hbrush m_background;
        wil::unique_hbrush m_field;   // edit box and drop-down list backgrounds

        std::map<HWND, Style> m_styles;
        struct LabelInfo
        {
            TextRole role;
            Surface on;
        };
        std::map<HWND, LabelInfo> m_labels;
        std::map<HWND, Surface> m_dropDowns, m_sliders;
        struct ListInfo
        {
            int rowDip = 32;
            Surface on = Surface::Card;
        };
        std::map<HWND, ListInfo> m_lists;
        HWND m_owner = nullptr;
        DWORD m_style = 0, m_exStyle = 0;
        struct FieldInfo
        {
            RECT box{};
            bool search = false, invalid = false;
            Surface on = Surface::Card;
        };
        std::map<HWND, FieldInfo> m_fields;
        HWND m_hotDropDown = nullptr;
        Microsoft::WRL::ComPtr<IAccPropServices> m_accessibility;
        std::vector<HWND> m_described;
        RECT m_scrollArea{};
        int m_scrollY = 0, m_scrollHeight = 0;
        int m_wheelRest = 0;          // the part of a wheel turn too small to scroll a pixel yet
        bool m_scrolling = false;
        int m_holds = 0;              // Rebuild() calls under way
        bool m_held = false;          // the outermost one switched the drawing off (WM_SETREDRAW)
    };

    // The Windows colour dialog (ChooseColor), owned by `owner`, opening on `rgb` (0xRRGGBB): true with
    // the colour picked in `rgb`, false when cancelled. Its custom colours are kept while the app runs.
    bool PickColour(HWND owner, uint32_t& rgb);
}
