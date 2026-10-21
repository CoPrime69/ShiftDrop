#include "mediaeditor/EditorUi.h"
#include "Theme.h"

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace sd::mediatools::ui
{
    D2D1_RECT_F ToD2D(const RectF& r) { return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h); }

    // ---- Painter -------------------------------------------------------------------------------------

    bool Painter::Init(IDWriteFactory* dwrite, const FormatFn& format)
    {
        m_dwrite = dwrite;
        struct Spec
        {
            float size;
            DWRITE_FONT_WEIGHT weight;
            bool icon;
        };
        const Spec specs[9] = {
            { 15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false },   // Title
            { 13.f, DWRITE_FONT_WEIGHT_NORMAL, false },      // Body
            { 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false },   // BodyStrong
            { 12.f, DWRITE_FONT_WEIGHT_NORMAL, false },      // Small
            { 11.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false },   // Caption
            { 16.f, DWRITE_FONT_WEIGHT_NORMAL, true },       // Icon
            { 12.f, DWRITE_FONT_WEIGHT_NORMAL, true },       // IconSmall
            { 13.f, DWRITE_FONT_WEIGHT_NORMAL, false },      // Number
            { 15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false },   // NumberLarge
        };
        for (int i = 0; i < 9; ++i)
            if (!(m_formats[i] = format(specs[i].size, specs[i].weight, specs[i].icon)))
                return false;
        // Timecodes use tabular figures so they do not shuffle while playing.
        if (SUCCEEDED(dwrite->CreateTypography(&m_tabular)))
            m_tabular->AddFontFeature({ DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1 });
        return true;
    }

    void Painter::Begin(ID2D1RenderTarget* rt, const theme::EditorPalette& colors, unsigned device)
    {
        if (rt != m_rt || device != m_device)
            m_brush.Reset();
        m_rt = rt;
        m_device = device;
        m_c = colors;
    }

    ID2D1SolidColorBrush* Painter::Brush(const D2D1_COLOR_F& color)
    {
        if (!m_brush)
            m_rt->CreateSolidColorBrush(color, &m_brush);
        else
            m_brush->SetColor(color);
        return m_brush.Get();
    }

    void Painter::Fill(const RectF& r, const D2D1_COLOR_F& color, float radius)
    {
        if (r.w <= 0 || r.h <= 0)
            return;
        if (radius > 0)
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(r), radius, radius), Brush(color));
        else
            m_rt->FillRectangle(ToD2D(r), Brush(color));
    }

    void Painter::Stroke(const RectF& r, const D2D1_COLOR_F& color, float width, float radius)
    {
        const RectF in = r.Inset(width / 2);
        if (in.w <= 0 || in.h <= 0)
            return;
        if (radius > 0)
            m_rt->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(in), radius, radius), Brush(color), width);
        else
            m_rt->DrawRectangle(ToD2D(in), Brush(color), width);
    }

    void Painter::Line(float x0, float y0, float x1, float y1, const D2D1_COLOR_F& color, float width)
    {
        m_rt->DrawLine({ x0, y0 }, { x1, y1 }, Brush(color), width);
    }

    ComPtr<IDWriteTextLayout> Painter::Layout(const std::wstring& text, Font font, float w, float h)
    {
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(m_dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), Format(font), std::max(w, 1.f), std::max(h, 1.f), &layout)))
            return nullptr;
        if ((font == Font::Number || font == Font::NumberLarge) && m_tabular)
            layout->SetTypography(m_tabular.Get(), { 0, static_cast<UINT32>(text.size()) });
        return layout;
    }

    void Painter::Text(const std::wstring& text, const RectF& r, Font font, const D2D1_COLOR_F& color, int align)
    {
        if (text.empty() || r.w <= 0)
            return;
        const auto layout = Layout(text, font, r.w, r.h);
        if (!layout)
            return;
        layout->SetTextAlignment(align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        m_rt->DrawTextLayout({ r.x, r.y }, layout.Get(), Brush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    float Painter::Paragraph(const std::wstring& text, const RectF& r, Font font, const D2D1_COLOR_F& color)
    {
        if (text.empty() || r.w <= 0)
            return 0;
        const auto layout = Layout(text, font, r.w, 10000.f);
        if (!layout)
            return 0;
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        m_rt->DrawTextLayout({ r.x, r.y }, layout.Get(), Brush(color));
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return m.height;
    }

    float Painter::ParagraphHeight(const std::wstring& text, float width, Font font)
    {
        const auto layout = text.empty() ? nullptr : Layout(text, font, width, 10000.f);
        if (!layout)
            return 0;
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return m.height;
    }

    float Painter::TextWidth(const std::wstring& text, Font font)
    {
        const bool number = font == Font::Number || font == Font::NumberLarge;
        return theme::TextWidth(m_dwrite, text, Format(font), number ? m_tabular.Get() : nullptr);
    }

    float Painter::CaretX(const std::wstring& text, size_t chars, Font font)
    {
        const auto layout = text.empty() ? nullptr : Layout(text, font, 10000.f, 100.f);
        if (!layout)
            return 0;
        FLOAT x = 0, y = 0;
        DWRITE_HIT_TEST_METRICS hit{};
        layout->HitTestTextPosition(static_cast<UINT32>(std::min(chars, text.size())), FALSE, &x, &y, &hit);
        return x;
    }

    size_t Painter::CharAt(const std::wstring& text, float x, Font font)
    {
        const auto layout = text.empty() ? nullptr : Layout(text, font, 10000.f, 100.f);
        if (!layout)
            return 0;
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS hit{};
        layout->HitTestPoint(x, 10.f, &trailing, &inside, &hit);
        return std::min(text.size(), static_cast<size_t>(hit.textPosition) + (trailing ? 1 : 0));
    }
}
