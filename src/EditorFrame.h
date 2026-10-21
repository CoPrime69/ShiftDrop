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

// The frame every Direct2D editor window shares: the PDF page view and Merge list, the picture editor
// (canvas) and the video and audio timeline editor. An ordinary resizable top-level window drawn
// with Direct2D in DIPs at the monitor's DPI, in the editor palette (theme::EditorColors, light or
// dark like the rest of the app, title bar included), with cached brushes and text formats, and
// push buttons drawn and hit-tested here. Modeless: each window owns itself and deletes itself when
// it is destroyed. UI thread only.
namespace sd
{
    class EditorFrame
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
        bool Create(const std::wstring& title, int widthDip, int heightDip, int minWidthDip, int minHeightDip, bool show = true);
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

        const theme::EditorPalette& Theme() const { return m_colors; }
        ID2D1Factory* Factory() const { return m_d2d.Get(); }
        IDWriteFactory* DWrite() const { return m_dwrite.Get(); }
        ID2D1RenderTarget* Target() const { return m_rt.Get(); }
        // Device-dependent resources (brushes, bitmaps) must be rebuilt when this changes.
        unsigned DeviceGeneration() const { return m_generation; }

        ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& color);
        // Cached one-line formats, vertically centred, that end in "..." when the text doesn't fit.
        IDWriteTextFormat* TextFormat(float sizeDip, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
        IDWriteTextFormat* IconFormat(float sizeDip);
        void DrawText(std::wstring_view text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);
        float TextWidth(std::wstring_view text, float sizeDip, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);

        std::vector<Button> m_buttons;
        void DrawButtons(ID2D1RenderTarget* rt);
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
        theme::EditorPalette m_colors{};
        Microsoft::WRL::ComPtr<ID2D1Factory> m_d2d;
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwrite;
        Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_rt;
        unsigned m_generation = 0;
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
        };
        std::vector<CachedFormat> m_formats;
        std::wstring m_textFamily, m_iconFamily;
        int m_hot = -1, m_pressed = -1;   // buttons
    };
}
