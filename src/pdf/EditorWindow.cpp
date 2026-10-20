#include "pdf/EditorWindow.h"
#include "Brand.h"
#include "Log.h"
#include "Theme.h"
#include "resource.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

namespace sd::pdf
{
    namespace
    {
        constexpr wchar_t kClass[] = L"" SD_NAME L".PdfEditor";

        D2D1_COLOR_F Rgb(uint32_t rgb, float alpha = 1.f)
        {
            return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, alpha);
        }

        bool Same(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
    }

    EditorWindow::EditorWindow()
    {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwrite.GetAddressOf()));
        if (m_dwrite)
        {
            m_textFamily = theme::TextFamily(m_dwrite.Get());
            m_iconFamily = theme::IconFamily(m_dwrite.Get());
        }
        LoadColors();
    }

    EditorWindow::~EditorWindow() = default;

    void EditorWindow::LoadColors()
    {
        const auto palette = theme::Current(Style::Solid);
        Colors c;
        c.dark = palette.dark;
        c.accent = palette.accent;
        c.textOnAccent = palette.textOnAccent;
        if (c.dark)
        {
            c.background = Rgb(0x202020);
            c.surface = Rgb(0x2D2D2D);
            c.line = Rgb(0xFFFFFF, 0.10f);
            c.text = Rgb(0xFFFFFF);
            c.textDim = Rgb(0xFFFFFF, 0.62f);
            c.selection = theme::WithAlpha(palette.accent, 0.30f);
            c.hover = Rgb(0xFFFFFF, 0.06f);
        }
        else
        {
            c.background = Rgb(0xF3F3F3);
            c.surface = Rgb(0xFFFFFF);
            c.line = Rgb(0x000000, 0.10f);
            c.text = Rgb(0x1A1A1A);
            c.textDim = Rgb(0x000000, 0.60f);
            c.selection = theme::WithAlpha(palette.accent, 0.22f);
            c.hover = Rgb(0x000000, 0.04f);
        }
        m_colors = c;
        if (m_hwnd)
        {
            const BOOL dark = c.dark;
            DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        }
    }

    bool EditorWindow::Create(const std::wstring& title, int widthDip, int heightDip, int minWidthDip, int minHeightDip)
    {
        static const bool registered = [] {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
            wc.lpszClassName = kClass;
            return RegisterClassExW(&wc) != 0;
        }();
        if (!registered || !m_d2d || !m_dwrite)
            return false;
        m_minDip = { minWidthDip, minHeightDip };

        // Open on the monitor the user is working on (where the drop happened).
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT work = monitor.rcWork;
        m_hwnd = CreateWindowExW(0, kClass, title.c_str(), WS_OVERLAPPEDWINDOW, work.left + 40, work.top + 40, 400, 300, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), this);
        if (!m_hwnd)
            return false;
        LoadColors();   // the title bar's dark mode needs the window
        m_dpi = GetDpiForWindow(m_hwnd);
        RECT r{ 0, 0, MulDiv(widthDip, static_cast<int>(m_dpi), 96), MulDiv(heightDip, static_cast<int>(m_dpi), 96) };
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, m_dpi);
        const int w = std::min<int>(r.right - r.left, work.right - work.left);
        const int h = std::min<int>(r.bottom - r.top, work.bottom - work.top);
        SetWindowPos(m_hwnd, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(m_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(m_hwnd);   // we are answering the user's drop
        return true;
    }

    void EditorWindow::Close()
    {
        if (m_hwnd)
        {
            OnClosing();
            DestroyWindow(m_hwnd);   // WM_NCDESTROY deletes this
        }
    }

    D2D1_SIZE_F EditorWindow::ClientDip() const
    {
        RECT r{};
        GetClientRect(m_hwnd, &r);
        return D2D1::SizeF(static_cast<float>(r.right) / Scale(), static_cast<float>(r.bottom) / Scale());
    }

    D2D1_POINT_2F EditorWindow::DipFromLParam(LPARAM lParam) const
    {
        return D2D1::Point2F(static_cast<float>(GET_X_LPARAM(lParam)) / Scale(), static_cast<float>(GET_Y_LPARAM(lParam)) / Scale());
    }

    void EditorWindow::Invalidate() const
    {
        if (m_hwnd)
            InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    ID2D1SolidColorBrush* EditorWindow::Brush(const D2D1_COLOR_F& color)
    {
        for (const auto& b : m_brushes)
            if (Same(b.color, color))
                return b.brush.Get();
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if (!m_rt || FAILED(m_rt->CreateSolidColorBrush(color, &brush)))
            return nullptr;
        if (m_brushes.size() > 64)
            m_brushes.clear();
        m_brushes.push_back({ color, brush });
        return brush.Get();
    }

    IDWriteTextFormat* EditorWindow::TextFormat(float sizeDip, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align)
    {
        for (const auto& f : m_formats)
            if (!f.icon && f.size == sizeDip && f.weight == weight && f.align == align)
                return f.format.Get();
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        if (FAILED(m_dwrite->CreateTextFormat(m_textFamily.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              sizeDip, L"", &format)))
            return nullptr;
        format->SetTextAlignment(align);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // Long file names end in "..." instead of running off.
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
        m_dwrite->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
        format->SetTrimming(&trimming, ellipsis.Get());
        m_formats.push_back({ sizeDip, static_cast<int>(weight), static_cast<int>(align), false, format });
        return format.Get();
    }

    IDWriteTextFormat* EditorWindow::IconFormat(float sizeDip)
    {
        for (const auto& f : m_formats)
            if (f.icon && f.size == sizeDip)
                return f.format.Get();
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        if (FAILED(m_dwrite->CreateTextFormat(m_iconFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, sizeDip, L"", &format)))
            return nullptr;
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_formats.push_back({ sizeDip, 0, 0, true, format });
        return format.Get();
    }

    void EditorWindow::DrawText(std::wstring_view text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color)
    {
        if (format && m_rt && !text.empty())
            if (auto* brush = Brush(color))
                m_rt->DrawText(text.data(), static_cast<UINT32>(text.size()), format, rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void EditorWindow::DrawButtons(ID2D1RenderTarget* rt)
    {
        const auto& c = m_colors;
        auto* label = TextFormat(13.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
        auto* icon = IconFormat(14.f);
        for (size_t i = 0; i < m_buttons.size(); ++i)
        {
            const auto& b = m_buttons[i];
            const bool hot = b.enabled && static_cast<int>(i) == m_hot;
            const bool pressed = hot && static_cast<int>(i) == m_pressed;
            D2D1_COLOR_F fill = b.primary ? c.accent : c.surface;
            if (hot)
                fill = theme::Mix(fill, c.dark ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(0, 0, 0, 1), pressed ? 0.16f : 0.08f);
            const float alpha = b.enabled ? 1.f : 0.4f;
            const auto rr = D2D1::RoundedRect(b.rect, 4.f, 4.f);
            if (auto* brush = Brush(theme::WithAlpha(fill, alpha)))
                rt->FillRoundedRectangle(rr, brush);
            if (!b.primary)
                if (auto* brush = Brush(c.line))
                    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.rect.left + 0.5f, b.rect.top + 0.5f, b.rect.right - 0.5f, b.rect.bottom - 0.5f), 4.f, 4.f),
                                             brush, 1.f);
            const D2D1_COLOR_F ink = theme::WithAlpha(b.primary ? c.textOnAccent : c.text, alpha);
            // Icon and text centred together.
            float textWidth = 0;
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            if (label && SUCCEEDED(m_dwrite->CreateTextLayout(b.text.c_str(), static_cast<UINT32>(b.text.size()), label, 1000.f, 40.f, &layout)))
            {
                DWRITE_TEXT_METRICS m{};
                layout->GetMetrics(&m);
                textWidth = m.widthIncludingTrailingWhitespace;
            }
            const float iconWidth = b.icon ? 22.f : 0.f;
            const float total = iconWidth + textWidth;
            const float x = (b.rect.left + b.rect.right - total) / 2.f;
            if (b.icon)
            {
                const wchar_t glyph[2] = { b.icon, 0 };
                DrawText(glyph, icon, D2D1::RectF(x, b.rect.top, x + 16.f, b.rect.bottom), ink);
            }
            DrawText(b.text, label, D2D1::RectF(x + iconWidth, b.rect.top, x + total + 2.f, b.rect.bottom), ink);
        }
    }

    int EditorWindow::ButtonAt(D2D1_POINT_2F p) const
    {
        for (size_t i = 0; i < m_buttons.size(); ++i)
        {
            const auto& r = m_buttons[i].rect;
            if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom)
                return static_cast<int>(i);
        }
        return -1;
    }

    bool EditorWindow::EnsureTarget()
    {
        if (m_rt)
            return true;
        RECT r{};
        GetClientRect(m_hwnd, &r);
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                        static_cast<float>(m_dpi), static_cast<float>(m_dpi));
        const auto hwndProps = D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(static_cast<UINT32>(r.right), static_cast<UINT32>(r.bottom)));
        if (FAILED(m_d2d->CreateHwndRenderTarget(props, hwndProps, &m_rt)))
            return false;
        m_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        ++m_generation;
        return true;
    }

    void EditorWindow::Render()
    {
        if (!EnsureTarget())
            return;
        m_rt->BeginDraw();
        m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
        m_rt->Clear(m_colors.background);
        Paint(m_rt.Get(), m_rt->GetSize());
        if (m_rt->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET))
        {
            m_rt.Reset();
            m_brushes.clear();
            Invalidate();
        }
    }

    LRESULT CALLBACK EditorWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
        {
            auto* self = static_cast<EditorWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->m_hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<EditorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self)
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        if (msg == WM_NCDESTROY)
        {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->m_hwnd = nullptr;
            delete self;
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        return self->Handle(msg, wParam, lParam);
    }

    LRESULT EditorWindow::Handle(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(m_hwnd, &ps);
            Render();
            EndPaint(m_hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (m_rt)
                m_rt->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            Layout(ClientDip());
            Invalidate();
            break;
        case WM_DPICHANGED:
        {
            m_dpi = HIWORD(wParam);
            if (m_rt)
                m_rt->SetDpi(static_cast<float>(m_dpi), static_cast<float>(m_dpi));
            const auto* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(ClientDip());
            Invalidate();
            return 0;
        }
        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            RECT r{ 0, 0, MulDiv(m_minDip.cx, static_cast<int>(m_dpi), 96), MulDiv(m_minDip.cy, static_cast<int>(m_dpi), 96) };
            AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, m_dpi);
            info->ptMinTrackSize = { r.right - r.left, r.bottom - r.top };
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0)
            {
                LoadColors();
                m_brushes.clear();
                Invalidate();
            }
            break;
        case WM_MOUSEMOVE:
        {
            const int hot = ButtonAt(DipFromLParam(lParam));
            if (hot != m_hot)
            {
                m_hot = hot;
                Invalidate();
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, m_hwnd, 0 };
                TrackMouseEvent(&track);
            }
            if (m_pressed >= 0)
                return 0;
            break;
        }
        case WM_MOUSELEAVE:
            if (m_hot >= 0)
            {
                m_hot = -1;
                Invalidate();
            }
            break;
        case WM_LBUTTONDOWN:
        {
            const int at = ButtonAt(DipFromLParam(lParam));
            if (at >= 0)
            {
                if (m_buttons[static_cast<size_t>(at)].enabled)
                {
                    m_pressed = at;
                    SetCapture(m_hwnd);
                    Invalidate();
                }
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP:
            if (m_pressed >= 0)
            {
                const int pressed = m_pressed;
                m_pressed = -1;
                ReleaseCapture();
                Invalidate();
                if (ButtonAt(DipFromLParam(lParam)) == pressed && m_buttons[static_cast<size_t>(pressed)].enabled)
                    if (auto action = m_buttons[static_cast<size_t>(pressed)].action)
                        action();   // may close the window: nothing after this touches members
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (m_pressed >= 0)
            {
                m_pressed = -1;
                Invalidate();
            }
            break;
        case WM_CLOSE:
            OnClosing();
            DestroyWindow(m_hwnd);
            return 0;
        default:
            break;
        }
        LRESULT result = 0;
        if (OnMessage(msg, wParam, lParam, result))
            return result;
        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }
}
