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
