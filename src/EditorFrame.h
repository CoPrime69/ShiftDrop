#pragma once
#include "Theme.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The control kit: push buttons, tool-bar icons, segmented choices, switches, sliders, text fields and
// colour swatches, one look for every window that draws its controls with Direct2D, in the editor
// palette (theme::EditorColors), with their hover, pressed, disabled and keyboard-focus states. The
// window lays them out, tracks their state and draws their labels; labels of disabled controls are
// drawn at kDisabledAlpha like the controls. It draws in DIPs on the target SetTarget() gave it: an
// editor's window (EditorFrame below) or a custom-drawn standard control (DialogWindow::CustomDraw).
// UI thread only.
namespace sd
{
    class ControlKit
    {
    public:
        ControlKit();
        ControlKit(const ControlKit&) = delete;
        ControlKit& operator=(const ControlKit&) = delete;

        // The target to draw on (null while there is none); brushes are made again for another one.
        void SetTarget(ID2D1RenderTarget* target);
        void SetPalette(const theme::EditorPalette& colors) { m_colors = colors; }
        const theme::EditorPalette& Theme() const { return m_colors; }
        IDWriteFactory* DWrite() const { return m_dwrite.Get(); }

        ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& color);
        // Cached one-line formats, vertically centred, that end in "..." when the text doesn't fit.
        IDWriteTextFormat* TextFormat(float sizeDip, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
        IDWriteTextFormat* IconFormat(float sizeDip);
        void DrawText(std::wstring_view text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);
        float TextWidth(std::wstring_view text, float sizeDip, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);

        struct ControlState
        {
            bool enabled = true;
            bool hot = false;       // under the mouse
            bool pressed = false;   // the mouse is down on it
            bool focused = false;   // has the keyboard focus: drawn with a ring
        };
        static constexpr float kControlRadius = 4.f;
        static constexpr float kControlText = 13.f;   // the size of control labels
        static constexpr float kDisabledAlpha = 0.4f;

        // ---- The editors' frame (App Editors page; sizes and layout maths in EditorChrome.h) ----------
        // The tool bar's background, with a line under it.
        void DrawToolBar(const D2D1_RECT_F& r);
        // A tool's tab on the tool bar: its icon over its name, and while `selected` a tinted
        // background, a semibold name and a short accent bar under it. Width: chrome::TabWidth.
        void DrawToolTab(const D2D1_RECT_F& r, wchar_t icon, std::wstring_view label, bool selected, ControlState s);
        // The 1 DIP line between groups of the tool bar.
        void DrawToolBarDivider(const D2D1_RECT_F& r);
        // The options panel: a card with a line on its left and the tool's name at the top. Its rows
        // start chrome::kPanelPadY + chrome::kPanelTitleHeight + chrome::kPanelGap below r.top.
        void DrawSidePanel(const D2D1_RECT_F& r, std::wstring_view title);
        // A small semibold heading over a group of the panel's controls ("Ratio", "Size").
        void DrawSectionLabel(const D2D1_RECT_F& r, std::wstring_view text);
        // The footer: a line over it, `status` on the left (what will be saved; from `textLeft`, or the
        // footer's padding when negative, and ending in "..." before `textRight`). The window draws
        // Cancel and Apply on its right.
        void DrawFooterBar(const D2D1_RECT_F& r, std::wstring_view status, float textRight, float textLeft = -1.f);
        // One option of a choice shown as tiles of an icon over a short name (Annotate's Arrow, Text...):
        // tinted and outlined in the accent when `chosen`.
        void DrawToolTile(const D2D1_RECT_F& r, wchar_t icon, std::wstring_view label, bool chosen, ControlState s);

        // ---- Every window's controls ----
        void DrawButton(const D2D1_RECT_F& r, std::wstring_view text, wchar_t icon, bool primary, ControlState s);
        // A glyph alone, filled only while hot, pressed or `on`.
        void DrawIconButton(const D2D1_RECT_F& r, wchar_t icon, bool on, ControlState s);
        // One option of a segmented choice.
        void DrawChoice(const D2D1_RECT_F& r, std::wstring_view text, bool selected, ControlState s);
        // An on/off switch, `r` 40 x 20 DIPs.
        void DrawSwitch(const D2D1_RECT_F& r, bool on, ControlState s);
        // A slider's bar `track` (4 DIPs high), filled from `from` to `value` (both 0..1), and its thumb.
        void DrawSlider(const D2D1_RECT_F& track, float value, float from, ControlState s);
        // A text field's box; the editor draws its text and caret inside.
        void DrawField(const D2D1_RECT_F& r, bool editing, bool invalid, ControlState s);
        // A round colour swatch as large as fits in `r` with room for the focus ring around it, `glyph`
        // (an icon font character, 0 for none) on it in `ink`, and a ring around it when `selected`.
        void DrawSwatch(const D2D1_RECT_F& r, const D2D1_COLOR_F& colour, wchar_t glyph, const D2D1_COLOR_F& ink, bool selected, ControlState s);
        void DrawFocusRing(const D2D1_RECT_F& r, float radius);
        void FillRound(const D2D1_RECT_F& r, const D2D1_COLOR_F& color, float radius);
        void StrokeRound(const D2D1_RECT_F& r, const D2D1_COLOR_F& color, float width, float radius);   // inside `r`
        // `r` filled with `color` where it is inside the rounded rectangle `shape` (a line along a box's edge).
        void FillInside(const D2D1_RECT_F& shape, float radius, const D2D1_RECT_F& r, const D2D1_COLOR_F& color);

        // ---- The windows' pieces (App Windows page: Settings, History, Rename, the inspector and the
        // questions). Each draws one control in `r`; the window says where and in what state. ----
        static constexpr float kCardRadius = 6.f;
        static constexpr float kNavText = 14.f;     // navigation, tabs, a card's title
        static constexpr float kSmallText = 12.f;   // a card's description, hints
        // A glyph of the icon font, centred in `r`.
        void DrawGlyph(wchar_t glyph, float sizeDip, const D2D1_RECT_F& r, const D2D1_COLOR_F& color);
        // Text that wraps at the width of `r`, from its top; and how tall it is at `width`.
        void DrawParagraph(std::wstring_view text, float sizeDip, DWRITE_FONT_WEIGHT weight, const D2D1_RECT_F& r, const D2D1_COLOR_F& color,
                           DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
        float TextHeight(std::wstring_view text, float sizeDip, DWRITE_FONT_WEIGHT weight, float width);
        // A card: one setting, a group of rows, a list.
        void DrawCard(const D2D1_RECT_F& r);
        // An item of the navigation on the left: its glyph and name; the selected one has a fill and the
        // accent's bar at its start.
        void DrawNavItem(const D2D1_RECT_F& r, wchar_t icon, std::wstring_view text, bool selected, ControlState s);
        // A switch with "On" or "Off" before it, at the right of `r`.
        void DrawToggle(const D2D1_RECT_F& r, bool on, ControlState s);
        // One option of a segmented choice (Windows 11's): a grey track around the options, the
        // selected one raised in the card colour with the accent under it. Each option draws its part
        // of the track, so the options side by side make the whole.
        enum class Part { Only, First, Middle, Last };
        void DrawSegment(const D2D1_RECT_F& r, std::wstring_view text, bool selected, Part part, ControlState s);
        float SegmentWidth(std::wstring_view text);
        // A radio button: a round mark, its text, and `detail` after it in the subtle colour.
        void DrawRadio(const D2D1_RECT_F& r, std::wstring_view text, std::wstring_view detail, bool selected, ControlState s);
        // A tab: its name, with the accent's bar under the selected one.
        void DrawTab(const D2D1_RECT_F& r, std::wstring_view text, bool selected, ControlState s);
        float TabWidth(std::wstring_view text);
        // A chip that turns something on or off: a pill, ticked and in the accent's tint while on.
        void DrawChip(const D2D1_RECT_F& r, std::wstring_view text, bool on, ControlState s);
        float ChipWidth(std::wstring_view text);
        // A closed drop-down list: its value and a chevron; and one row of the open list (`current` is
        // the value it has now: the accent's bar; `hot` under the mouse or the arrow keys).
        void DrawDropDown(const D2D1_RECT_F& r, std::wstring_view text, bool open, ControlState s);
        void DrawListRow(const D2D1_RECT_F& r, std::wstring_view text, bool current, bool hot);
        // The box around a text field (a native edit inside it): a line under it, the accent's while
        // the field has the focus, the error colour when what is typed can't be used.
        void DrawTextBox(const D2D1_RECT_F& r, bool focused, bool invalid, bool enabled = true);
        // The header of a card that folds open (Settings › Wheel's kinds of file, the accent colours):
        // glyph, title, a line under it, a count at the end, and a chevron.
        void DrawExpander(const D2D1_RECT_F& r, wchar_t icon, std::wstring_view title, std::wstring_view detail, std::wstring_view trailing,
                          bool open, ControlState s);
        // A small picture of the wheel: `tiles` placed where the real wheel places them
        // (keyboard/WheelLayout.h), in the wheel's own colours (`palette`), tile `hot` in the accent,
        // and `centre` over `detail` in the middle.
        struct MiniTile
        {
            std::wstring label;
            wchar_t icon = 0;
        };
        void DrawMiniWheel(D2D1_POINT_2F centre, float radius, const std::vector<MiniTile>& tiles, int hot, const theme::Palette& palette,
                           std::wstring_view centreText, std::wstring_view detail);

    private:
        ID2D1RenderTarget* m_target = nullptr;
        theme::EditorPalette m_colors{};
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwrite;
        std::wstring m_textFamily, m_iconFamily;
        struct CachedBrush
        {
            D2D1_COLOR_F color;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        };
        std::vector<CachedBrush> m_brushes;
        struct CachedFormat
        {
            float size;
            int weight, align;
            bool icon;
            Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
            bool wrap = false;   // DrawParagraph's: wraps, from the top
        };
        std::vector<CachedFormat> m_formats;
        IDWriteTextFormat* WrapFormat(float sizeDip, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align);
    };
}

// The frame every Direct2D editor window shares: the PDF page view and Merge list, the picture editor
// (canvas) and the video and audio timeline editor. An ordinary resizable top-level window drawn
// with Direct2D in DIPs at the monitor's DPI, in the editor palette (theme::EditorColors, light or
// dark like the rest of the app, title bar included), with the control kit above (its brushes, text
// formats and controls), and push buttons drawn and hit-tested here. Modeless: each window owns itself
// and deletes itself when it is destroyed. UI thread only.
namespace sd
{
    class EditorFrame : protected ControlKit
    {
    public:
        EditorFrame(const EditorFrame&) = delete;
        EditorFrame& operator=(const EditorFrame&) = delete;

    protected:
        // `opaque`: an opaque target, so text is drawn with the system's antialiasing (ClearType);
        // otherwise grayscale on a premultiplied target.
        explicit EditorFrame(bool opaque = false);
        virtual ~EditorFrame();

        // The monitor under the cursor, where a new window opens (the user is working there).
        struct Monitor
        {
            RECT work{};    // its work area, pixels
            UINT dpi = 96;
        };
        static Monitor CursorMonitor();

        // Creates the window (and shows it in front, unless `show` is false), its client area
        // `widthDip` x `heightDip` on the cursor's monitor, centred and kept on screen; the user can't
        // make the client area smaller than the minimum. On failure the caller still owns the object.
        // An `owner` makes it a window of the owner's (no taskbar button), centred over it.
        bool Create(const std::wstring& title, int widthDip, int heightDip, int minWidthDip, int minHeightDip, bool show = true, HWND owner = nullptr);
        void Present();   // shows it in front: we are answering the user's drop
        void Close();     // OnClosing(), then destroys the window and the object
        // The × button or Alt+F4. A window whose own handlers might still be running posts its
        // closing instead.
        virtual void RequestClose() { Close(); }

        struct Button
        {
            std::wstring text;
            wchar_t icon = 0;          // a glyph of the icon font, drawn before the text
            bool primary = false;
            bool enabled = true;
            D2D1_RECT_F rect{};        // DIPs, set by the subclass's layout
            std::function<void()> action;
            bool visible = true;       // hidden buttons are neither drawn nor clicked
            bool flat = false;         // an icon alone with no box, like the tool bar's undo and redo (DrawIconButton)
        };

        // ---- For subclasses ----------------------------------------------------------------------
        // Between BeginDraw and EndDraw, after the window is cleared to Theme().window.
        virtual void Paint(ID2D1RenderTarget* rt, D2D1_SIZE_F size) = 0;
        virtual void Layout(D2D1_SIZE_F) {}
        // Window messages the frame doesn't take; return true when handled.
        virtual bool OnMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) = 0;
        // The window is going away (Esc, ×, or Close()): a last chance to stop background work.
        virtual void OnClosing() {}
        // The window moved to a monitor with another DPI (after Layout at the new size).
        virtual void OnDpiChanged() {}

        HWND Hwnd() const { return m_hwnd; }
        UINT Dpi() const { return m_dpi; }
        float Scale() const { return m_dpi / 96.f; }
        D2D1_SIZE_F ClientDip() const;
        D2D1_POINT_2F DipFromLParam(LPARAM lParam) const;
        void Invalidate() const;

        ID2D1Factory* Factory() const { return m_d2d.Get(); }
        ID2D1RenderTarget* Target() const { return m_rt.Get(); }
        // Device-dependent resources (brushes, bitmaps) must be rebuilt when this changes.
        unsigned DeviceGeneration() const { return m_generation; }

        std::vector<Button> m_buttons;
        void DrawButtons();
        // The button under a point, or -1.
        int ButtonAt(D2D1_POINT_2F p) const;

        static bool Ctrl() { return GetKeyState(VK_CONTROL) < 0; }
        static bool Shift() { return GetKeyState(VK_SHIFT) < 0; }
        static bool Alt() { return GetKeyState(VK_MENU) < 0; }

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);
        void Render();
        bool EnsureTarget();
        void LoadColors();

        bool m_opaque = false;
        HWND m_hwnd = nullptr;
        UINT m_dpi = 96;
        SIZE m_minDip{};
        Microsoft::WRL::ComPtr<ID2D1Factory> m_d2d;
        Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_rt;
        unsigned m_generation = 0;
        int m_hot = -1, m_pressed = -1;   // buttons
    };
}
