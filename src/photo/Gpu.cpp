#include "photo/Gpu.h"
#include "Log.h"

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <mutex>

namespace sd::photo::gpu
{
    namespace
    {
        std::mutex g_lock;
        std::shared_ptr<Device> g_device;
        bool g_failed = false;   // no device could be made: don't retry for every picture

        bool MakeD3D(D3D_DRIVER_TYPE type, ComPtr<ID3D11Device>& device)
        {
            // BGRA support is what Direct2D needs; the feature levels default to 11.1 .. 9.1.
            return SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                               &device, nullptr, nullptr));
        }
    }

    std::shared_ptr<Device> Device::Get()
    {
        std::lock_guard lock(g_lock);
        if (g_device || g_failed)
            return g_device;
        auto d = std::make_shared<Device>();
        ComPtr<ID3D11Device> d3d;
        if (MakeD3D(D3D_DRIVER_TYPE_HARDWARE, d3d))
        {
            d->m_hardware = true;
            d->m_name = L"GPU";
        }
        else if (MakeD3D(D3D_DRIVER_TYPE_WARP, d3d))
            d->m_name = L"WARP";
        ComPtr<IDXGIDevice> dxgi;
        D2D1_FACTORY_OPTIONS options{};
        if (!d3d || FAILED(d3d.As(&dxgi)) ||
            FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &options,
                                     reinterpret_cast<void**>(d->m_factory.GetAddressOf()))) ||
            FAILED(d->m_factory->CreateDevice(dxgi.Get(), &d->m_device)))
        {
            log::Warn(L"photo: no Direct2D device (no Direct3D 11 here); photo tools render on the CPU where they can");
            g_failed = true;
            return nullptr;
        }
        log::Info(L"photo: rendering on {}", d->m_name);
        g_device = std::move(d);
        return g_device;
    }

    void Device::Lost(const Device* device)
    {
        std::lock_guard lock(g_lock);
        if (g_device.get() == device)
        {
            log::Warn(L"photo: the Direct2D device was lost; making a new one");
            g_device.reset();
        }
    }

    ComPtr<ID2D1DeviceContext> Device::NewContext() const
    {
        ComPtr<ID2D1DeviceContext> dc;
        if (m_device && SUCCEEDED(m_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)))
            dc->SetDpi(96.f, 96.f);
        return dc;
    }

    ComPtr<ID2D1Bitmap1> Upload(ID2D1DeviceContext* dc, const canvas::Raster& raster, HRESULT* hr)
    {
        ComPtr<ID2D1Bitmap1> bitmap;
        if (!dc || raster.Empty())
        {
            if (hr)
                *hr = E_INVALIDARG;
            return bitmap;
        }
        // Direct2D bitmaps are premultiplied.
        std::vector<uint8_t> pm(raster.px.size());
        for (size_t i = 0; i < pm.size(); i += 4)
        {
            const unsigned a = raster.px[i + 3];
            for (int c = 0; c < 3; ++c)
                pm[i + c] = static_cast<uint8_t>((raster.px[i + c] * a + 127) / 255);
            pm[i + 3] = static_cast<uint8_t>(a);
        }
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        const HRESULT result = dc->CreateBitmap(D2D1::SizeU(raster.width, raster.height), pm.data(), raster.width * 4, props, &bitmap);
        if (hr)
            *hr = result;
        return bitmap;
    }

    ComPtr<ID2D1Bitmap1> UploadFloat(ID2D1DeviceContext* dc, unsigned width, unsigned height, const float* r, const float* g, const float* b)
    {
        ComPtr<ID2D1Bitmap1> bitmap;
        if (!dc || width == 0 || height == 0)
            return bitmap;
        std::vector<float> px(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
        {
            px[i * 4 + 0] = r ? r[i] : 0.f;
            px[i * 4 + 1] = g ? g[i] : 0.f;
            px[i * 4 + 2] = b ? b[i] : 0.f;
            px[i * 4 + 3] = 1.f;
        }
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                   D2D1::PixelFormat(DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(width, height), px.data(), width * 16, props, &bitmap)))
            bitmap.Reset();
        return bitmap;
    }

    ComPtr<ID2D1Bitmap1> NewTarget(ID2D1DeviceContext* dc, unsigned width, unsigned height, HRESULT* hr)
    {
        ComPtr<ID2D1Bitmap1> target;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        const HRESULT result = dc ? dc->CreateBitmap(D2D1::SizeU(std::max(1u, width), std::max(1u, height)), nullptr, 0, props, &target) : E_POINTER;
        if (hr)
            *hr = result;
        return target;
    }

    HRESULT ReadBack(ID2D1DeviceContext* dc, ID2D1Bitmap1* target, canvas::Raster& out)
    {
        const auto size = target->GetPixelSize();
        // The GPU copy goes into a CPU-readable bitmap, which is then mapped.
        ComPtr<ID2D1Bitmap1> staging;
        const auto props = D2D1::BitmapProperties1(static_cast<D2D1_BITMAP_OPTIONS>(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW),
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (HRESULT hr = dc->CreateBitmap(size, nullptr, 0, props, &staging); FAILED(hr))
            return hr;
        if (HRESULT hr = staging->CopyFromBitmap(nullptr, target, nullptr); FAILED(hr))
            return hr;
        D2D1_MAPPED_RECT mapped{};
        if (HRESULT hr = staging->Map(D2D1_MAP_OPTIONS_READ, &mapped); FAILED(hr))
            return hr;
        const double dpiX = out.dpiX, dpiY = out.dpiY;
        auto icc = std::move(out.icc);
        if (!out.Allocate(size.width, size.height))
        {
            staging->Unmap();
            return E_OUTOFMEMORY;
        }
        out.dpiX = dpiX;
        out.dpiY = dpiY;
        out.icc = std::move(icc);
        for (UINT y = 0; y < size.height; ++y)
        {
            const uint8_t* src = mapped.bits + static_cast<size_t>(y) * mapped.pitch;
            uint8_t* dst = out.At(0, y);
            for (UINT x = 0; x < size.width; ++x, src += 4, dst += 4)
            {
                const unsigned a = src[3];
                for (int c = 0; c < 3; ++c)
                    dst[c] = a == 255 ? src[c] : a ? static_cast<uint8_t>(std::min(255u, (src[c] * 255u + a / 2) / a)) : 0;
                dst[3] = static_cast<uint8_t>(a);
            }
        }
        staging->Unmap();
        return S_OK;
    }

    HRESULT RenderToRaster(ID2D1DeviceContext* dc, unsigned width, unsigned height, const std::function<HRESULT(ID2D1DeviceContext*)>& draw,
                           canvas::Raster& out)
    {
        HRESULT hr = S_OK;
        auto target = NewTarget(dc, width, height, &hr);
        if (!target)
            return hr;
        ComPtr<ID2D1Image> previous;
        dc->GetTarget(&previous);
        dc->SetTarget(target.Get());
        dc->BeginDraw();
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        HRESULT drew = draw(dc);
        hr = dc->EndDraw();
        dc->SetTarget(previous.Get());
        if (FAILED(drew))
            return drew;
        if (FAILED(hr))
            return hr;
        return ReadBack(dc, target.Get(), out);
    }

    unsigned MaxEdge(ID2D1DeviceContext* dc) { return dc ? dc->GetMaximumBitmapSize() : 0; }
}
