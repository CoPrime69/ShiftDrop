#pragma once
#include <windows.h>
#include <d2d1.h>
#include <wil/resource.h>
#include <wrl/client.h>

namespace sd
{
    // Draws a per-pixel-alpha layered window: Direct2D renders into a 32bpp DIB, and
    // UpdateLayeredWindow hands that bitmap to DWM. Used by the wheel and the top panel.
    class Canvas
    {
    public:
        explicit Canvas(ID2D1Factory* factory) : m_d2d(factory) {}

        // Starts a frame of `size` pixels, cleared to transparent. nullptr if drawing is impossible.
        ID2D1RenderTarget* Begin(SIZE size);
        // Ends the frame and shows it at `origin` (screen px) with `opacity` applied to the whole window.
        // Returns false if the device was lost; the next Begin() rebuilds the target, and device
        // bitmaps must be recreated (DeviceGeneration() changes).
        bool End(HWND hwnd, POINT origin, float opacity);
        unsigned DeviceGeneration() const { return m_generation; }

    private:
        bool CreateSurface(SIZE size);

        ID2D1Factory* m_d2d;
        Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_rt;
        unsigned m_generation = 0;
        // Declaration order matters: m_select restores the DC's old bitmap before m_bitmap is deleted.
        wil::unique_hdc m_dc;
        wil::unique_hbitmap m_bitmap;
        wil::unique_select_object m_select;
        SIZE m_size{};
    };
}
