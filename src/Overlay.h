#pragma once
#include "Canvas.h"
#include "DropTarget.h"
#include "HiddenCursor.h"
#include "Settings.h"
#include "Theme.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sd
{
    struct WheelSlice
    {
        std::wstring label;      // e.g. "PNG"
        std::wstring actionId;   // e.g. "to-png"
        bool enabled = true;     // false: drawn dimmed, dropping on it is refused
        wchar_t icon = 0;        // Segoe Fluent Icons / MDL2 glyph; 0 = text only (format names)
        bool mirrorIcon = false; // e.g. rotate left drawn as a mirrored rotate right
        std::wstring detail;     // shown in the centre while the slice is hovered, e.g. "WebP, quality 85"
    };

    struct WheelContent
    {
        std::vector<WheelSlice> slices;   // slice 0 is at 12 o'clock, then clockwise
        std::wstring centerText;          // e.g. "3 files", or why nothing applies
        // Nothing is enabled, but the slices are meant for these files and the centre says why they
        // can't run (the media engine is missing). Such a wheel is opened and scrolled to like one
        // with actions, so the reason is seen.
        bool explains = false;
    };

    // One of the wheels you can switch between (shown in the top panel and as dots in the centre).
    struct WheelRing
    {
        std::wstring title;   // "Convert"
        std::wstring key;     // "Shift"; empty when no key opens it (still reachable by scrolling)
        wchar_t icon = 0;
        // Opened by this wheel's key but nothing on it applies to the dragged files (a PDF on the image
        // Convert wheel): open the first wheel that has something instead.
        bool fallThrough = false;
    };

    // The radial menu: a topmost, non-activating, layered popup centred on the cursor.
    //
    // It is drawn with Direct2D and pushed with UpdateLayeredWindow (per-pixel alpha, see Canvas).
    // For a layered window, pixels with alpha 0 are invisible to hit-testing, so drops on the
    // transparent corners fall through to whatever is underneath; everything we want to receive the
    // drop (including the gaps between tiles) must be painted with alpha > 0. It is deliberately NOT
    // WS_EX_TRANSPARENT while open, which would make it click-through and OLE would never find our
    // drop target.
    //
    // Switching wheels while it is open: press another wheel's key (main.cpp calls SetRing) or turn
    // the mouse wheel (the hook forwards it to OnScroll, see MouseHook.h).
    class Overlay final : public IDropSink
    {
    public:
        using ContentProvider = std::function<WheelContent(const std::vector<std::wstring>& files, int ring)>;
        using DropHandler = std::function<void(std::vector<std::wstring> files, std::wstring actionId)>;
        struct Events
        {
            std::function<void(int ring, HMONITOR monitor)> shown;   // the wheel appeared (files arrived)
            std::function<void()> hidden;                            // dropped, cancelled or dismissed
            std::function<void(int ring)> ringChanged;
        };

        Overlay(HINSTANCE instance, std::vector<WheelRing> rings, ContentProvider provider, DropHandler onDrop, Events events);
        ~Overlay();
        Overlay(const Overlay&) = delete;
        Overlay& operator=(const Overlay&) = delete;

        bool Create();     // window, D2D/DirectWrite factories, RegisterDragDrop. Needs OleInitialize first.
        void Destroy();
        void SetAppearance(const Appearance& appearance) { m_appearance = appearance; }

        // Shows an almost invisible, hit-testable disc under the cursor. The real wheel is drawn only
        // once OLE delivers DragEnter with files, so a Shift+drag that is not a file drag (selecting
        // text, moving a window) never flashes a menu.
        void ShowProbe(POINT screenPt, int ring);
        void Hide();       // at once, no animation
        bool IsVisible() const { return m_mode == Mode::Probe || m_mode == Mode::Wheel; }
        bool InDrag() const { return m_inDrag; }   // between DragEnter and DragLeave/Drop

        void SetRing(int ring);          // another wheel's key was pressed
        void OnScroll(int wheelDelta);   // WM_MOUSEWHEEL delta (120 per notch; touchpads send less)
        // The mouse hook swallows wheel events while this is true, so the window under the cursor
        // does not scroll while the wheel is in use. Points into this object; valid until Destroy().
        const bool* ScrollCaptureFlag() const { return &m_captureScroll; }

        // IDropSink
        bool OnDragEnter(const std::vector<std::wstring>& files, POINT pt) override;
        DWORD OnDragOver(POINT pt, DWORD allowed) override;
        void OnDragLeave() override;
        void OnDrop(const std::vector<std::wstring>& files, POINT pt) override;

    private:
        enum class Mode { Hidden, Probe, Wheel, Closing };
        static constexpr int kOutside = -1;
        static constexpr int kCenter = -2;

        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        void SetMode(Mode mode);
        void Layout(POINT screenPt);
        int HitTest(POINT screenPt) const;
        void SetHot(int hot);
        void SwitchTo(int ring, int direction);
        bool HasActions(int ring) const;   // any enabled slice for m_files (or WheelContent::explains)
        void BeginClose();
        void OnFrame();
        bool Settled() const;
        void Animate();
        void NotifyHidden();

        bool EnsureTextFormats();
        Microsoft::WRL::ComPtr<IDWriteTextFormat> MakeFormat(const std::wstring& family, float dip, DWRITE_FONT_WEIGHT weight) const;
        void Render();
        void DrawScene();
        void DrawSlices(const WheelContent& content, const std::vector<float>& hotT, float alpha, float rotation);
        void DrawCentre();
        D2D1_POINT_2F Polar(float radius, float degrees) const;   // 0° = 12 o'clock, clockwise
        Microsoft::WRL::ComPtr<ID2D1Geometry> TileGeometry(size_t count, size_t index, float inner, float outer);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> Brush(const D2D1_COLOR_F& color, float alpha = 1.f);
        void Text(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float alpha = 1.f);

        HINSTANCE m_instance;
        std::vector<WheelRing> m_rings;
        ContentProvider m_provider;
        DropHandler m_onDrop;
        Events m_events;
        Appearance m_appearance;

        wil::unique_hwnd m_hwnd;
        Microsoft::WRL::ComPtr<DropTarget> m_dropTarget;
        bool m_registered = false;

        Microsoft::WRL::ComPtr<ID2D1Factory> m_d2d;
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwrite;
        std::unique_ptr<Canvas> m_canvas;
        ID2D1RenderTarget* m_rt = nullptr;   // valid between Canvas::Begin and End
        std::wstring m_textFamily, m_iconFamily;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_labelFont, m_formatFont, m_iconFont, m_countFont, m_ringFont, m_detailFont;
        float m_fontScale = 0.f;

        // Layout, in window pixels.
        HMONITOR m_monitor = nullptr;
        float m_scale = 1.f;          // pixels per DIP (monitor DPI and the Size setting)
        POINT m_origin{};             // window top-left, screen px
        SIZE m_size{};
        D2D1_POINT_2F m_c{};          // wheel centre: the cursor when it opened
        float m_radius = 0.f;         // outer edge of the tiles (hit-testing stops here)
        float m_inner = 0.f;          // centre dead zone: dropping here cancels
        float m_tileIn = 0.f, m_tileOut = 0.f;
        std::map<uint64_t, Microsoft::WRL::ComPtr<ID2D1Geometry>> m_tileCache;

        // State.
        Mode m_mode = Mode::Hidden;
        bool m_captureScroll = false;
        bool m_inDrag = false;
        bool m_announced = false;     // events.shown sent for this showing
        int m_ring = 0;
        int m_hot = kCenter;
        int m_detail = -1;            // slice whose detail the centre shows (kept while it fades out)
        int m_scrollAccum = 0;
        WheelContent m_content, m_previous;
        std::vector<std::wstring> m_files;
        POINT m_lastPt{};
        theme::Palette m_palette{};

        // Animation: all 0..1. Driven by a ~64 fps timer that stops once everything has settled.
        bool m_animating = false;
        bool m_motion = true;         // Windows' animation effects setting
        ULONGLONG m_lastFrame = 0;
        float m_openT = 1.f, m_switchT = 1.f, m_closeT = 0.f;
        float m_away = 0.f, m_awayTarget = 0.f;   // cursor past the tiles toward the window edge: the wheel fades
        bool m_dropped = false;                   // closing after a drop (not because the cursor left)
        int m_switchDir = 1;
        std::vector<float> m_hotT;

        HiddenCursor m_cursor;   // the pointer can be hidden over the wheel (tray menu)

        // Glass style: the screen under the window, captured when the wheel opens, shrunk and blurred.
        theme::Pixels m_backdropPx;
        float m_backdropStep = 1.f;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_backdrop;
        unsigned m_backdropGeneration = 0;
    };
}
