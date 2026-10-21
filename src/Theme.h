#pragma once
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
namespace sd::theme
{
    struct Palette
    {
        bool dark = true;
        D2D1_COLOR_F accent, accentHi, textOnAccent;
        D2D1_COLOR_F text, textDim, shadow;
        D2D1_COLOR_F tile, tileLine, centre, centreLine;   // wheel tiles and centre
        D2D1_COLOR_F glassTint;                            // over the blurred backdrop (Glass style)
        D2D1_COLOR_F panel, panelLine, panelText, panelTextDim, keycap;   // top panel
        D2D1_COLOR_F ok, warn, error;
    };
    // From the Windows light/dark app setting (or the Settings window's override) and accent colour,
    // read each time a wheel opens so a theme change applies without a restart.
    Palette Current(Style style);
    // Settings > Theme: follow Windows, or always light or dark. Every window that asks Current()
    // follows it: the wheel, the top panel, the shelf, Settings and the editors. UI thread.
    void SetChoice(ThemeChoice choice);

    // The colours of the Direct2D editors (the PDF page view and Merge list, the picture editor, the
    // video and audio timeline editor), light or dark like everything else that asks Current(), with
    // the accent adjusted so it reads well on them.
    struct EditorPalette
    {
        bool dark = true;
        D2D1_COLOR_F window;              // behind everything; the title bar is tinted with it
        D2D1_COLOR_F panel;               // side panels, lists and grids
        D2D1_COLOR_F stage, track;        // the picture and the timeline wells
        D2D1_COLOR_F line, text, textDim, textFaint;
        D2D1_COLOR_F control, controlHover, controlDown, controlLine;   // buttons and fields
        D2D1_COLOR_F accent, accentHover, onAccent;
        D2D1_COLOR_F accentSoft;          // the selection
        D2D1_COLOR_F danger, dangerSoft, focus, shadow;
    };
    EditorPalette EditorColors();

    // The title bar of `hwnd` light or dark; on Windows 11 also filled with `caption` (left as
    // Windows draws it by default).
    void ApplyDarkCaption(HWND hwnd, bool dark, COLORREF caption = CLR_INVALID);
    COLORREF ToColorRef(const D2D1_COLOR_F& c);

    // How wide `text` is in `format` (with `typography`, such as tabular figures, if given), in DIPs.
    float TextWidth(IDWriteFactory* dwrite, std::wstring_view text, IDWriteTextFormat* format, IDWriteTypography* typography = nullptr);

    // The GDI colours of the windows built from standard controls (Settings, History, Rename, the
    // inspector), light or dark like everything else that asks Current(). `lightBackground` is the
    // system colour (COLOR_WINDOW, COLOR_BTNFACE) the window has in light mode.
    struct DialogColors
    {
        bool dark = false;
        COLORREF background = 0;   // the window behind the controls
        COLORREF text = 0;
        COLORREF field = 0;        // edit boxes, drop-down lists and list views
    };
    DialogColors Dialog(int lightBackground = COLOR_WINDOW);

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
