#include "PanelWindow.h"
#include "Theme.h"

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace sd
{
    HWND PanelWindow::CreatePanel(HINSTANCE instance, const wchar_t* className, WNDPROC proc, DWORD exStyle, const wchar_t* title,
                                  void* self, LPCWSTR cursor, UINT classStyle)
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = classStyle;
        wc.lpfnWndProc = proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, cursor);
        wc.lpszClassName = className;
        RegisterClassExW(&wc);   // fails harmlessly when the class is already registered
        return CreateWindowExW(WS_EX_LAYERED | exStyle, className, title, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, self);
    }

    bool PanelWindow::InitGraphics()
    {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.ReleaseAndGetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(m_dwrite.ReleaseAndGetAddressOf()))))
            return false;
        m_canvas = std::make_unique<Canvas>(m_d2d.Get());
        m_textFamily = theme::TextFamily(m_dwrite.Get());
        m_iconFamily = theme::IconFamily(m_dwrite.Get());
        return true;
    }

    ComPtr<IDWriteTextFormat> PanelWindow::MakeFormat(const std::wstring& family, float dip, DWRITE_FONT_WEIGHT weight,
                                                      DWRITE_TEXT_ALIGNMENT align) const
    {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(m_dwrite->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              dip * m_scale, L"", &format)))
            return nullptr;
        format->SetTextAlignment(align);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // Text that does not fit ends in "…" rather than being cut mid-letter.
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(m_dwrite->CreateEllipsisTrimmingSign(format.Get(), &ellipsis)))
            format->SetTrimming(&trimming, ellipsis.Get());
        return format;
    }

    float PanelWindow::TextWidth(const std::wstring& text, IDWriteTextFormat* format) const
    {
        return std::ceil(theme::TextWidth(m_dwrite.Get(), text, format));
    }

    ComPtr<ID2D1SolidColorBrush> PanelWindow::Brush(const D2D1_COLOR_F& color, float alpha)
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        m_rt->CreateSolidColorBrush(theme::WithAlpha(color, alpha), &brush);
        return brush;
    }

    void PanelWindow::DrawFileMark(const D2D1_RECT_F& box, const theme::Palette& palette, bool done, float alpha)
    {
        // The kit's 104 x 128 drawing, scaled to fit.
        constexpr float kW = 104.f, kH = 128.f;
        const float k = std::min((box.right - box.left) / kW, (box.bottom - box.top) / kH);
        const float x0 = (box.left + box.right - kW * k) / 2.f, y0 = (box.top + box.bottom - kH * k) / 2.f;
        const auto at = [&](float x, float y) { return D2D1::Point2F(x0 + x * k, y0 + y * k); };
        const auto arc = [&](float x, float y, float r, D2D1_SWEEP_DIRECTION sweep) {
            return D2D1::ArcSegment(at(x, y), D2D1::SizeF(r * k, r * k), 0.f, sweep, D2D1_ARC_SIZE_SMALL);
        };
        constexpr auto cw = D2D1_SWEEP_DIRECTION_CLOCKWISE, ccw = D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
        ComPtr<ID2D1PathGeometry> page;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(m_d2d->CreatePathGeometry(&page)) && SUCCEEDED(page->Open(&sink)))
        {
            sink->BeginFigure(at(15, 13), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(at(63, 13));
            sink->AddArc(arc(89, 39, 26, ccw));
            sink->AddLine(at(89, 113));
            sink->AddArc(arc(79, 123, 10, cw));
            sink->AddLine(at(15, 123));
            sink->AddArc(arc(5, 113, 10, cw));
            sink->AddLine(at(5, 23));
            sink->AddArc(arc(15, 13, 10, cw));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            m_rt->FillGeometry(page.Get(), Brush(palette.page, alpha).Get());
            m_rt->DrawGeometry(page.Get(), Brush(palette.pageLine, alpha).Get(), std::max(1.f, 1.5f * k));
        }
        ComPtr<ID2D1PathGeometry> corner;
        sink.Reset();
        if (done && SUCCEEDED(m_d2d->CreatePathGeometry(&corner)) && SUCCEEDED(corner->Open(&sink)))
        {
            sink->BeginFigure(at(67.5f, 8.5f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(at(90.5f, 8.5f));
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(93.5f, 8.5f), at(93.5f, 11.5f)));
            sink->AddLine(at(93.5f, 34.5f));
            sink->AddArc(arc(67.5f, 8.5f, 26, cw));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            m_rt->FillGeometry(corner.Get(), Brush(palette.pageMark, alpha).Get());
        }
    }

    void PanelWindow::Text(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float alpha)
    {
        if (text.empty() || !format || alpha <= 0.f)
            return;
        m_rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, Brush(color, alpha).Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void FrameClock::Start(HWND hwnd, UINT_PTR timerId)
    {
        if (m_running)
            return;
        m_running = true;
        m_last = GetTickCount64();
        SetTimer(hwnd, timerId, 10, nullptr);
    }

    void FrameClock::Stop(HWND hwnd, UINT_PTR timerId)
    {
        KillTimer(hwnd, timerId);
        m_running = false;
    }

    float FrameClock::Tick(ULONGLONG now)
    {
        const float dt = static_cast<float>(std::min<ULONGLONG>(now - m_last, 50));
        m_last = now;
        return dt;
    }
}
