#pragma once
#include "Accent.h"
#include "Settings.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Colours, sizes, motion and the frosted-glass snapshot shared by the wheel and the top panel.
//
// Every colour a window draws with comes from here: the brand's colours (Brand Guidelines > Colour)
// in `brand`, the tokens of one theme built from them (Tokens), and what each kind of window takes
// from those (Palette for the wheel, the top panel, the result card and the shelf; EditorPalette for
// the Direct2D editors; DialogColors for the windows of standard controls). UI files never write a
// colour of their own. The builders (TokensFor, MakePalette, MakeEditorColors, MakeDialog) are pure,
// in ThemeTokens.cpp, so the unit tests check every text colour reads on what it sits on.
namespace sd::theme
{
    // The kit's colours, 0xRRGGBB.
    namespace brand
    {
        inline constexpr uint32_t kInk = 0x15171E;        // N900: text, the mark
        inline constexpr uint32_t kN0 = 0xFFFFFF;         // surface
        inline constexpr uint32_t kN25 = 0xF6F7F9;        // page
        inline constexpr uint32_t kN50 = 0xEEF0F3;        // surface 2
        inline constexpr uint32_t kN100 = 0xE3E5EA;       // border
        inline constexpr uint32_t kN200 = 0xD5D8DE;       // strong border
        inline constexpr uint32_t kN500 = 0x6B7180;       // text 3
        inline constexpr uint32_t kN600 = 0x4A4F5A;       // text 2
        inline constexpr uint32_t kN800 = 0x22252D;       // icons
        inline constexpr uint32_t kN950 = 0x0E1014;       // dark page
        inline constexpr uint32_t kDarkText = 0xF2F3F5;   // the dark theme's text, text 2 and text 3
        inline constexpr uint32_t kDarkText2 = 0xB4B9C3;
        inline constexpr uint32_t kDarkText3 = 0x8A909C;
        inline constexpr uint32_t kDarkBorder = 0x2A2E37;
        // Status, on light and on dark. The kit has no info colour: it is the Blue preset's tones.
        inline constexpr uint32_t kOk = 0x1D7F4A, kOkOnDark = 0x4ADE80;
        inline constexpr uint32_t kWarn = 0x8A5A00, kWarnOnDark = 0xF5C04A;
        inline constexpr uint32_t kError = 0xC02B2B, kErrorOnDark = 0xFF7A7A;
        inline constexpr uint32_t kInfo = 0x0067C0, kInfoOnDark = 0x60A5FA;
    }

    D2D1_COLOR_F Rgb(uint32_t rgb, float alpha = 1.f);   // 0xRRGGBB
    uint32_t RgbOf(const D2D1_COLOR_F& c);                // back to 0xRRGGBB, alpha dropped

    // One theme's colours for the windows you work in (App Windows and App Editors pages): Settings,
    // History, Rename, the inspector, the question dialogs and the editors.
    struct Tokens
    {
        bool dark = false;
        D2D1_COLOR_F window;         // behind everything (Mica-like grey); the title bar is tinted with it
        D2D1_COLOR_F card;           // a card on the window: one setting, a side panel, a list
        D2D1_COLOR_F cardLine;       // the card's 1 px edge
        D2D1_COLOR_F divider;        // between the rows of a card and the sections of a panel
        D2D1_COLOR_F navSelected;    // the selected navigation item or tab, behind its text
        D2D1_COLOR_F text;           // body text and labels
        D2D1_COLOR_F subtleText;     // descriptions under a setting, secondary values
        D2D1_COLOR_F faintText;      // placeholders and hints, on a card or the window
        D2D1_COLOR_F control, controlHover, controlDown;   // buttons, segmented choices, drop-downs
        D2D1_COLOR_F controlLine;    // their edge
        D2D1_COLOR_F field;          // an edit box
        D2D1_COLOR_F border;         // around thumbnails and list rows
        D2D1_COLOR_F strongBorder;   // a switch's or a check box's outline when off
        D2D1_COLOR_F stage;          // the picture's well in an editor
        D2D1_COLOR_F track;          // the timeline's well
        D2D1_COLOR_F paper;          // a light card in both themes, for dark ink (signatures, PDF pages)
        D2D1_COLOR_F paperText, paperFaint, paperLine;   // text, hints and rules on paper
        D2D1_COLOR_F ok, warn, error, info;              // status text and marks, readable on the window and cards
        D2D1_COLOR_F shadow;         // floating windows and menus
    };
    Tokens TokensFor(bool dark);

    // How well `text` reads on `background` (WCAG 2 contrast ratio, 1 to 21), each first laid over
    // what is under it: `background` over `under`, `text` over that.
    double Contrast(const D2D1_COLOR_F& text, const D2D1_COLOR_F& background, const D2D1_COLOR_F& under = D2D1::ColorF(0xFFFFFF));
    D2D1_COLOR_F Over(const D2D1_COLOR_F& top, const D2D1_COLOR_F& bottom);   // `top` composited on `bottom`, opaque if it is

    // The always-on surfaces (App UI page): the wheel, the top panel, the result card, the shelf.
    struct Palette
    {
        bool dark = true;
        D2D1_COLOR_F accent, accentHi, textOnAccent;   // the hovered tile and the active section: fill, its rim, its text
        D2D1_COLOR_F accentMark;                       // the current wheel's dot and progress: the accent as a mark on the theme
        D2D1_COLOR_F text, textDim, shadow;            // labels; a dimmed tile's label; the wheel's shadow
        D2D1_COLOR_F subtleText;                       // the centre's detail line and the card's sizes
        D2D1_COLOR_F disc, discLine;                   // the round plate behind the tiles
        D2D1_COLOR_F tile, tileLine, centre, centreLine;   // wheel tiles and centre
        D2D1_COLOR_F dot;                              // the other wheels' dots in the centre
        D2D1_COLOR_F glassTint;                        // over the blurred backdrop (Glass style)
        D2D1_COLOR_F panel, panelLine, panelText, panelTextDim, keycap;   // top panel, result card and shelf
        D2D1_COLOR_F panelAccent, panelTextOnAccent;   // the accent on the top panel: the active section and the done mark
        D2D1_COLOR_F hover;                            // a hovered row or button on the panel or the shelf
        D2D1_COLOR_F ok, warn, error, info;            // status, readable on the panel
        D2D1_COLOR_F track;                            // under a progress bar and a spinner's arc
        D2D1_COLOR_F page, pageLine, pageMark;         // the brand's file mark: a page, its edge, the done corner (DrawFileMark)
    };
    // From the Windows light/dark app setting (or the Settings window's override) and the accent colour
    // (accent::Current(), Settings › Accent colour), read each time a wheel opens so a change applies
    // without a restart.
    Palette Current(Style style);
    Palette MakePalette(Style style, bool dark, const accent::Variants& tones);
    // Settings > Theme: follow Windows, or always light or dark. Every window that asks Current()
    // follows it: the wheel, the top panel, the shelf, Settings and the editors. UI thread.
    void SetChoice(ThemeChoice choice);
    bool IsDark();   // the theme Current() uses now

    // The colours of the Direct2D editors (the PDF page view and Merge list, the picture editor, the
    // video and audio timeline editor), light or dark like everything else that asks Current(), with
    // the accent's tone for that theme (accent::Variants) so text on it and next to it reads.
    struct EditorPalette
    {
        bool dark = true;
        D2D1_COLOR_F window;              // behind everything; the title bar is tinted with it
        D2D1_COLOR_F panel;               // side panels, lists and grids (a card)
        D2D1_COLOR_F stage, track;        // the picture and the timeline wells
        D2D1_COLOR_F line, text, textDim, textFaint;   // dividers; text; secondary text; hints
        D2D1_COLOR_F control, controlHover, controlDown, controlLine;   // buttons and fields
        D2D1_COLOR_F field;               // an edit box while you type in it
        D2D1_COLOR_F thumb;               // a slider's knob
        D2D1_COLOR_F selected;            // the selected tab or tool, behind its label
        D2D1_COLOR_F border;              // around thumbnails and rows
        D2D1_COLOR_F accent, accentHover, onAccent;
        D2D1_COLOR_F accentSoft;          // the selection
        D2D1_COLOR_F danger, dangerSoft, onDanger, focus, shadow;
        D2D1_COLOR_F ok, warn, info;      // status text
        D2D1_COLOR_F paper, paperText, paperFaint, paperLine;   // a light card for dark ink, in both themes (Tokens)
    };
    EditorPalette EditorColors();
    EditorPalette MakeEditorColors(bool dark, const accent::Variants& tones);

    // The title bar of `hwnd` light or dark; on Windows 11 also filled with `caption` (left as
    // Windows draws it by default).
    void ApplyDarkCaption(HWND hwnd, bool dark, COLORREF caption = CLR_INVALID);
    COLORREF ToColorRef(const D2D1_COLOR_F& c);

    // How wide `text` is in `format` (with `typography`, such as tabular figures, if given), in DIPs.
    float TextWidth(IDWriteFactory* dwrite, std::wstring_view text, IDWriteTextFormat* format, IDWriteTypography* typography = nullptr);

    // The GDI colours of the windows built from standard controls (Settings, History, Rename, the
    // inspector), light or dark like everything else that asks Current(): the Tokens, laid over the
    // window where they are see-through. `lightBackground` is the system colour the window had in
    // light mode: COLOR_WINDOW windows are a card (white), COLOR_BTNFACE ones the grey window.
    struct DialogColors
    {
        bool dark = false;
        COLORREF background = 0;   // the window behind the controls
        COLORREF text = 0;
        COLORREF field = 0;        // edit boxes, drop-down lists and list views
        COLORREF card = 0;         // one setting's card
        COLORREF cardLine = 0;     // its edge
        COLORREF divider = 0;      // between rows and sections
        COLORREF navSelected = 0;  // the selected navigation item
        COLORREF subtleText = 0;   // a setting's description
        COLORREF muted = 0;        // hints, and what no longer counts (an undone run)
        COLORREF problem = 0;      // what can't be used or went wrong (a name Rename refuses, a failed run)
        COLORREF ok = 0, warn = 0, info = 0;
        COLORREF accentText = 0;   // links and the accent as text
    };
    DialogColors Dialog(int lightBackground = COLOR_WINDOW);
    DialogColors MakeDialog(bool dark, const accent::Variants& tones, int lightBackground = COLOR_WINDOW);

    // Pixels per DIP for a monitor: its DPI scale times the size preset. Auto also follows the
    // monitor's height in DIPs, so a 1440p screen gets a larger wheel than a 1080p one at the same
    // DPI (within limits, so it never gets silly on huge or tiny screens).
    float ScaleFor(HMONITOR monitor, WheelSize size);

    // Windows' "Animation effects" setting (Settings > Accessibility > Visual effects). Off: no
    // springs or slides, things just appear.
    bool AnimationsOn();

    struct Pixels   // CPU copy of an image, so D2D bitmaps can be rebuilt after a device loss
    {
        std::vector<uint32_t> px;   // premultiplied BGRA, top-down
        UINT w = 0, h = 0;
    };
    // The screen inside `rect` (physical pixels), shrunk by `step` and blurred. Used for the Glass
    // style; kept in memory only. Must be called before our window covers that area.
    Pixels CaptureBlurred(const RECT& rect, int step);
    Microsoft::WRL::ComPtr<ID2D1Bitmap> ToBitmap(ID2D1RenderTarget* rt, const Pixels& pixels);
    // The shell's thumbnail of a file (Explorer's cache, so usually instant), or its icon, at most
    // `px` pixels on its longer side. Empty when the shell has none. Worker threads with COM, too.
    Pixels ShellThumbnail(const std::wstring& path, UINT px);

    // Windows 11 has Segoe UI Variable and Segoe Fluent Icons; Windows 10 has Segoe UI and Segoe
    // MDL2 Assets, with the same icon code points.
    std::wstring TextFamily(IDWriteFactory* dwrite);
    std::wstring IconFamily(IDWriteFactory* dwrite);

    D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t);
    D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& c, float alphaFactor);
    float EaseOutCubic(float t);
    float EaseInOutCubic(float t);
    // A lightly damped spring from 0 to 1 over t = 0..1: overshoots by about 8% and settles.
    float Spring(float t);
}
