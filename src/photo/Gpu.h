#pragma once
#include "canvas/Raster.h"

#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>

// Direct2D off screen: the device the photo tools' exports (and the tests) render on, and the
// bitmap upload / read-back helpers the preview shares. One device for the process: Direct3D 11 on
// the GPU, else WARP (Windows' software rasteriser, which runs the same shaders), on a
// multithreaded Direct2D factory so each worker can draw with its own device context.
namespace sd::photo::gpu
{
    using Microsoft::WRL::ComPtr;

    class Device
    {
    public:
        // The shared device, made on first use; null when there is none (no D3D11 at all: some
        // virtual machines, Wine without a display). After `Lost()` the next call makes a new one.
        static std::shared_ptr<Device> Get();
        static void Lost(const Device* device);

        ComPtr<ID2D1DeviceContext> NewContext() const;   // 96 DPI, so DIPs are pixels
        ID2D1Factory1* Factory() const { return m_factory.Get(); }
        bool Hardware() const { return m_hardware; }
        const std::wstring& Name() const { return m_name; }   // "GPU", "WARP" (for logs)

    private:
        ComPtr<ID2D1Factory1> m_factory;
        ComPtr<ID2D1Device> m_device;
        bool m_hardware = false;
        std::wstring m_name;
    };

    // A premultiplied BGRA bitmap of a straight-alpha raster, usable as an effect input or drawn.
    ComPtr<ID2D1Bitmap1> Upload(ID2D1DeviceContext* dc, const canvas::Raster& raster, HRESULT* hr = nullptr);
    // A 128-bit float bitmap (RGBA, alpha 1) from up to three planes of floats.
    ComPtr<ID2D1Bitmap1> UploadFloat(ID2D1DeviceContext* dc, unsigned width, unsigned height, const float* r, const float* g, const float* b);
    // A target bitmap to draw into.
    ComPtr<ID2D1Bitmap1> NewTarget(ID2D1DeviceContext* dc, unsigned width, unsigned height, HRESULT* hr = nullptr);
    // Copies a target back to the CPU as straight-alpha BGRA.
    HRESULT ReadBack(ID2D1DeviceContext* dc, ID2D1Bitmap1* target, canvas::Raster& out);

    // Draws into a new `width` x `height` target with `dc` (cleared to transparent, identity
    // transform) and reads the result back. The context's own target is restored.
    HRESULT RenderToRaster(ID2D1DeviceContext* dc, unsigned width, unsigned height,
                           const std::function<HRESULT(ID2D1DeviceContext*)>& draw, canvas::Raster& out);

    // The largest bitmap this context can make (16384 on Direct3D 11 hardware and WARP).
    unsigned MaxEdge(ID2D1DeviceContext* dc);

    std::wstring HrText(HRESULT hr);   // "0x887A0005"
}
