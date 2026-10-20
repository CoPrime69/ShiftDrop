#pragma once
#include "DropTarget.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <vector>

namespace sd
{
    struct WheelSlice
    {
        std::wstring label;      // e.g. "PNG"
        std::wstring actionId;   // e.g. "wic.to.png"
        bool enabled = true;     // false: drawn dimmed, dropping on it is refused
    };

    struct WheelContent
    {
        std::vector<WheelSlice> slices;   // slice 0 is at 12 o'clock, then clockwise
        std::wstring centerText;          // e.g. "3 files"
    };

    // The radial menu: a topmost, non-activating, layered popup centred on the cursor.
    //
    // It is drawn with Direct2D into a 32-bit DIB and pushed with UpdateLayeredWindow (per-pixel
    // alpha). For a layered window, pixels with alpha 0 are invisible to hit-testing, so drops on the
    // transparent corners fall through to whatever is underneath; the wheel itself must be painted
    // with alpha > 0 everywhere we want to receive the drop. It is deliberately NOT WS_EX_TRANSPARENT,
    // which would make the whole window click-through and OLE would never find our drop target.
    class Overlay final : public IDropSink
    {
    public:
        using ContentProvider = std::function<WheelContent(const std::vector<std::wstring>& files)>;
        using DropHandler = std::function<void(std::vector<std::wstring> files, std::wstring actionId)>;

        Overlay(HINSTANCE instance, ContentProvider provider, DropHandler onDrop);
        ~Overlay();
        Overlay(const Overlay&) = delete;
        Overlay& operator=(const Overlay&) = delete;

        bool Create();     // window, D2D/DirectWrite factories, RegisterDragDrop. Needs OleInitialize first.
        void Destroy();

        // Shows an almost invisible, hit-testable disc under the cursor. The real wheel is drawn only
        // once OLE delivers DragEnter with files, so a Shift+drag that is not a file drag (selecting
        // text, moving a window) never flashes a menu.
        void ShowProbe(POINT screenPt);
        void Hide();
        bool IsVisible() const { return m_mode != Mode::Hidden; }
        bool InDrag() const { return m_inDrag; }   // between DragEnter and DragLeave/Drop

        // IDropSink
        bool OnDragEnter(const std::vector<std::wstring>& files, POINT pt) override;
        DWORD OnDragOver(POINT pt, DWORD allowed) override;
        void OnDragLeave() override;
        void OnDrop(const std::vector<std::wstring>& files, POINT pt) override;

    private:
        enum class Mode { Hidden, Probe, Wheel };
        static constexpr int kOutside = -1;
        static constexpr int kCenter = -2;

        struct Palette
        {
            D2D1_COLOR_F disc, border, center, hot, text, textDim, separator;
        };

        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        int HitTest(POINT screenPt) const;
        void Layout(POINT screenPt);
        bool EnsureSurface();
        bool CreateSurface();
        bool EnsureTextFormats();
        void Render();
        void DrawWheel();
        D2D1_POINT_2F Polar(float radius, float degrees) const;   // 0° = 12 o'clock, clockwise
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> SliceGeometry(float a0, float a1) const;
        void DrawLabel(const std::wstring& text, D2D1_POINT_2F at, IDWriteTextFormat* format, const D2D1_COLOR_F& color);
        static Palette CurrentPalette();

        HINSTANCE m_instance;
        ContentProvider m_provider;
        DropHandler m_onDrop;

        wil::unique_hwnd m_hwnd;
        Microsoft::WRL::ComPtr<DropTarget> m_dropTarget;
        bool m_registered = false;

        Microsoft::WRL::ComPtr<ID2D1Factory> m_d2d;
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwrite;
        Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_rt;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_sliceFont;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_centerFont;
        float m_fontScale = 0.f;

        // Off-screen 32bpp top-down DIB that D2D draws into and UpdateLayeredWindow reads from.
        // Declaration order matters: m_select restores the DC's old bitmap before m_bitmap is deleted.
        wil::unique_hdc m_dc;
        wil::unique_hbitmap m_bitmap;
        wil::unique_select_object m_select;
        SIZE m_surface{};

        Mode m_mode = Mode::Hidden;
        bool m_inDrag = false;
        POINT m_center{};      // screen px
        POINT m_origin{};      // window top-left, screen px
        SIZE m_size{};         // window size, px
        float m_scale = 1.f;   // monitor DPI / 96
        float m_radius = 0.f;  // outer radius, px
        float m_inner = 0.f;   // dead-zone radius, px
        int m_hot = kCenter;
        WheelContent m_content;
        std::vector<std::wstring> m_files;
        Palette m_palette{};
    };
}
