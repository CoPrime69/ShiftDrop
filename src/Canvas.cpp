#include "Canvas.h"

#include <algorithm>
#include <cmath>

namespace sd
{
    bool Canvas::CreateSurface(SIZE size)
    {
        m_select.reset();
        m_bitmap.reset();
        m_dc.reset(CreateCompatibleDC(nullptr));
        if (!m_dc)
            return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = size.cx;
        bi.bmiHeader.biHeight = -size.cy;   // negative = top-down rows
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        m_bitmap.reset(CreateDIBSection(m_dc.get(), &bi, DIB_RGB_COLORS, &bits, nullptr, 0));
        if (!m_bitmap)
            return false;
        m_select = wil::SelectObject(m_dc.get(), m_bitmap.get());
        m_size = size;
        return true;
    }

    ID2D1RenderTarget* Canvas::Begin(SIZE size)
    {
        if (size.cx <= 0 || size.cy <= 0)
            return nullptr;
        if ((!m_dc || m_size.cx != size.cx || m_size.cy != size.cy) && !CreateSurface(size))
            return nullptr;
        if (!m_rt)
        {
            // Premultiplied BGRA is exactly what UpdateLayeredWindow(ULW_ALPHA) expects. DPI is pinned
            // to 96 so one D2D unit is one physical pixel; callers do their own DPI scaling.
            const auto props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
            if (FAILED(m_d2d->CreateDCRenderTarget(&props, m_rt.ReleaseAndGetAddressOf())))
                return nullptr;
            // ClearType needs an opaque background to blend against; on a transparent surface it
            // leaves coloured fringes, so text is antialiased in grayscale.
            m_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            ++m_generation;
        }
        RECT rc{ 0, 0, size.cx, size.cy };
        if (FAILED(m_rt->BindDC(m_dc.get(), &rc)))
            return nullptr;
        m_rt->BeginDraw();
        m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
        m_rt->Clear(D2D1::ColorF(0, 0.f));
        return m_rt.Get();
    }

    bool Canvas::End(HWND hwnd, POINT origin, float opacity)
    {
        if (!m_rt)
            return false;
        if (m_rt->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET))
        {
            m_rt.Reset();
            return false;
        }
        POINT src{ 0, 0 };
        const auto alpha = static_cast<BYTE>(std::lround(255.f * std::clamp(opacity, 0.f, 1.f)));
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
        UpdateLayeredWindow(hwnd, nullptr, &origin, &m_size, m_dc.get(), &src, 0, &blend, ULW_ALPHA);
        return true;
    }
}
