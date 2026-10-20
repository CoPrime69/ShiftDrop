#pragma once
#include "Canvas.h"
#include "Overlay.h"
#include "Settings.h"
#include "Theme.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sd
{
    // The panel at the top centre of the screen (Windows has no notch, so it hangs from the top
    // edge of the monitor's work area instead).
    //
    // While a wheel is open it shows every wheel, the one you are on, its key and that scrolling
    // switches. After a drop it turns into the job's progress and then its result: a success tucks
    // itself away after two seconds, a failure stays until clicked. Clicking a result shows the
    // files in Explorer.
    //
    // A layered, topmost, non-activating window like the wheel. It is click-through
    // (WS_EX_TRANSPARENT) except while it shows a result, so it never gets in the way of the
    // window under it, and it is never a drop target.
    class Island
    {
    public:
        enum class Outcome { Success, Partial, Failure };

        Island(HINSTANCE instance, std::vector<WheelRing> rings, std::function<void()> onClick);
        ~Island();
        Island(const Island&) = delete;
        Island& operator=(const Island&) = delete;

        bool Create();
        void SetAppearance(const Appearance& appearance) { m_appearance = appearance; }

        void ShowSwitcher(int ring, HMONITOR monitor);   // a wheel opened on `monitor`
        void SetRing(int ring);
        void HideSwitcher();                             // the wheel closed and nothing was dropped
        void ShowProgress(const std::wstring& title, size_t total);   // "Converting 4 files…"
        void SetProgress(size_t done);
        void ShowResult(Outcome outcome, const std::wstring& title, const std::wstring& subtitle);

    private:
        enum class State { Hidden, Switcher, Progress, Result };

        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        void Present(State state, HMONITOR monitor);
        void Retract();
        void Layout(HMONITOR monitor);
        void SetClickThrough(bool through);
        void Animate();
        void OnFrame();
        float ContentWidth() const;
        float ContentHeight() const;

        Microsoft::WRL::ComPtr<IDWriteTextFormat> MakeFormat(const std::wstring& family, float dip, DWRITE_FONT_WEIGHT weight,
                                                             DWRITE_TEXT_ALIGNMENT align) const;
        bool EnsureTextFormats();
        float TextWidth(const std::wstring& text, IDWriteTextFormat* format) const;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> Brush(const D2D1_COLOR_F& color, float alpha = 1.f);
        void Text(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float alpha = 1.f);
        void Render();
        void DrawSwitcher(const D2D1_RECT_F& box, float alpha);
        void DrawProgress(const D2D1_RECT_F& box, float alpha);
        void DrawResult(const D2D1_RECT_F& box, float alpha);

        HINSTANCE m_instance;
        std::vector<WheelRing> m_rings;
        std::function<void()> m_onClick;
        Appearance m_appearance;

        wil::unique_hwnd m_hwnd;
        Microsoft::WRL::ComPtr<ID2D1Factory> m_d2d;
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwrite;
        std::unique_ptr<Canvas> m_canvas;
        ID2D1RenderTarget* m_rt = nullptr;   // valid between Canvas::Begin and End
        std::wstring m_textFamily, m_iconFamily;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_tabFont, m_tabIconFont, m_keyFont, m_hintFont, m_titleFont, m_subFont, m_statusIconFont;
        float m_fontScale = 0.f;

        // Layout, window pixels.
        HMONITOR m_monitor = nullptr;
        float m_scale = 1.f;
        POINT m_origin{};
        SIZE m_size{};
        float m_pad = 0.f;   // transparent room around the panel for its shadow
        theme::Palette m_palette{};
        theme::Pixels m_backdropPx;
        float m_backdropStep = 1.f;
        RECT m_backdropRect{};
        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_backdrop;
        unsigned m_backdropGeneration = 0;

        // Content.
        State m_state = State::Hidden;
        int m_ring = 0;
        std::wstring m_title, m_subtitle;
        size_t m_total = 0, m_done = 0;
        Outcome m_outcome = Outcome::Success;

        // Animation.
        bool m_animating = false;
        bool m_motion = true;
        bool m_retracting = false;
        ULONGLONG m_lastFrame = 0;
        ULONGLONG m_resultSince = 0;
        float m_presence = 0.f;      // 0 = tucked above the top edge, 1 = fully down (spring)
        float m_presenceT = 0.f;     // time along the spring, 0..1
        float m_w = 0.f, m_h = 0.f;  // current panel size, easing toward the content's size
        float m_contentT = 1.f;      // fade-in of the current state's content
        float m_tabPos = 0.f;        // sliding highlight in the switcher (fractional ring index)
        float m_bar = 0.f;           // shown progress, easing toward m_done / m_total
        float m_spin = 0.f;          // spinner angle, degrees
    };
}
