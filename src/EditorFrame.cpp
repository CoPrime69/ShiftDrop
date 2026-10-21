#include "EditorFrame.h"
#include "Brand.h"
#include "UiThread.h"
#include "resource.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace sd
{
    namespace
    {
        constexpr wchar_t kClass[] = L"" SD_NAME L".Editor";

        bool Same(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
    }

    ControlKit::ControlKit() : m_colors(theme::EditorColors())
    {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwrite.GetAddressOf()));
        if (m_dwrite)
        {
            m_textFamily = theme::TextFamily(m_dwrite.Get());
            m_iconFamily = theme::IconFamily(m_dwrite.Get());
        }
    }

    void ControlKit::SetTarget(ID2D1RenderTarget* target)
    {
        if (target != m_target)
            m_brushes.clear();
        m_target = target;
    }

    EditorFrame::EditorFrame(bool opaque) : m_opaque(opaque)
    {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.GetAddressOf());
    }

    EditorFrame::~EditorFrame() = default;

    void EditorFrame::LoadColors()
    {
        SetPalette(theme::EditorColors());
        if (m_hwnd)
            theme::ApplyDarkCaption(m_hwnd, Theme().dark, theme::ToColorRef(Theme().window));
    }

    EditorFrame::Monitor EditorFrame::CursorMonitor()
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ sizeof(info) };
        GetMonitorInfoW(monitor, &info);
        UINT dpiX = 96, dpiY = 96;
        GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        return { info.rcWork, dpiX };
    }

    bool EditorFrame::Create(const std::wstring& title, int widthDip, int heightDip, int minWidthDip, int minHeightDip, bool show, HWND owner)
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
        if (!registered || !m_d2d || !DWrite())
            return false;
        m_minDip = { minWidthDip, minHeightDip };

        // Open on the monitor the user is working on (where the drop happened), centred.
        auto monitor = CursorMonitor();
        RECT around = monitor.work;
        if (owner)
        {
            // Over its owner, on the owner's monitor.
            MONITORINFO info{ sizeof(info) };
            GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &info);
            monitor = { info.rcWork, GetDpiForWindow(owner) };
            GetWindowRect(owner, &around);
        }
        const RECT work = monitor.work;
        const int dpi = static_cast<int>(monitor.dpi);
        const DWORD exStyle = owner ? 0 : WS_EX_APPWINDOW;
        RECT r{ 0, 0, MulDiv(widthDip, dpi, 96), MulDiv(heightDip, dpi, 96) };
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, exStyle, monitor.dpi);
        const int w = std::min<int>(r.right - r.left, work.right - work.left);
        const int h = std::min<int>(r.bottom - r.top, work.bottom - work.top);
        const int x = std::clamp<int>(around.left + (around.right - around.left - w) / 2, work.left, std::max<int>(work.left, work.right - w));
        const int y = std::clamp<int>(around.top + (around.bottom - around.top - h) / 2, work.top, std::max<int>(work.top, work.bottom - h));
        // WM_NCCREATE stores the handle in m_hwnd (messages arrive before CreateWindowEx returns).
        if (!CreateWindowExW(exStyle, kClass, title.c_str(), WS_OVERLAPPEDWINDOW, x, y, w, h, owner, nullptr, GetModuleHandleW(nullptr), this) || !m_hwnd)
            return false;
        m_dpi = GetDpiForWindow(m_hwnd);
        LoadColors();   // the title bar needs the window
        if (show)
            Present();
        return true;
    }

    void EditorFrame::Present()
    {
        ShowWindow(m_hwnd, SW_SHOWNORMAL);
        BringToFront(m_hwnd);   // we are answering the user's drop
    }

    void EditorFrame::Close()
    {
        if (m_hwnd)
        {
            OnClosing();
            DestroyWindow(m_hwnd);   // WM_NCDESTROY deletes this
        }
    }

    D2D1_SIZE_F EditorFrame::ClientDip() const
    {
        RECT r{};
        GetClientRect(m_hwnd, &r);
        return D2D1::SizeF(static_cast<float>(r.right) / Scale(), static_cast<float>(r.bottom) / Scale());
    }

    D2D1_POINT_2F EditorFrame::DipFromLParam(LPARAM lParam) const
    {
        return D2D1::Point2F(static_cast<float>(GET_X_LPARAM(lParam)) / Scale(), static_cast<float>(GET_Y_LPARAM(lParam)) / Scale());
    }

    void EditorFrame::Invalidate() const
    {
        if (m_hwnd)
            InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    ID2D1SolidColorBrush* ControlKit::Brush(const D2D1_COLOR_F& color)
    {
        for (const auto& b : m_brushes)
            if (Same(b.color, color))
                return b.brush.Get();
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if (!m_target || FAILED(m_target->CreateSolidColorBrush(color, &brush)))
            return nullptr;
        if (m_brushes.size() > 64)
            m_brushes.clear();
        m_brushes.push_back({ color, brush });
        return brush.Get();
    }

    IDWriteTextFormat* ControlKit::TextFormat(float sizeDip, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align)
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

    IDWriteTextFormat* ControlKit::IconFormat(float sizeDip)
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

    void ControlKit::DrawText(std::wstring_view text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color)
    {
        if (format && m_target && !text.empty())
            if (auto* brush = Brush(color))
                m_target->DrawText(text.data(), static_cast<UINT32>(text.size()), format, rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    float ControlKit::TextWidth(std::wstring_view text, float sizeDip, DWRITE_FONT_WEIGHT weight)
    {
        return theme::TextWidth(m_dwrite.Get(), text, TextFormat(sizeDip, weight));
    }

    void EditorFrame::DrawButtons()
    {
        for (size_t i = 0; i < m_buttons.size(); ++i)
        {
            const auto& b = m_buttons[i];
            const bool hot = static_cast<int>(i) == m_hot;
            if (b.visible)
                DrawButton(b.rect, b.text, b.icon, b.primary, { b.enabled, hot, hot && static_cast<int>(i) == m_pressed, false });
        }
    }

    // ---- Controls ------------------------------------------------------------------------------------

    void ControlKit::FillRound(const D2D1_RECT_F& r, const D2D1_COLOR_F& color, float radius)
    {
        if (r.right <= r.left || r.bottom <= r.top || !m_target)
            return;
        if (auto* brush = Brush(color))
            m_target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush);
    }

    void ControlKit::StrokeRound(const D2D1_RECT_F& r, const D2D1_COLOR_F& color, float width, float radius)
    {
        const float in = width / 2;
        if (r.right - r.left <= width || r.bottom - r.top <= width || !m_target)
            return;
        if (auto* brush = Brush(color))
            m_target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + in, r.top + in, r.right - in, r.bottom - in), radius, radius), brush, width);
    }

    void ControlKit::DrawFocusRing(const D2D1_RECT_F& r, float radius)
    {
        StrokeRound(D2D1::RectF(r.left - 3, r.top - 3, r.right + 3, r.bottom + 3), m_colors.focus, 2.f, radius + 3);
    }

    void ControlKit::DrawButton(const D2D1_RECT_F& r, std::wstring_view text, wchar_t icon, bool primary, ControlState s)
    {
        const auto& c = m_colors;
        const bool hot = s.enabled && s.hot, pressed = hot && s.pressed;
        const D2D1_COLOR_F fill = primary ? (pressed ? theme::WithAlpha(c.accent, 0.85f) : hot ? c.accentHover : c.accent)
                                          : (pressed ? c.controlDown : hot ? c.controlHover : c.control);
        const float alpha = s.enabled ? 1.f : kDisabledAlpha;
        FillRound(r, theme::WithAlpha(fill, alpha), kControlRadius);
        if (!primary)
            StrokeRound(r, c.controlLine, 1.f, kControlRadius);
        const D2D1_COLOR_F ink = theme::WithAlpha(primary ? c.onAccent : c.text, alpha);
        // Icon and text centred together; a label too long for the button ends in "...".
        auto* label = TextFormat(kControlText);
        const float iconWidth = icon ? (text.empty() ? 16.f : 22.f) : 0.f;
        const float total = iconWidth + (text.empty() ? 0.f : theme::TextWidth(m_dwrite.Get(), text, label));
        const float x = std::max(r.left + 8.f, (r.left + r.right - total) / 2.f);
        if (icon)
        {
            const wchar_t glyph[2] = { icon, 0 };
            DrawText(glyph, IconFormat(14.f), D2D1::RectF(x, r.top, x + 16.f, r.bottom), ink);
        }
        DrawText(text, label, D2D1::RectF(x + iconWidth, r.top, std::min(x + total + 2.f, r.right - 6.f), r.bottom), ink);
        if (s.focused)
            DrawFocusRing(r, kControlRadius);
    }

    void ControlKit::DrawIconButton(const D2D1_RECT_F& r, wchar_t icon, bool on, ControlState s)
    {
        const auto& c = m_colors;
        if (on)
            FillRound(r, c.accentSoft, kControlRadius);
        else if (s.enabled && s.hot && s.pressed)
            FillRound(r, c.controlDown, kControlRadius);
        else if (s.enabled && s.hot)
            FillRound(r, c.controlHover, kControlRadius);
        const wchar_t glyph[2] = { icon, 0 };
        DrawText(glyph, IconFormat(r.bottom - r.top < 26 ? 12.f : 16.f), r, theme::WithAlpha(on ? c.accent : c.text, s.enabled ? 1.f : kDisabledAlpha));
        if (s.focused)
            DrawFocusRing(r, kControlRadius);
    }

    void ControlKit::DrawChoice(const D2D1_RECT_F& r, std::wstring_view text, bool selected, ControlState s)
    {
        const auto& c = m_colors;
        const bool hot = s.enabled && s.hot;
        const float alpha = s.enabled ? 1.f : kDisabledAlpha;
        if (selected)
            FillRound(r, theme::WithAlpha(c.accent, alpha), kControlRadius);
        else
        {
            FillRound(r, theme::WithAlpha(hot && s.pressed ? c.controlDown : hot ? c.controlHover : c.control, alpha), kControlRadius);
            StrokeRound(r, c.controlLine, 1.f, kControlRadius);
        }
        DrawText(text, TextFormat(kControlText, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(r.left + 2, r.top, r.right - 2, r.bottom),
                 theme::WithAlpha(selected ? c.onAccent : c.text, alpha));
        if (s.focused)
            DrawFocusRing(r, kControlRadius);
    }

    void ControlKit::DrawSwitch(const D2D1_RECT_F& r, bool on, ControlState s)
    {
        const auto& c = m_colors;
        const bool hot = s.enabled && s.hot;
        const float alpha = s.enabled ? 1.f : kDisabledAlpha;
        const float radius = (r.bottom - r.top) / 2;
        if (on)
            FillRound(r, theme::WithAlpha(hot ? c.accentHover : c.accent, alpha), radius);
        else
        {
            FillRound(r, theme::WithAlpha(hot ? c.controlHover : c.control, alpha), radius);
            StrokeRound(r, theme::WithAlpha(c.textDim, alpha), 1.f, radius);
        }
        const float knob = on ? 7.f : 6.f;
        const auto centre = D2D1::Point2F(on ? r.right - radius : r.left + radius, (r.top + r.bottom) / 2);
        if (auto* brush = m_target ? Brush(theme::WithAlpha(on ? c.onAccent : c.textDim, alpha)) : nullptr)
            m_target->FillEllipse(D2D1::Ellipse(centre, knob, knob), brush);
        if (s.focused)
            DrawFocusRing(r, radius);
    }

    void ControlKit::DrawSlider(const D2D1_RECT_F& track, float value, float from, ControlState s)
    {
        if (!m_target)
            return;
        const auto& c = m_colors;
        const bool hot = s.enabled && s.hot;
        const float alpha = s.enabled ? 1.f : kDisabledAlpha;
        const float width = track.right - track.left;
        const float x = track.left + std::clamp(value, 0.f, 1.f) * width;
        const float start = track.left + std::clamp(from, 0.f, 1.f) * width;
        const float y = (track.top + track.bottom) / 2;
        FillRound(track, theme::WithAlpha(c.dark ? c.controlHover : c.controlDown, alpha), 2.f);
        FillRound(D2D1::RectF(std::min(start, x), track.top, std::max(start, x), track.bottom), theme::WithAlpha(c.accent, alpha), 2.f);
        const float radius = s.pressed ? 8.f : hot ? 7.5f : 7.f;
        const auto thumb = D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius);
        if (auto* brush = Brush(theme::WithAlpha(c.dark ? D2D1::ColorF(0x45454C) : D2D1::ColorF(1, 1, 1, 1), alpha)))
            m_target->FillEllipse(thumb, brush);
        if (auto* brush = Brush(theme::WithAlpha(c.controlLine, alpha)))
            m_target->DrawEllipse(thumb, brush, 1.f);
        if (auto* brush = Brush(theme::WithAlpha(c.accent, alpha)))
            m_target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius - 3.5f, radius - 3.5f), brush);
        if (s.focused)
            DrawFocusRing(D2D1::RectF(x - radius, y - radius, x + radius, y + radius), radius);
    }

    void ControlKit::DrawField(const D2D1_RECT_F& r, bool editing, bool invalid, ControlState s)
    {
        const auto& c = m_colors;
        const D2D1_COLOR_F fill = editing ? (c.dark ? c.stage : D2D1::ColorF(1, 1, 1, 1)) : s.enabled && s.hot ? c.controlHover : c.control;
        FillRound(r, theme::WithAlpha(fill, s.enabled ? 1.f : kDisabledAlpha), kControlRadius);
        StrokeRound(r, invalid ? c.danger : c.controlLine, invalid ? 1.5f : 1.f, kControlRadius);
        if (editing)
            FillRound(D2D1::RectF(r.left + 2, r.bottom - 2, r.right - 2, r.bottom), invalid ? c.danger : c.accent, 1.f);
        if (s.focused && !editing)
            DrawFocusRing(r, kControlRadius);
    }

    void ControlKit::DrawSwatch(const D2D1_RECT_F& r, const D2D1_COLOR_F& colour, wchar_t glyph, const D2D1_COLOR_F& ink, bool selected, ControlState s)
    {
        if (!m_target)
            return;
        const auto& c = m_colors;
        const float alpha = s.enabled ? 1.f : kDisabledAlpha;
        const auto centre = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        const float outer = std::max(4.f, std::min(r.right - r.left, r.bottom - r.top) / 2 - 4.f);   // the focus ring goes around it
        // Selected: a ring in the text colour, a gap, then the colour; hot or pressed: the colour grows
        // a little, like the slider's thumb.
        const bool hot = s.enabled && s.hot;
        const float dot = selected ? outer - 4.5f : hot && s.pressed ? outer : hot ? outer - 0.5f : outer - 1.5f;
        if (selected)
            if (auto* brush = Brush(theme::WithAlpha(c.text, alpha)))
                m_target->DrawEllipse(D2D1::Ellipse(centre, outer - 1.f, outer - 1.f), brush, 2.f);
        if (auto* brush = Brush(theme::WithAlpha(colour, alpha)))
            m_target->FillEllipse(D2D1::Ellipse(centre, dot, dot), brush);
        // A faint edge, so a colour close to the window's still shows its shape.
        if (auto* brush = Brush(theme::WithAlpha(c.controlLine, alpha)))
            m_target->DrawEllipse(D2D1::Ellipse(centre, dot - 0.5f, dot - 0.5f), brush, 1.f);
        if (glyph)
        {
            const wchar_t text[2] = { glyph, 0 };
            DrawText(text, IconFormat(std::max(8.f, std::round(dot * 0.9f))), D2D1::RectF(centre.x - dot, centre.y - dot, centre.x + dot, centre.y + dot),
                     theme::WithAlpha(ink, alpha));
        }
        if (s.focused)
            DrawFocusRing(D2D1::RectF(centre.x - outer, centre.y - outer, centre.x + outer, centre.y + outer), outer);
    }

    int EditorFrame::ButtonAt(D2D1_POINT_2F p) const
    {
        for (size_t i = 0; i < m_buttons.size(); ++i)
        {
            const auto& r = m_buttons[i].rect;
            if (m_buttons[i].visible && p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom)
                return static_cast<int>(i);
        }
        return -1;
    }

    bool EditorFrame::EnsureTarget()
    {
        if (m_rt)
            return true;
        RECT r{};
        GetClientRect(m_hwnd, &r);
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, m_opaque ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                        static_cast<float>(m_dpi), static_cast<float>(m_dpi));
        const auto hwndProps = D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(static_cast<UINT32>(r.right), static_cast<UINT32>(r.bottom)));
        if (FAILED(m_d2d->CreateHwndRenderTarget(props, hwndProps, &m_rt)))
            return false;
        SetTarget(m_rt.Get());
        if (!m_opaque)
            m_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // no ClearType on a transparent target
        ++m_generation;
        return true;
    }

    void EditorFrame::Render()
    {
        if (!EnsureTarget())
            return;
        m_rt->BeginDraw();
        m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
        m_rt->Clear(Theme().window);
        Paint(m_rt.Get(), m_rt->GetSize());
        if (m_rt->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET))
        {
            SetTarget(nullptr);
            m_rt.Reset();
            Invalidate();
        }
    }

    LRESULT CALLBACK EditorFrame::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
        {
            auto* self = static_cast<EditorFrame*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->m_hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<EditorFrame*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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

    LRESULT EditorFrame::Handle(UINT msg, WPARAM wParam, LPARAM lParam)
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
            OnDpiChanged();
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
        case WM_SETTINGCHANGE:   // light or dark ("ImmersiveColorSet")
        case WM_THEMECHANGED:
        case WM_DWMCOLORIZATIONCOLORCHANGED:   // the accent colour
            LoadColors();
            Invalidate();
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
            RequestClose();   // may delete this
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
