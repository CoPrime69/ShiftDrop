#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <vector>

// The frame shared by the PDF editor windows (Merge, the page view): an ordinary resizable top-level
// window drawn with Direct2D in DIPs, following the Windows light/dark setting and accent colour,
// with push buttons drawn and hit-tested here. Modeless: each window owns itself and deletes itself
// when it is closed. UI thread only.
namespace sd::pdf
{
    class EditorWindow
    {
    public:
        EditorWindow(const EditorWindow&) = delete;
        EditorWindow& operator=(const EditorWindow&) = delete;

    protected:
        EditorWindow();
        virtual ~EditorWindow();

        // Creates the window (and shows it, unless `show` is false), sized in DIPs for the monitor
        // under the cursor. On failure the caller still owns the object.
        bool Create(const std::wstring& title, int widthDip, int heightDip, int minWidthDip, int minHeightDip, bool show = true);
        void Close();   // destroys the window, then the object

        struct Colors
        {
            bool dark = false;
            D2D1_COLOR_F background, surface, line, text, textDim, accent, textOnAccent, selection, hover;
        };

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
        virtual void Paint(ID2D1RenderTarget* rt, D2D1_SIZE_F size) = 0;
        virtual void Layout(D2D1_SIZE_F) {}
        // Window messages before the base handles them; return true when handled.
        virtual bool OnMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) = 0;
        // The window is going away (Esc, ×, or Close()): a last chance to stop background work.
        virtual void OnClosing() {}

        HWND Hwnd() const { return m_hwnd; }
        float Scale() const { return m_dpi / 96.f; }
        D2D1_SIZE_F ClientDip() const;
        D2D1_POINT_2F DipFromLParam(LPARAM lParam) const;
        void Invalidate() const;

        const Colors& Theme() const { return m_colors; }
        ID2D1RenderTarget* Target() const { return m_rt.Get(); }
        ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& color);
        IDWriteTextFormat* TextFormat(float sizeDip, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
        IDWriteTextFormat* IconFormat(float sizeDip);
        IDWriteFactory* DWrite() const { return m_dwrite.Get(); }
        void DrawText(std::wstring_view text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);

        std::vector<Button> m_buttons;
        void DrawButtons(ID2D1RenderTarget* rt);
        // The button under a point, or -1.
        int ButtonAt(D2D1_POINT_2F p) const;
        // Device-dependent resources (bitmaps) must be rebuilt when this changes.
        unsigned DeviceGeneration() const { return m_generation; }

        static bool Ctrl() { return GetKeyState(VK_CONTROL) < 0; }
        static bool Shift() { return GetKeyState(VK_SHIFT) < 0; }
        static bool Alt() { return GetKeyState(VK_MENU) < 0; }

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);
        void Render();
        bool EnsureTarget();
        void LoadColors();

        HWND m_hwnd = nullptr;
        UINT m_dpi = 96;
        SIZE m_minDip{};
        Colors m_colors{};
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
