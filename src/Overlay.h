#pragma once
#include "Canvas.h"
#include "DropTarget.h"
#include "HiddenCursor.h"
#include "Settings.h"
#include "Theme.h"
#include "keyboard/KeyNav.h"
#include "keyboard/WheelLayout.h"

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
        std::wstring headline;   // if set, replaces the centre's big text while hovered (Reduce size: the new size)
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

    // What the preview card needs to know about the highlighted slice.
    struct WheelHighlight
    {
        std::wstring actionId;                              // empty: nothing (or a dimmed slice) is highlighted
        const std::vector<std::wstring>* files = nullptr;   // the wheel's files, valid during the call
        POINT centre{};                                     // the wheel, screen px
        float radius = 0.f;                                 // its outer edge, px
        HMONITOR monitor = nullptr;
        float scale = 1.f;                                  // px per DIP
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
    //
    // Up to 8 slices sit on one ring, 9 to 16 on two, more page through a "More" slice
    // (keyboard/WheelLayout.h). The keyboard picks slices too (Navigate / Activate, fed by
    // KeyboardController): a slice picked by key stays lit until the mouse really moves.
    //
    // Besides a drag, the wheel opens in "click mode" on files given to it (Explorer's selection, the
    // right-click entry): hover or arrows to pick, click or Enter to run, Esc or a click outside to close.
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
            // The highlighted slice changed (hover or keyboard): what the preview card shows.
            std::function<void(const struct WheelHighlight&)> highlighted;
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

        // Click mode: the wheel opens at `screenPt` on `files` without a drag.
        void ShowForFiles(POINT screenPt, int ring, std::vector<std::wstring> files);
        bool InClickMode() const { return m_click && m_mode == Mode::Wheel; }
        bool ShowsWheel() const { return m_mode == Mode::Wheel; }   // the probe has become the real wheel
        bool Contains(POINT screenPt) const;                         // over the tiles (or the centre)

        // Keyboard picking. Activate runs the highlighted slice (as a drop would) or turns the page.
        enum class Activation { None, Paged, Ran };
        void Navigate(keynav::Key key);
        Activation Activate();
        void Cancel();                   // Esc in click mode, or a click outside

        void SetRing(int ring);          // another wheel's key was pressed
        // The tray's Features menu turned sections on or off. Only while hidden (the tray menu is up).
        void SetRings(std::vector<WheelRing> rings);
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
        static constexpr int kOutside = wheel::kOutside;
        static constexpr int kCenter = wheel::kCentre;

        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        void SetMode(Mode mode);
        void Layout(POINT screenPt, int rings);
        void EnsureRings(int rings);       // grows the window (around the same centre) for two rings
        void Arrange(int page);            // m_layout and m_shown for m_content
        int HitTest(POINT screenPt) const;
        int PointerSlot(POINT screenPt);   // HitTest, but the opening spot of a moved wheel is the centre
        POINT ClampCentre(POINT screenPt, float extentPx) const;   // keeps a wheel this big on the monitor
        bool Open(const std::vector<std::wstring>& files, POINT pt);   // the probe becomes the wheel
        void SetHot(int hot, bool byKey = false);
        bool Selectable(int slot) const;   // an enabled slice or "More"
        void Run(int slot);                // drop / click / Enter on a slot
        bool EndSink();                    // the drag ended after a slice ran by key
        void NotifyHighlight();
        int NextRing(int step) const;      // the next wheel in order, wrapping around
        void NextPage();
        void OnClickMove(POINT screenPt);
        void OnClickUp(POINT screenPt);
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
        void DrawSlices(const std::vector<WheelSlice>& slices, const wheel::Layout& layout, const std::vector<float>& hotT, float alpha, float rotation);
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
        D2D1_POINT_2F m_c{};          // wheel centre: the cursor when it opened, moved in near a screen edge
        POINT m_openPt{};             // the cursor when the wheel opened, screen px
        bool m_shifted = false;       // the wheel was moved in from an edge, so it is not centred on m_openPt
        bool m_edgeGuard = false;     // ... and the cursor has not moved off m_openPt yet
        float m_radius = 0.f;         // outer edge of the tiles (hit-testing stops here)
        float m_inner = 0.f;          // centre dead zone: dropping here cancels
        float m_tileIn = 0.f, m_tileOut = 0.f;   // inner edge of the inner ring, outer edge of the outermost
        int m_windowRings = 1;                  // rings the window is sized for
        std::map<uint64_t, Microsoft::WRL::ComPtr<ID2D1Geometry>> m_tileCache;

        // State.
        Mode m_mode = Mode::Hidden;
        bool m_captureScroll = false;
        bool m_inDrag = false;
        bool m_announced = false;     // events.shown sent for this showing
        int m_ring = 0;
        int m_hot = kCenter;
        int m_detail = -1;            // slot whose detail the centre shows (kept while it fades out)
        int m_scrollAccum = 0;
        WheelContent m_content;       // every slice for m_files on this wheel
        wheel::Layout m_layout, m_prevLayout;
        std::vector<WheelSlice> m_shown, m_prevShown;   // the visible slots (this page, plus "More")
        bool m_keyHot = false;        // m_hot was picked by key: the mouse takes over only when it moves
        POINT m_keyAnchor{};          // where the cursor was when the key picked it
        bool m_click = false;         // click mode (no drag)
        bool m_tracking = false;      // TrackMouseEvent armed (click mode)
        bool m_sink = false;          // ran by key mid-drag: keep catching the drop until the drag ends
        std::wstring m_lastHighlight;
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
        POINT m_backdropOrigin{};   // screen px of the snapshot's top-left (it covers the largest wheel)
        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_backdrop;
        unsigned m_backdropGeneration = 0;
    };
}
