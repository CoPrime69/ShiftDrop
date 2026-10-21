#pragma once
#include "Canvas.h"
#include "Theme.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <memory>
#include <string>

namespace sd
{
    // What the layered panel windows share: the wheel (Overlay), the top panel (Island), the Float
    // shelf and the preview card. Each is a WS_POPUP layered window drawn with Direct2D into a Canvas,
    // with Segoe text and icon fonts. They derive from this privately and keep their own window
    // procedure, layout and drawing; this holds the factories, the canvas, the fonts and the few
    // drawing helpers they all had a copy of.
    class PanelWindow
    {
    protected:
        PanelWindow() = default;
        ~PanelWindow() = default;
        PanelWindow(const PanelWindow&) = delete;
        PanelWindow& operator=(const PanelWindow&) = delete;

        // Registers `className` and creates its hidden WS_POPUP window, WS_EX_LAYERED plus `exStyle`,
        // with `self` as the creation parameter (the window procedure keeps it on WM_NCCREATE).
        static HWND CreatePanel(HINSTANCE instance, const wchar_t* className, WNDPROC proc, DWORD exStyle, const wchar_t* title,
                                void* self, LPCWSTR cursor = IDC_ARROW, UINT classStyle = 0);
        // The Direct2D and DirectWrite factories, the canvas and the font families. False when
        // Direct2D or DirectWrite is unavailable.
        bool InitGraphics();

        // A one-line text format of `dip` DIPs at m_scale, centred vertically, that ends in "…" when
        // the text does not fit.
        Microsoft::WRL::ComPtr<IDWriteTextFormat> MakeFormat(const std::wstring& family, float dip, DWRITE_FONT_WEIGHT weight,
                                                             DWRITE_TEXT_ALIGNMENT align) const;
        // How wide `text` is drawn in `format`, rounded up to a whole pixel.
        float TextWidth(const std::wstring& text, IDWriteTextFormat* format) const;
        // Between Canvas::Begin and End (m_rt set).
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> Brush(const D2D1_COLOR_F& color, float alpha = 1.f);
        void Text(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float alpha = 1.f);
        // The brand's file mark (a page with a rounded-off corner), as large as fits in `box`, centred;
        // `done` adds the accent corner that says "made by ShiftDrop". For a file that has no picture
        // of its own: the top panel's result, the result card, the shelf.
        void DrawFileMark(const D2D1_RECT_F& box, const theme::Palette& palette, bool done, float alpha = 1.f);

        Microsoft::WRL::ComPtr<ID2D1Factory> m_d2d;
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwrite;
        std::unique_ptr<Canvas> m_canvas;
        ID2D1RenderTarget* m_rt = nullptr;   // valid between Canvas::Begin and End
        std::wstring m_textFamily, m_iconFamily;
        float m_scale = 1.f;       // pixels per DIP (monitor DPI and the Size setting)
        float m_fontScale = 0.f;   // the scale the window's text formats were made at
    };

    // The panels' frame timer. 10 ms rounds up to the system timer tick (~15.6 ms), so animations run
    // at about 64 frames a second; each window stops it once everything has settled.
    class FrameClock
    {
    public:
        bool Running() const { return m_running; }
        void Start(HWND hwnd, UINT_PTR timerId);   // nothing while it runs
        void Stop(HWND hwnd, UINT_PTR timerId);
        // On each WM_TIMER: the milliseconds since the last frame, at most 50 so a stall does not jump.
        float Tick(ULONGLONG now);

    private:
        bool m_running = false;
        ULONGLONG m_last = 0;
    };
}
