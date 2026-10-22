#include "EditorFrame.h"
#include "EditorChrome.h"
#include "Brand.h"
#include "UiThread.h"
#include "resource.h"
#include "keyboard/WheelLabels.h"
#include "keyboard/WheelLayout.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <numbers>

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
            if (!f.icon && !f.wrap && f.size == sizeDip && f.weight == weight && f.align == align)
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
            const ControlState state{ b.enabled, hot, hot && static_cast<int>(i) == m_pressed, false };
            if (b.visible && b.flat)
                DrawIconButton(b.rect, b.icon, false, state);
            else if (b.visible)
                DrawButton(b.rect, b.text, b.icon, b.primary, state);
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

    // ---- The editors' frame -------------------------------------------------------------------------

    void ControlKit::DrawToolBar(const D2D1_RECT_F& r)
    {
        if (!m_target)
            return;
        if (auto* brush = Brush(m_colors.window))
            m_target->FillRectangle(r, brush);
        if (auto* brush = Brush(m_colors.line))
            m_target->DrawLine(D2D1::Point2F(r.left, r.bottom - 0.5f), D2D1::Point2F(r.right, r.bottom - 0.5f), brush, 1.f);
    }

    void ControlKit::DrawToolTab(const D2D1_RECT_F& r, wchar_t icon, std::wstring_view label, bool selected, ControlState s)
    {
        const auto& c = m_colors;
        const bool hot = s.enabled && s.hot;
        const float alpha = s.enabled ? 1.f : kDisabledAlpha;
        if (selected)
            FillRound(r, c.selected, kControlRadius);
        else if (hot)
            FillRound(r, s.pressed ? c.controlDown : c.controlHover, kControlRadius);
        // Icon 16 DIPs, 3 between it and the name, 3 between the name and the bar (the kit's tab).
        const float top = r.top + 6.f;
        if (icon)
        {
            DrawGlyph(icon, 16.f, D2D1::RectF(r.left, top, r.right, top + 16.f), theme::WithAlpha(selected ? c.accent : c.text, alpha));
        }
        DrawText(label, TextFormat(12.f, selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                 D2D1::RectF(r.left + 2, top + 19.f, r.right - 2, top + 35.f), theme::WithAlpha(selected ? c.text : c.textDim, alpha));
        if (selected)
        {
            const float cx = (r.left + r.right) / 2;
            FillRound(D2D1::RectF(cx - 8.f, top + 38.f, cx + 8.f, top + 41.f), theme::WithAlpha(c.accent, alpha), 1.5f);
        }
        if (s.focused)
            DrawFocusRing(r, kControlRadius);
    }

    void ControlKit::DrawToolBarDivider(const D2D1_RECT_F& r)
    {
        if (m_target && r.bottom > r.top)
            if (auto* brush = Brush(m_colors.line))
                m_target->FillRectangle(r, brush);
    }

    void ControlKit::DrawSidePanel(const D2D1_RECT_F& r, std::wstring_view title)
    {
        if (!m_target)
            return;
        if (auto* brush = Brush(m_colors.panel))
            m_target->FillRectangle(r, brush);
        if (auto* brush = Brush(m_colors.line))
            m_target->DrawLine(D2D1::Point2F(r.left + 0.5f, r.top), D2D1::Point2F(r.left + 0.5f, r.bottom), brush, 1.f);
        const float top = r.top + chrome::kPanelPadY;
        DrawText(title, TextFormat(chrome::kPanelTitle, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                 D2D1::RectF(r.left + chrome::kPanelPadX, top, r.right - chrome::kPanelPadX, top + chrome::kPanelTitleHeight), m_colors.text);
    }

    void ControlKit::DrawSectionLabel(const D2D1_RECT_F& r, std::wstring_view text)
    {
        DrawText(text, TextFormat(chrome::kSectionLabel, DWRITE_FONT_WEIGHT_SEMI_BOLD), r, m_colors.textDim);
    }

    void ControlKit::DrawFooterBar(const D2D1_RECT_F& r, std::wstring_view status, float textRight, float textLeft)
    {
        if (!m_target)
            return;
        if (auto* brush = Brush(m_colors.window))
            m_target->FillRectangle(r, brush);
        if (auto* brush = Brush(m_colors.line))
            m_target->DrawLine(D2D1::Point2F(r.left, r.top + 0.5f), D2D1::Point2F(r.right, r.top + 0.5f), brush, 1.f);
        const float left = textLeft < 0 ? r.left + chrome::kFooterPadX : textLeft;
        DrawText(status, TextFormat(12.5f), D2D1::RectF(left, r.top, std::max(left, textRight - 12.f), r.bottom), m_colors.textDim);
    }

    void ControlKit::DrawToolTile(const D2D1_RECT_F& r, wchar_t icon, std::wstring_view label, bool chosen, ControlState s)
    {
        const auto& c = m_colors;
        const bool hot = s.enabled && s.hot;
        const float alpha = s.enabled ? 1.f : kDisabledAlpha;
        if (chosen)
        {
            FillRound(r, theme::WithAlpha(c.accentSoft, alpha), kControlRadius);
            StrokeRound(r, theme::WithAlpha(c.accent, alpha), 1.f, kControlRadius);
        }
        else if (hot)
            FillRound(r, theme::WithAlpha(s.pressed ? c.controlDown : c.controlHover, alpha), kControlRadius);
        const float h = r.bottom - r.top, top = r.top + std::max(2.f, (h - 34.f) / 2);
        DrawGlyph(icon, 16.f, D2D1::RectF(r.left, top, r.right, top + 18.f), theme::WithAlpha(chosen ? c.accent : c.text, alpha));
        DrawText(label, TextFormat(10.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(r.left + 1, top + 19.f, r.right - 1, top + 34.f),
                 theme::WithAlpha(chosen ? c.text : c.textDim, alpha));
        if (s.focused)
            DrawFocusRing(r, kControlRadius);
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
        {
            StrokeRound(r, c.controlLine, 1.f, kControlRadius);
            // The kit's buttons sit on the surface: their bottom edge a shade stronger.
            if (auto* brush = m_target && r.right - r.left > 2 * kControlRadius ? Brush(theme::WithAlpha(c.controlLine, alpha)) : nullptr)
                m_target->DrawLine(D2D1::Point2F(r.left + kControlRadius, r.bottom - 1.5f), D2D1::Point2F(r.right - kControlRadius, r.bottom - 1.5f), brush, 1.f);
        }
        const D2D1_COLOR_F ink = theme::WithAlpha(primary ? c.onAccent : c.text, alpha);
        // Icon and text centred together; a label too long for the button ends in "...".
        auto* label = TextFormat(kControlText, primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
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
            if (hot)
                FillRound(r, theme::WithAlpha(c.controlHover, alpha), radius);
            StrokeRound(r, theme::WithAlpha(c.outline, alpha), 1.f, radius);
        }
        // A 12 DIP knob, 4 DIPs in from the end (the kit's switch), a little larger while pressed.
        const float knob = hot && s.pressed ? 7.f : 6.f;
        const float inset = 4.f + knob;
        const auto centre = D2D1::Point2F(on ? r.right - inset : r.left + inset, (r.top + r.bottom) / 2);
        if (auto* brush = m_target ? Brush(theme::WithAlpha(on ? c.onAccent : c.outline, alpha)) : nullptr)
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
        FillRound(track, theme::WithAlpha(c.chipLine, alpha), 2.f);
        FillRound(D2D1::RectF(std::min(start, x), track.top, std::max(start, x), track.bottom), theme::WithAlpha(c.accent, alpha), 2.f);
        // A 16 DIP knob on the surface with an 8 DIP accent dot (larger while hovered or dragged).
        const float radius = 8.f;
        const float dot = s.pressed ? 5.f : hot ? 4.5f : 4.f;
        const auto thumb = D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius);
        if (auto* brush = Brush(theme::WithAlpha(c.shadow, alpha * 0.5f)))
            m_target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y + 1), radius, radius), brush);
        if (auto* brush = Brush(theme::WithAlpha(c.thumb, alpha)))
            m_target->FillEllipse(thumb, brush);
        if (auto* brush = Brush(theme::WithAlpha(c.chipLine, alpha)))
            m_target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius - 0.5f, radius - 0.5f), brush, 1.f);
        if (auto* brush = Brush(theme::WithAlpha(c.accent, alpha)))
            m_target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), dot, dot), brush);
        if (s.focused)
            DrawFocusRing(D2D1::RectF(x - radius, y - radius, x + radius, y + radius), radius);
    }

    void ControlKit::DrawField(const D2D1_RECT_F& r, bool editing, bool invalid, ControlState s)
    {
        const auto& c = m_colors;
        const D2D1_COLOR_F fill = editing ? c.field : s.enabled && s.hot ? c.controlHover : c.control;
        FillRound(r, theme::WithAlpha(fill, s.enabled ? 1.f : kDisabledAlpha), kControlRadius);
        StrokeRound(r, invalid ? c.danger : c.controlLine, invalid ? 1.5f : 1.f, kControlRadius);
        if (editing)
            FillRound(D2D1::RectF(r.left + 2, r.bottom - 2, r.right - 2, r.bottom), invalid ? c.danger : c.accent, 1.f);
        else if (!invalid && r.right - r.left > 2 * kControlRadius)
            FillRound(D2D1::RectF(r.left + kControlRadius - 1, r.bottom - 1, r.right - kControlRadius + 1, r.bottom),
                      theme::WithAlpha(c.fieldLine, s.enabled ? 1.f : kDisabledAlpha), 0.5f);
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
            if (auto* brush = Brush(theme::WithAlpha(c.accent, alpha)))
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

    // ---- The windows' pieces ------------------------------------------------------------------------

    IDWriteTextFormat* ControlKit::WrapFormat(float sizeDip, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align)
    {
        for (const auto& f : m_formats)
            if (f.wrap && f.size == sizeDip && f.weight == weight && f.align == align)
                return f.format.Get();
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        if (!m_dwrite || FAILED(m_dwrite->CreateTextFormat(m_textFamily.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                           sizeDip, L"", &format)))
            return nullptr;
        format->SetTextAlignment(align);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        CachedFormat cached{ sizeDip, static_cast<int>(weight), static_cast<int>(align), false, format };
        cached.wrap = true;
        m_formats.push_back(cached);
        return format.Get();
    }

    void ControlKit::DrawGlyph(wchar_t glyph, float sizeDip, const D2D1_RECT_F& r, const D2D1_COLOR_F& color)
    {
        if (!glyph)
            return;
        const wchar_t text[2] = { glyph, 0 };
        DrawText(text, IconFormat(sizeDip), r, color);
    }

    void ControlKit::DrawParagraph(std::wstring_view text, float sizeDip, DWRITE_FONT_WEIGHT weight, const D2D1_RECT_F& r, const D2D1_COLOR_F& color,
                                   DWRITE_TEXT_ALIGNMENT align)
    {
        auto* format = WrapFormat(sizeDip, weight, align);
        if (format && m_target && !text.empty())
            if (auto* brush = Brush(color))
                m_target->DrawText(text.data(), static_cast<UINT32>(text.size()), format, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    float ControlKit::TextHeight(std::wstring_view text, float sizeDip, DWRITE_FONT_WEIGHT weight, float width)
    {
        auto* format = WrapFormat(sizeDip, weight, DWRITE_TEXT_ALIGNMENT_LEADING);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        if (!format || text.empty() ||
            FAILED(m_dwrite->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, std::max(1.f, width), 10000.f, &layout)) ||
            FAILED(layout->GetMetrics(&metrics)))
            return text.empty() ? 0.f : sizeDip * 1.4f;
        return std::ceil(metrics.height);
    }

    void ControlKit::DrawCard(const D2D1_RECT_F& r)
    {
        FillRound(r, m_colors.panel, kCardRadius);
        StrokeRound(r, m_colors.line, 1.f, kCardRadius);
    }

    void ControlKit::DrawNavItem(const D2D1_RECT_F& r, wchar_t icon, std::wstring_view text, bool selected, ControlState s)
    {
        const auto& c = m_colors;
        const bool hot = s.enabled && s.hot;
        if (selected || (hot && s.pressed))
            FillRound(r, c.selected, kControlRadius);
        else if (hot)
            FillRound(r, theme::WithAlpha(c.selected, 0.6f), kControlRadius);
        if (selected)
        {
            const float cy = (r.top + r.bottom) / 2;
            FillRound(D2D1::RectF(r.left, cy - 8, r.left + 3, cy + 8), c.accent, 1.5f);
        }
        DrawGlyph(icon, 16.f, D2D1::RectF(r.left + 12, r.top, r.left + 30, r.bottom), c.text);
        DrawText(text, TextFormat(kNavText, selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(r.left + 44, r.top, r.right - 8, r.bottom),
                 c.text);
        if (s.focused)
            DrawFocusRing(r, kControlRadius);
    }

    void ControlKit::DrawListItem(const D2D1_RECT_F& r, bool selected, bool hot, bool focused, bool active)
    {
        const auto& c = m_colors;
        const auto row = D2D1::RectF(r.left + 4, r.top + 1, r.right - 4, r.bottom - 1);
        if (selected)
            FillRound(row, active ? c.selected : theme::WithAlpha(c.selected, 0.7f), kControlRadius);
        else if (hot)
            FillRound(row, theme::WithAlpha(c.selected, 0.5f), kControlRadius);
        if (selected)
        {
            const float cy = (row.top + row.bottom) / 2;
            FillRound(D2D1::RectF(row.left, cy - 8, row.left + 3, cy + 8), active ? c.accent : c.textFaint, 1.5f);
        }
        if (focused)
            DrawFocusRing(row, kControlRadius);
    }

    void ControlKit::DrawPill(const D2D1_RECT_F& r, std::wstring_view text, const D2D1_COLOR_F& ink)
    {
        const float radius = (r.bottom - r.top) / 2;
        FillRound(r, theme::WithAlpha(ink, 0.14f), radius);
        DrawText(text, TextFormat(kSmallText, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), r, ink);
    }

    float ControlKit::PillWidth(std::wstring_view text)
    {
        return std::ceil(TextWidth(text, kSmallText, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 20.f;
    }

    void ControlKit::DrawToggle(const D2D1_RECT_F& r, bool on, ControlState s)
    {
        const float cy = std::round((r.top + r.bottom) / 2);
        const auto track = D2D1::RectF(r.right - 40, cy - 10, r.right, cy + 10);
        DrawSwitch(track, on, { s.enabled, s.hot, s.pressed, false });
        DrawText(on ? L"On" : L"Off", TextFormat(kNavText, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(r.left, r.top, track.left - 12, r.bottom),
                 theme::WithAlpha(m_colors.text, s.enabled ? 1.f : kDisabledAlpha));
        if (s.focused)
            DrawFocusRing(D2D1::RectF(r.left + 3, r.top + 3, r.right - 3, r.bottom - 3), kControlRadius);
    }

    float ControlKit::SegmentWidth(std::wstring_view text)
    {
        return std::ceil(TextWidth(text, kControlText, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 32.f;
    }

    void ControlKit::DrawSegment(const D2D1_RECT_F& r, std::wstring_view text, bool selected, Part part, ControlState s)
    {
        if (!m_target)
            return;
        const auto& c = m_colors;
        const float alpha = s.enabled ? 1.f : kDisabledAlpha;
        // The track runs on past the sides another option continues; the clip cuts it there.
        const bool open0 = part == Part::Middle || part == Part::Last, open1 = part == Part::First || part == Part::Middle;
        m_target->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        const auto track = D2D1::RectF(open0 ? r.left - 12 : r.left, r.top, open1 ? r.right + 12 : r.right, r.bottom);
        FillRound(track, theme::WithAlpha(c.window, alpha), 5.f);
        StrokeRound(track, c.controlLine, 1.f, 5.f);
        m_target->PopAxisAlignedClip();
        const auto pill = D2D1::RectF(r.left + (open0 ? 1.f : 3.f), r.top + 3, r.right - (open1 ? 1.f : 3.f), r.bottom - 3);
        const bool hot = s.enabled && s.hot;
        if (selected)
        {
            FillRound(pill, theme::WithAlpha(c.panel, alpha), kControlRadius);
            StrokeRound(pill, c.controlLine, 1.f, kControlRadius);
            FillInside(pill, kControlRadius, D2D1::RectF(pill.left, pill.bottom - 2, pill.right, pill.bottom), theme::WithAlpha(c.accent, alpha));
        }
        else if (hot)
            FillRound(pill, s.pressed ? c.controlDown : c.controlHover, kControlRadius);
        DrawText(text, TextFormat(kControlText, selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), pill,
                 theme::WithAlpha(selected || hot ? c.text : c.textDim, alpha));
        if (s.focused)
            DrawFocusRing(pill, kControlRadius);
    }

    void ControlKit::DrawRadio(const D2D1_RECT_F& r, std::wstring_view text, std::wstring_view detail, bool selected, ControlState s)
    {
        if (!m_target)
            return;
        const auto& c = m_colors;
        const bool hot = s.enabled && s.hot;
        const float alpha = s.enabled ? 1.f : kDisabledAlpha;
        const auto centre = D2D1::Point2F(r.left + 10, std::round((r.top + r.bottom) / 2));
        if (selected)
        {
            if (auto* brush = Brush(theme::WithAlpha(hot ? c.accentHover : c.accent, alpha)))
                m_target->FillEllipse(D2D1::Ellipse(centre, 9.f, 9.f), brush);
            if (auto* brush = Brush(theme::WithAlpha(c.onAccent, alpha)))
                m_target->FillEllipse(D2D1::Ellipse(centre, hot && s.pressed ? 3.f : 4.f, hot && s.pressed ? 3.f : 4.f), brush);
        }
        else
        {
            if (auto* brush = Brush(theme::WithAlpha(hot ? c.controlHover : c.control, alpha)))
                m_target->FillEllipse(D2D1::Ellipse(centre, 9.f, 9.f), brush);
            if (auto* brush = Brush(theme::WithAlpha(c.textDim, alpha)))
                m_target->DrawEllipse(D2D1::Ellipse(centre, 8.5f, 8.5f), brush, 1.f);
        }
        auto* format = TextFormat(kControlText);
        const float x = r.left + 30;
        const float width = text.empty() ? 0.f : std::ceil(theme::TextWidth(m_dwrite.Get(), text, format));
        DrawText(text, format, D2D1::RectF(x, r.top, std::min(r.right, x + width + 2), r.bottom), theme::WithAlpha(c.text, alpha));
        if (!detail.empty())
            DrawText(detail, TextFormat(kSmallText), D2D1::RectF(x + width + 12, r.top, r.right, r.bottom), theme::WithAlpha(c.textDim, alpha));
        if (s.focused)
            DrawFocusRing(D2D1::RectF(r.left + 1, centre.y - 9, r.left + 19, centre.y + 9), 9.f);
    }

    float ControlKit::TabWidth(std::wstring_view text)
    {
        return std::ceil(TextWidth(text, kNavText, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 6.f;
    }

    void ControlKit::DrawTab(const D2D1_RECT_F& r, std::wstring_view text, bool selected, ControlState s)
    {
        const auto& c = m_colors;
        const bool hot = s.enabled && s.hot;
        DrawText(text, TextFormat(kNavText, selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                 D2D1::RectF(r.left, r.top, r.right, r.bottom - 9), theme::WithAlpha(selected || hot ? c.text : c.textDim, s.enabled ? 1.f : kDisabledAlpha));
        const float cx = (r.left + r.right) / 2;
        if (selected)
            FillRound(D2D1::RectF(cx - 9, r.bottom - 6, cx + 9, r.bottom - 3), c.accent, 1.5f);
        else if (hot)
            FillRound(D2D1::RectF(cx - 9, r.bottom - 6, cx + 9, r.bottom - 3), c.controlLine, 1.5f);
        if (s.focused)
            DrawFocusRing(D2D1::RectF(r.left + 2, r.top + 4, r.right - 2, r.bottom - 2), kControlRadius);
    }

    float ControlKit::ChipWidth(std::wstring_view text)
    {
        return std::ceil(TextWidth(text, kControlText, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 42.f;
    }

    void ControlKit::DrawChip(const D2D1_RECT_F& r, std::wstring_view text, bool on, ControlState s)
    {
        const auto& c = m_colors;
        const bool hot = s.enabled && s.hot;
        const float alpha = s.enabled ? 1.f : kDisabledAlpha;
        const float radius = (r.bottom - r.top) / 2;
        if (on)
        {
            FillRound(r, theme::WithAlpha(hot ? theme::WithAlpha(c.accent, s.pressed ? 0.34f : 0.26f) : c.accentSoft, alpha), radius);
            StrokeRound(r, theme::WithAlpha(c.accent, alpha), 1.f, radius);
            DrawGlyph(0xE73E, 11.f, D2D1::RectF(r.left + 9, r.top, r.left + 23, r.bottom), theme::WithAlpha(c.text, alpha));   // CheckMark
        }
        else
        {
            FillRound(r, theme::WithAlpha(hot && s.pressed ? c.controlDown : hot ? c.controlHover : c.control, alpha), radius);
            StrokeRound(r, theme::WithAlpha(c.controlLine, alpha), 1.f, radius);
        }
        // The label keeps its place on and off, so chips don't shift as they are ticked.
        DrawText(text, TextFormat(kControlText, on ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(r.left + 27, r.top, r.right - 10, r.bottom),
                 theme::WithAlpha(on ? c.text : c.textDim, alpha));
        if (s.focused)
            DrawFocusRing(r, radius);
    }

    void ControlKit::DrawDropDown(const D2D1_RECT_F& r, std::wstring_view text, bool open, ControlState s)
    {
        const auto& c = m_colors;
        const bool hot = s.enabled && s.hot;
        const float alpha = s.enabled ? 1.f : kDisabledAlpha;
        FillRound(r, theme::WithAlpha(open || (hot && s.pressed) ? c.controlDown : hot ? c.controlHover : c.control, alpha), kControlRadius);
        StrokeRound(r, c.controlLine, 1.f, kControlRadius);
        DrawText(text, TextFormat(kControlText), D2D1::RectF(r.left + 12, r.top, r.right - 34, r.bottom), theme::WithAlpha(c.text, alpha));
        DrawGlyph(open ? 0xE70E : 0xE70D, 11.f, D2D1::RectF(r.right - 30, r.top, r.right - 10, r.bottom), theme::WithAlpha(c.textDim, alpha));   // ChevronUp / ChevronDown
        if (s.focused)
            DrawFocusRing(r, kControlRadius);
    }

    void ControlKit::DrawListRow(const D2D1_RECT_F& r, std::wstring_view text, bool current, bool hot)
    {
        const auto& c = m_colors;
        if (!m_target)
            return;
        if (auto* brush = Brush(c.field))
            m_target->FillRectangle(r, brush);
        const auto inner = D2D1::RectF(r.left + 4, r.top + 2, r.right - 4, r.bottom - 2);
        if (hot || current)
            FillRound(inner, hot ? c.controlHover : c.selected, kControlRadius);
        if (current)
        {
            const float cy = (r.top + r.bottom) / 2;
            FillRound(D2D1::RectF(inner.left, cy - 8, inner.left + 3, cy + 8), c.accent, 1.5f);
        }
        DrawText(text, TextFormat(kControlText), D2D1::RectF(inner.left + 12, r.top, inner.right - 8, r.bottom), c.text);
    }

    void ControlKit::FillInside(const D2D1_RECT_F& shape, float radius, const D2D1_RECT_F& r, const D2D1_COLOR_F& color)
    {
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> mask;
        if (!m_target)
            return;
        m_target->GetFactory(&factory);
        if (!factory || FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(shape, radius, radius), &mask)))
            return;
        // A layer without its own bitmap (Windows 8 and later), masked by the rounded shape.
        m_target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), mask.Get()), nullptr);
        if (auto* brush = Brush(color))
            m_target->FillRectangle(r, brush);
        m_target->PopLayer();
    }

    void ControlKit::DrawTextBox(const D2D1_RECT_F& r, bool focused, bool invalid, bool enabled)
    {
        const auto& c = m_colors;
        FillRound(r, theme::WithAlpha(c.field, enabled ? 1.f : kDisabledAlpha), kControlRadius);
        StrokeRound(r, invalid ? c.danger : c.controlLine, 1.f, kControlRadius);
        // The line under it follows the rounded corners; thicker while typing or when it is wrong.
        const float thick = focused || invalid ? 2.f : 1.f;
        const auto line = invalid ? c.danger : focused ? c.accent : theme::WithAlpha(c.textFaint, enabled ? 1.f : kDisabledAlpha);
        FillInside(r, kControlRadius, D2D1::RectF(r.left, r.bottom - thick, r.right, r.bottom), line);
    }

    void ControlKit::DrawExpander(const D2D1_RECT_F& r, wchar_t icon, std::wstring_view title, std::wstring_view detail, std::wstring_view trailing,
                                  bool open, ControlState s)
    {
        const auto& c = m_colors;
        const bool hot = s.enabled && s.hot;
        if (hot)
            FillRound(r, s.pressed ? c.controlDown : c.controlHover, kCardRadius);
        const float alpha = s.enabled ? 1.f : kDisabledAlpha;
        DrawGlyph(icon, 18.f, D2D1::RectF(r.left + 16, r.top, r.left + 40, r.bottom), theme::WithAlpha(c.text, alpha));
        const float x = r.left + (icon ? 54.f : 18.f), cy = (r.top + r.bottom) / 2;
        const float right = r.right - 46 - (trailing.empty() ? 0.f : std::ceil(TextWidth(trailing, kControlText)) + 16);
        if (detail.empty())
            DrawText(title, TextFormat(kNavText), D2D1::RectF(x, r.top, right, r.bottom), theme::WithAlpha(c.text, alpha));
        else
        {
            DrawText(title, TextFormat(kNavText), D2D1::RectF(x, cy - 19, right, cy), theme::WithAlpha(c.text, alpha));
            DrawText(detail, TextFormat(kSmallText), D2D1::RectF(x, cy + 1, right, cy + 18), theme::WithAlpha(c.textDim, alpha));
        }
        DrawText(trailing, TextFormat(kControlText, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(right, r.top, r.right - 46, r.bottom),
                 theme::WithAlpha(c.textDim, alpha));
        DrawGlyph(open ? 0xE70E : 0xE70D, 11.f, D2D1::RectF(r.right - 42, r.top, r.right - 18, r.bottom), theme::WithAlpha(c.text, alpha));
        if (s.focused)
            DrawFocusRing(D2D1::RectF(r.left + 3, r.top + 3, r.right - 3, r.bottom - 3), kCardRadius);
    }

    void ControlKit::DrawMiniWheel(D2D1_POINT_2F centre, float radius, const std::vector<MiniTile>& tiles, int hot, const theme::Palette& p,
                                   std::wstring_view centreText, std::wstring_view detail)
    {
        if (!m_target)
            return;
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        m_target->GetFactory(&factory);
        // The real wheel's layout: which ring and place each tile takes, the rings' radii, and "More"
        // when there are more tiles than two rings hold.
        const auto layout = wheel::Arrange(tiles.size());
        constexpr float kDiscMargin = 6.f;   // the plate around the outer ring, DIPs at full size
        const float scale = radius / (layout.OuterEdge() + kDiscMargin);
        constexpr float kDeg = 180.f / std::numbers::pi_v<float>;
        const auto polar = [&](float r, float degrees) {
            return D2D1::Point2F(centre.x + r * std::sin(degrees / kDeg), centre.y - r * std::cos(degrees / kDeg));
        };
        if (auto* brush = Brush(p.disc))
            m_target->FillEllipse(D2D1::Ellipse(centre, radius, radius), brush);
        if (auto* brush = Brush(p.discLine))
            m_target->DrawEllipse(D2D1::Ellipse(centre, radius - 0.5f, radius - 0.5f), brush, 1.f);
        const float gap = wheel::kTileGapDip * scale / 2.f;
        for (int i = 0; i < static_cast<int>(layout.slots.size()); ++i)
        {
            const auto band = layout.BandOf(i);
            const float in = band.inner * scale, out = band.outer * scale, mid = layout.MidDeg(i), span = layout.SpanDeg(i);
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (!factory || FAILED(factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink)))
                return;
            if (layout.slots[static_cast<size_t>(i)].count == 1)
            {
                // A ring with one tile is the whole ring.
                sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
                for (const float r : { out, in })
                {
                    sink->BeginFigure(polar(r, 0.f), D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddArc(D2D1::ArcSegment(polar(r, 180.f), D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->AddArc(D2D1::ArcSegment(polar(r, 360.f), D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                }
            }
            else
            {
                // Each side `gap` from the radial line, so the gap between tiles is as wide all along.
                const float a0 = mid - span / 2, a1 = mid + span / 2;
                const float dOut = std::asin(std::min(1.f, gap / out)) * kDeg, dIn = std::asin(std::min(1.f, gap / std::max(in, gap))) * kDeg;
                sink->BeginFigure(polar(out, a0 + dOut), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddArc(D2D1::ArcSegment(polar(out, a1 - dOut), D2D1::SizeF(out, out), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                              span - 2 * dOut > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                sink->AddLine(polar(in, a1 - dIn));
                sink->AddArc(D2D1::ArcSegment(polar(in, a0 + dIn), D2D1::SizeF(in, in), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                              span - 2 * dIn > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            }
            sink->Close();
            const int index = layout.ContentIndex(i);
            const bool lit = index >= 0 && index == hot;
            if (auto* brush = Brush(lit ? p.accent : p.tile))
                m_target->FillGeometry(path.Get(), brush);
            if (!lit)
                if (auto* brush = Brush(p.tileLine))
                    m_target->DrawGeometry(path.Get(), brush, 1.f);
            const bool more = layout.IsMore(i);
            if (!more && (index < 0 || static_cast<size_t>(index) >= tiles.size()))
                continue;
            const std::wstring_view label = more ? std::wstring_view(L"More") : std::wstring_view(tiles[static_cast<size_t>(index)].label);
            const wchar_t icon = more ? wchar_t(0xE712) : tiles[static_cast<size_t>(index)].icon;   // More: three dots
            const auto at = polar((in + out) / 2, mid);
            const float width = std::min(out - in, 2.f * (in + out) / 2 * std::sin(std::min(span, 180.f) / 2 / kDeg)) * 0.92f;
            const float size = std::max(6.f, std::round(13.f * scale * 2) / 2);
            const auto ink = lit ? p.textOnAccent : p.text;
            if (icon)
            {
                const float iconSize = std::max(7.f, std::round(18.f * scale));
                DrawGlyph(icon, iconSize, D2D1::RectF(at.x - iconSize, at.y - iconSize - 1, at.x + iconSize, at.y + 1), ink);
                DrawText(label, TextFormat(size, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
                         D2D1::RectF(at.x - width / 2, at.y + 1, at.x + width / 2, at.y + 2 + size * 1.4f), ink);
            }
            else
                DrawText(label, TextFormat(size, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
                         D2D1::RectF(at.x - width / 2, at.y - size, at.x + width / 2, at.y + size), ink);
        }
        const float hub = wheel::kCentreDip * scale;
        if (auto* brush = Brush(p.centre))
            m_target->FillEllipse(D2D1::Ellipse(centre, hub, hub), brush);
        if (auto* brush = Brush(p.centreLine))
            m_target->DrawEllipse(D2D1::Ellipse(centre, hub - 0.5f, hub - 0.5f), brush, 1.f);
        const float size = std::max(7.f, std::round(15.f * scale * 2) / 2), detailSize = std::max(6.f, std::round(11.f * scale * 2) / 2);
        DrawText(centreText, TextFormat(size, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
                 D2D1::RectF(centre.x - hub, centre.y - size * 1.2f, centre.x + hub, centre.y + (detail.empty() ? size * 1.2f : 0.f)), p.text);
        DrawText(detail, TextFormat(detailSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                 D2D1::RectF(centre.x - hub + 4, centre.y + 1, centre.x + hub - 4, centre.y + 1 + detailSize * 1.4f), p.subtleText);
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
