#include "Brand.h"
#include "Overlay.h"
#include "Log.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include <cmath>
#include <numbers>

using Microsoft::WRL::ComPtr;

namespace sd
{
    namespace
    {
        constexpr wchar_t kClassName[] = L"" SD_NAME L".Overlay";
        constexpr float kOuterDip = 120.f;   // wheel radius in device-independent pixels
        constexpr float kInnerDip = 38.f;    // centre dead zone: dropping here cancels
        constexpr float kMarginDip = 6.f;

        D2D1_COLOR_F Rgba(UINT rgb, float a)
        {
            return D2D1::ColorF(rgb, a);
        }

        bool AppsUseLightTheme()
        {
            DWORD value = 1, size = sizeof(value);
            RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
            return value != 0;
        }
    }

    Overlay::Overlay(HINSTANCE instance, ContentProvider provider, DropHandler onDrop)
        : m_instance(instance), m_provider(std::move(provider)), m_onDrop(std::move(onDrop))
    {
    }

    Overlay::~Overlay() { Destroy(); }

    bool Overlay::Create()
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = m_instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);

        // WS_EX_NOACTIVATE + WM_MOUSEACTIVATE=MA_NOACTIVATE: showing or touching the wheel never takes
        // focus from the app you are dragging out of. WS_EX_TOOLWINDOW keeps it off the taskbar/Alt+Tab.
        m_hwnd.reset(CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                     kClassName, SD_NAME, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, m_instance, this));
        if (!m_hwnd)
        {
            log::Error(L"overlay: CreateWindowExW failed ({})", GetLastError());
            return false;
        }

        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.ReleaseAndGetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(m_dwrite.ReleaseAndGetAddressOf()))))
        {
            log::Error(L"overlay: Direct2D/DirectWrite unavailable");
            return false;
        }

        m_dropTarget = Microsoft::WRL::Make<DropTarget>(m_hwnd.get(), this);
        const HRESULT hr = RegisterDragDrop(m_hwnd.get(), m_dropTarget.Get());
        if (FAILED(hr))
        {
            log::Error(L"overlay: RegisterDragDrop failed (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }
        m_registered = true;

        // About the Windows 11 acrylic backdrop (DWMWA_SYSTEMBACKDROP_TYPE): DWM only applies system
        // backdrops to windows it composes itself. A per-pixel-alpha layered window (UpdateLayeredWindow)
        // supplies its own finished bitmap, so the attribute has no effect here and the backdrop is
        // always rectangular anyway. We therefore draw a translucent fill. See PLAN.md, "Glass look".
        log::Info(L"overlay: ready (translucent fill; system acrylic does not apply to UpdateLayeredWindow windows)");
        return true;
    }

    void Overlay::Destroy()
    {
        if (m_registered && m_hwnd)
            RevokeDragDrop(m_hwnd.get());   // releases OLE's reference to m_dropTarget
        m_registered = false;
        m_hwnd.reset();
        m_dropTarget.Reset();
    }

    LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTCLIENT;   // the alpha-0 pixels are already excluded by the layered-window hit test
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    void Overlay::Layout(POINT screenPt)
    {
        UINT dpiX = 96, dpiY = 96;
        GetDpiForMonitor(MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        m_scale = static_cast<float>(dpiX) / 96.f;
        m_radius = std::round(kOuterDip * m_scale);
        m_inner = std::round(kInnerDip * m_scale);
        const int half = static_cast<int>(m_radius + std::round(kMarginDip * m_scale));
        m_center = screenPt;
        // Centred on the cursor even near a screen edge: shifting the wheel would put the cursor on a
        // slice instead of the dead zone, and a drop without moving would then trigger an action.
        m_origin = POINT{ screenPt.x - half, screenPt.y - half };
        m_size = SIZE{ half * 2, half * 2 };
    }

    void Overlay::ShowProbe(POINT screenPt)
    {
        if (!m_hwnd)
            return;
        Layout(screenPt);
        m_palette = CurrentPalette();
        m_mode = Mode::Probe;
        m_hot = kCenter;
        m_files.clear();
        m_content = {};
        Render();
        // Topmost above other topmost windows, without activation.
        SetWindowPos(m_hwnd.get(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        log::Info(L"overlay: probe at ({}, {}), scale {:.2f}", screenPt.x, screenPt.y, m_scale);
    }

    void Overlay::Hide()
    {
        if (m_hwnd && m_mode != Mode::Hidden)
            ShowWindow(m_hwnd.get(), SW_HIDE);
        m_mode = Mode::Hidden;
    }

    int Overlay::HitTest(POINT pt) const
    {
        const float dx = static_cast<float>(pt.x - m_center.x);
        const float dy = static_cast<float>(pt.y - m_center.y);
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > m_radius)
            return kOutside;
        if (d < m_inner || m_content.slices.empty())
            return kCenter;
        float deg = std::atan2(dx, -dy) * 180.f / std::numbers::pi_v<float>;   // 0 at top, clockwise
        if (deg < 0)
            deg += 360.f;
        const auto n = static_cast<int>(m_content.slices.size());
        const float span = 360.f / static_cast<float>(n);
        return static_cast<int>(std::floor((deg + span / 2.f) / span)) % n;
    }

    // ---- IDropSink -------------------------------------------------------------------------------

    bool Overlay::OnDragEnter(const std::vector<std::wstring>& files, POINT pt)
    {
        m_inDrag = true;
        if (m_mode == Mode::Hidden)
            return false;
        if (files.empty())
        {
            log::Info(L"overlay: drag has no CF_HDROP (text, URL or virtual file); hiding");
            Hide();
            return false;
        }
        m_files = files;
        m_content = m_provider ? m_provider(files) : WheelContent{};
        m_mode = Mode::Wheel;
        m_hot = HitTest(pt);
        Render();
        log::Info(L"overlay: DragEnter with {} file(s), first: {}", files.size(), files.front());
        return true;
    }

    DWORD Overlay::OnDragOver(POINT pt, DWORD allowed)
    {
        if (m_mode != Mode::Wheel)
            return DROPEFFECT_NONE;
        const int hot = HitTest(pt);
        if (hot != m_hot)
        {
            m_hot = hot;
            Render();
        }
#if SHIFTDROP_M0
        return hot != kOutside ? SafeEffect(allowed) : DROPEFFECT_NONE;
#else
        // Centre and disabled slices answer NONE, so the cursor shows "not allowed" and releasing there
        // ends in DragLeave instead of Drop: that is the cancel gesture.
        if (hot >= 0 && m_content.slices[static_cast<size_t>(hot)].enabled)
            return SafeEffect(allowed);
        return DROPEFFECT_NONE;
#endif
    }

    void Overlay::OnDragLeave()
    {
        m_inDrag = false;
        if (m_mode != Mode::Hidden)
            log::Info(L"overlay: DragLeave (cursor left the wheel, Esc, or released on the centre); hiding");
        Hide();
    }

    void Overlay::OnDrop(const std::vector<std::wstring>& files, POINT pt)
    {
        m_inDrag = false;
        const int hot = HitTest(pt);
        const auto content = m_content;
        Hide();
        if (!m_onDrop || files.empty())
            return;
#if SHIFTDROP_M0
        (void)hot;
        (void)content;
        m_onDrop(files, L"");
#else
        if (hot >= 0 && content.slices[static_cast<size_t>(hot)].enabled)
            m_onDrop(files, content.slices[static_cast<size_t>(hot)].actionId);
#endif
    }

    // ---- Drawing ---------------------------------------------------------------------------------

    Overlay::Palette Overlay::CurrentPalette()
    {
        // Accent colour from DWM (ARGB). Falls back to a neutral blue.
        DWORD argb = 0xFF2463EB;
        BOOL opaque = FALSE;
        DwmGetColorizationColor(&argb, &opaque);
        const UINT accent = argb & 0x00FFFFFF;

        Palette p{};
        if (AppsUseLightTheme())
        {
            p.disc = Rgba(0xF3F3F3, 0.86f);
            p.border = Rgba(0x000000, 0.12f);
            p.center = Rgba(0xFFFFFF, 0.92f);
            p.text = Rgba(0x1A1A1A, 1.f);
            p.textDim = Rgba(0x1A1A1A, 0.35f);
            p.separator = Rgba(0x000000, 0.10f);
        }
        else
        {
            p.disc = Rgba(0x202020, 0.84f);
            p.border = Rgba(0xFFFFFF, 0.14f);
            p.center = Rgba(0x2C2C2C, 0.94f);
            p.text = Rgba(0xFFFFFF, 1.f);
            p.textDim = Rgba(0xFFFFFF, 0.35f);
            p.separator = Rgba(0xFFFFFF, 0.10f);
        }
        p.hot = Rgba(accent, 0.90f);
        return p;
    }

    bool Overlay::EnsureSurface()
    {
        if (!m_dc || m_surface.cx != m_size.cx || m_surface.cy != m_size.cy)
        {
            if (!CreateSurface())
                return false;
        }
        if (!m_rt)
        {
            // Premultiplied BGRA is exactly what UpdateLayeredWindow(ULW_ALPHA) expects. DPI is pinned
            // to 96 so one D2D unit is one physical pixel; we do the DPI scaling ourselves in Layout().
            const auto props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
            if (FAILED(m_d2d->CreateDCRenderTarget(&props, m_rt.ReleaseAndGetAddressOf())))
                return false;
        }
        return true;
    }

    bool Overlay::CreateSurface()
    {
        m_select.reset();
        m_bitmap.reset();
        m_dc.reset(CreateCompatibleDC(nullptr));
        if (!m_dc)
            return false;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = m_size.cx;
        bi.bmiHeader.biHeight = -m_size.cy;   // negative = top-down rows
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        m_bitmap.reset(CreateDIBSection(m_dc.get(), &bi, DIB_RGB_COLORS, &bits, nullptr, 0));
        if (!m_bitmap)
            return false;
        m_select = wil::SelectObject(m_dc.get(), m_bitmap.get());
        m_surface = m_size;
        return true;
    }

    bool Overlay::EnsureTextFormats()
    {
        if (m_sliceFont && m_fontScale == m_scale)
            return true;
        m_fontScale = m_scale;
        auto make = [&](float dip, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out) {
            if (FAILED(m_dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, dip * m_scale, L"", out.ReleaseAndGetAddressOf())))
                return false;
            out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            return true;
        };
        return make(15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, m_sliceFont) && make(11.f, DWRITE_FONT_WEIGHT_NORMAL, m_centerFont);
    }

    D2D1_POINT_2F Overlay::Polar(float radius, float degrees) const
    {
        const float c = static_cast<float>(m_size.cx) / 2.f;
        const float rad = degrees * std::numbers::pi_v<float> / 180.f;
        return D2D1::Point2F(c + radius * std::sin(rad), c - radius * std::cos(rad));
    }

    ComPtr<ID2D1PathGeometry> Overlay::SliceGeometry(float a0, float a1) const
    {
        ComPtr<ID2D1PathGeometry> geometry;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(m_d2d->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink)))
            return nullptr;
        const auto arc = (a1 - a0) > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
        sink->BeginFigure(Polar(m_radius, a0), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddArc(D2D1::ArcSegment(Polar(m_radius, a1), D2D1::SizeF(m_radius, m_radius), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, arc));
        sink->AddLine(Polar(m_inner, a1));
        sink->AddArc(D2D1::ArcSegment(Polar(m_inner, a0), D2D1::SizeF(m_inner, m_inner), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, arc));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        return geometry;
    }

    void Overlay::DrawLabel(const std::wstring& text, D2D1_POINT_2F at, IDWriteTextFormat* format, const D2D1_COLOR_F& color)
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        m_rt->CreateSolidColorBrush(color, &brush);
        const float w = 80.f * m_scale, h = 40.f * m_scale;
        m_rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format,
                       D2D1::RectF(at.x - w / 2, at.y - h / 2, at.x + w / 2, at.y + h / 2), brush.Get());
    }

    void Overlay::DrawWheel()
    {
        const float c = static_cast<float>(m_size.cx) / 2.f;
        const auto centre = D2D1::Point2F(c, c);
        ComPtr<ID2D1SolidColorBrush> disc, border, centreFill, hot, separator;
        m_rt->CreateSolidColorBrush(m_palette.disc, &disc);
        m_rt->CreateSolidColorBrush(m_palette.border, &border);
        m_rt->CreateSolidColorBrush(m_palette.center, &centreFill);
        m_rt->CreateSolidColorBrush(m_palette.hot, &hot);
        m_rt->CreateSolidColorBrush(m_palette.separator, &separator);

        m_rt->FillEllipse(D2D1::Ellipse(centre, m_radius, m_radius), disc.Get());

#if SHIFTDROP_M0
        // M0 placeholder: a plain disc; any drop on it is logged.
        m_rt->DrawEllipse(D2D1::Ellipse(centre, m_radius - 0.5f, m_radius - 0.5f), border.Get(), 1.f);
        DrawLabel(SD_NAME, D2D1::Point2F(c, c - 10.f * m_scale), m_sliceFont.Get(), m_palette.text);
        DrawLabel(m_content.centerText, D2D1::Point2F(c, c + 12.f * m_scale), m_centerFont.Get(), m_palette.textDim);
#else
        const auto n = m_content.slices.size();
        const float span = n ? 360.f / static_cast<float>(n) : 360.f;
        for (size_t i = 0; i < n; ++i)
        {
            const float a0 = static_cast<float>(i) * span - span / 2.f;
            const auto& slice = m_content.slices[i];
            if (static_cast<int>(i) == m_hot && slice.enabled)
            {
                if (auto g = SliceGeometry(a0, a0 + span))
                    m_rt->FillGeometry(g.Get(), hot.Get());
            }
            m_rt->DrawLine(Polar(m_inner, a0), Polar(m_radius - 1.f, a0), separator.Get(), 1.f);
            DrawLabel(slice.label, Polar((m_inner + m_radius) / 2.f, a0 + span / 2.f), m_sliceFont.Get(),
                      slice.enabled ? m_palette.text : m_palette.textDim);
        }
        m_rt->DrawEllipse(D2D1::Ellipse(centre, m_radius - 0.5f, m_radius - 0.5f), border.Get(), 1.f);
        m_rt->FillEllipse(D2D1::Ellipse(centre, m_inner, m_inner), centreFill.Get());
        m_rt->DrawEllipse(D2D1::Ellipse(centre, m_inner, m_inner), separator.Get(), 1.f);
        DrawLabel(m_content.centerText, centre, m_centerFont.Get(), m_palette.textDim);
#endif
    }

    void Overlay::Render()
    {
        if (!m_hwnd || !EnsureSurface() || !EnsureTextFormats())
            return;

        RECT rc{ 0, 0, m_size.cx, m_size.cy };
        if (FAILED(m_rt->BindDC(m_dc.get(), &rc)))
            return;
        m_rt->BeginDraw();
        m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
        m_rt->Clear(D2D1::ColorF(0, 0.f));

        if (m_mode == Mode::Probe)
        {
            // Alpha ~2%: invisible to the eye, but non-zero so the disc is hit-testable for OLE.
            ComPtr<ID2D1SolidColorBrush> probe;
            m_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0.02f), &probe);
            const float c = static_cast<float>(m_size.cx) / 2.f;
            m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), m_radius, m_radius), probe.Get());
        }
        else if (m_mode == Mode::Wheel)
        {
            DrawWheel();
        }

        if (m_rt->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET))
        {
            m_rt.Reset();   // device lost: the render target is rebuilt on the next Render
            return;
        }

        POINT src{ 0, 0 };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(m_hwnd.get(), nullptr, &m_origin, &m_size, m_dc.get(), &src, 0, &blend, ULW_ALPHA);
    }
}
