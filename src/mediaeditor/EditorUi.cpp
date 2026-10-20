#include "mediaeditor/EditorUi.h"
#include "Theme.h"

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace sd::mediatools::ui
{
    namespace
    {
        D2D1_COLOR_F Rgb(UINT rgb, float a = 1.f) { return D2D1::ColorF(rgb, a); }

        float Luminance(const D2D1_COLOR_F& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

        D2D1_COLOR_F Lighter(D2D1_COLOR_F c, float t) { return theme::Mix(c, D2D1::ColorF(1, 1, 1, 1), t); }
        D2D1_COLOR_F Darker(D2D1_COLOR_F c, float t) { return theme::Mix(c, D2D1::ColorF(0, 0, 0, 1), t); }
    }

    D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a)
    {
        c.a *= a;
        return c;
    }

    D2D1_RECT_F ToD2D(const RectF& r) { return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h); }

    Colors CurrentColors()
    {
        const auto t = theme::Current(Style::Solid);
        Colors c{};
        c.dark = t.dark;
        if (c.dark)
        {
            c.window = Rgb(0x202023);
            c.panel = Rgb(0x28282C);
            c.stage = Rgb(0x141416);
            c.track = Rgb(0x18181B);
            c.line = Rgb(0xFFFFFF, 0.08f);
            c.text = Rgb(0xF3F3F5);
            c.textDim = Rgb(0xF3F3F5, 0.66f);
            c.textFaint = Rgb(0xF3F3F5, 0.38f);
            c.control = Rgb(0x36363C);
            c.controlHover = Rgb(0x404047);
            c.controlDown = Rgb(0x2E2E33);
            c.controlLine = Rgb(0xFFFFFF, 0.07f);
            // Windows draws accent fills lighter in dark mode, with dark text when they get bright.
            c.accent = Lighter(t.accent, 0.22f);
            c.accentHover = Lighter(t.accent, 0.34f);
            c.danger = Rgb(0xFF7A7A);
            c.shadow = Rgb(0x000000, 0.5f);
        }
        else
        {
            c.window = Rgb(0xF3F3F5);
            c.panel = Rgb(0xFBFBFC);
            c.stage = Rgb(0xE2E2E7);
            c.track = Rgb(0xE9E9ED);
            c.line = Rgb(0x000000, 0.09f);
            c.text = Rgb(0x1A1A1E);
            c.textDim = Rgb(0x1A1A1E, 0.66f);
            c.textFaint = Rgb(0x1A1A1E, 0.40f);
            c.control = Rgb(0xFFFFFF);
            c.controlHover = Rgb(0xF5F5F7);
            c.controlDown = Rgb(0xEBEBEF);
            c.controlLine = Rgb(0x000000, 0.12f);
            c.accent = Luminance(t.accent) > 0.55f ? Darker(t.accent, 0.3f) : t.accent;
            c.accentHover = Lighter(c.accent, 0.12f);
            c.danger = Rgb(0xC42B1C);
            c.shadow = Rgb(0x000000, 0.18f);
        }
        c.onAccent = Luminance(c.accent) > 0.6f ? Rgb(0x111114) : Rgb(0xFFFFFF);
        c.accentSoft = Alpha(c.accent, c.dark ? 0.22f : 0.16f);
        c.dangerSoft = Alpha(c.danger, 0.24f);
        c.focus = c.text;
        return c;
    }

    // ---- Painter -------------------------------------------------------------------------------------

    bool Painter::Init(IDWriteFactory* dwrite)
    {
        m_dwrite = dwrite;
        const auto family = theme::TextFamily(dwrite);
        const auto icons = theme::IconFamily(dwrite);
        struct Spec
        {
            const std::wstring* family;
            float size;
            DWRITE_FONT_WEIGHT weight;
        };
        const Spec specs[9] = {
            { &family, 15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD },   // Title
            { &family, 13.f, DWRITE_FONT_WEIGHT_NORMAL },      // Body
            { &family, 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD },   // BodyStrong
            { &family, 12.f, DWRITE_FONT_WEIGHT_NORMAL },      // Small
            { &family, 11.f, DWRITE_FONT_WEIGHT_SEMI_BOLD },   // Caption
            { &icons, 16.f, DWRITE_FONT_WEIGHT_NORMAL },       // Icon
            { &icons, 12.f, DWRITE_FONT_WEIGHT_NORMAL },       // IconSmall
            { &family, 13.f, DWRITE_FONT_WEIGHT_NORMAL },      // Number
            { &family, 15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD },   // NumberLarge
        };
        for (int i = 0; i < 9; ++i)
        {
            if (FAILED(dwrite->CreateTextFormat(specs[i].family->c_str(), nullptr, specs[i].weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                specs[i].size, L"", &m_formats[i])))
                return false;
            m_formats[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            m_formats[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(m_formats[i].Get(), &ellipsis)))
                m_formats[i]->SetTrimming(&trimming, ellipsis.Get());
        }
        // Timecodes use tabular figures so they do not shuffle while playing.
        if (SUCCEEDED(dwrite->CreateTypography(&m_tabular)))
            m_tabular->AddFontFeature({ DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1 });
        return true;
    }

    void Painter::Begin(ID2D1RenderTarget* rt, const Colors& colors)
    {
        if (rt != m_rt)
            m_brush.Reset();
        m_rt = rt;
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
        const auto layout = text.empty() ? nullptr : Layout(text, font, 10000.f, 100.f);
        if (!layout)
            return 0;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
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

    // ---- Controls --------------------------------------------------------------------------------------

    namespace
    {
        constexpr float kRadius = 6.f;

        void FocusRing(Painter& p, const RectF& r, float radius)
        {
            p.Stroke({ r.x - 3, r.y - 3, r.w + 6, r.h + 6 }, p.C().focus, 2.f, radius + 3);
        }

        void DrawButton(Painter& p, const Control& c, const DrawState& s, bool primary)
        {
            const auto& k = p.C();
            D2D1_COLOR_F fill = primary ? (s.hover ? k.accentHover : k.accent) : (s.pressed ? k.controlDown : s.hover ? k.controlHover : k.control);
            if (primary && s.pressed)
                fill = Alpha(k.accent, 0.85f);
            const float a = c.enabled ? 1.f : 0.42f;
            p.Fill(c.rect, Alpha(fill, a), kRadius);
            if (!primary)
                p.Stroke(c.rect, k.controlLine, 1.f, kRadius);
            const auto ink = Alpha(primary ? k.onAccent : k.text, a);
            if (c.glyph && !c.text.empty())
            {
                const float tw = p.TextWidth(c.text, primary ? Font::BodyStrong : Font::Body);
                const float total = 16 + 8 + tw;
                const float x = c.rect.x + std::max(10.f, (c.rect.w - total) / 2);
                p.Text(c.glyph, { x, c.rect.y, 16, c.rect.h }, Font::IconSmall, ink, 1);
                p.Text(c.text, { x + 24, c.rect.y, c.rect.Right() - x - 24 - 6, c.rect.h }, primary ? Font::BodyStrong : Font::Body, ink);
            }
            else
                p.Text(c.text, c.rect.Inset(0), primary ? Font::BodyStrong : Font::Body, ink, 1);
        }

        void DrawIcon(Painter& p, const Control& c, const DrawState& s)
        {
            const auto& k = p.C();
            const float a = c.enabled ? 1.f : 0.35f;
            if (c.on)
                p.Fill(c.rect, k.accentSoft, kRadius);
            else if (s.pressed && c.enabled)
                p.Fill(c.rect, k.controlDown, kRadius);
            else if (s.hover && c.enabled)
                p.Fill(c.rect, k.controlHover, kRadius);
            p.Text(c.glyph ? c.glyph : L"", c.rect, c.rect.h < 26 ? Font::IconSmall : Font::Icon, Alpha(c.on ? k.accent : k.text, a), 1);
        }

        void DrawSegment(Painter& p, const Control& c, const DrawState& s)
        {
            const auto& k = p.C();
            const float a = c.enabled ? 1.f : 0.42f;
            if (c.on)
                p.Fill(c.rect, Alpha(k.accent, a), kRadius);
            else
            {
                p.Fill(c.rect, Alpha(s.pressed ? k.controlDown : s.hover ? k.controlHover : k.control, a), kRadius);
                p.Stroke(c.rect, k.controlLine, 1.f, kRadius);
            }
            p.Text(c.text, c.rect.Inset(2), c.on ? Font::BodyStrong : Font::Body, Alpha(c.on ? k.onAccent : k.text, a), 1);
        }

        void DrawToggle(Painter& p, const Control& c, const DrawState& s)
        {
            const auto& k = p.C();
            const float a = c.enabled ? 1.f : 0.42f;
            const RectF sw{ c.rect.Right() - 40, c.rect.y + (c.rect.h - 20) / 2, 40, 20 };
            p.Paragraph(c.text, { c.rect.x, c.rect.y + std::max(0.f, (c.rect.h - p.ParagraphHeight(c.text, c.rect.w - 52, Font::Body)) / 2), c.rect.w - 52, c.rect.h },
                        Font::Body, Alpha(k.text, a));
            if (c.on)
                p.Fill(sw, Alpha(s.hover ? k.accentHover : k.accent, a), 10);
            else
            {
                p.Fill(sw, Alpha(s.hover ? k.controlHover : k.control, a), 10);
                p.Stroke(sw, Alpha(k.textDim, a), 1.f, 10);
            }
            const float knob = c.on ? 14.f : 12.f;
            const float cx = c.on ? sw.Right() - 10 : sw.x + 10;
            p.Rt()->FillEllipse(D2D1::Ellipse({ cx, sw.y + 10 }, knob / 2, knob / 2), p.Brush(Alpha(c.on ? k.onAccent : k.textDim, a)));
            if (s.focused)
                FocusRing(p, sw, 10);
        }

        void DrawField(Painter& p, const Control& c, const DrawState& s)
        {
            const auto& k = p.C();
            RectF box = c.rect;
            if (!c.detail.empty())
            {
                p.Text(c.detail, { c.rect.x, c.rect.y, 64, c.rect.h }, Font::Body, Alpha(k.textDim, c.enabled ? 1.f : 0.5f));
                box = { c.rect.x + 64, c.rect.y, c.rect.w - 64, c.rect.h };
            }
            const bool editing = s.editText != nullptr;
            p.Fill(box, editing ? (k.dark ? k.stage : D2D1::ColorF(1, 1, 1, 1)) : s.hover && c.enabled ? k.controlHover : k.control, kRadius);
            p.Stroke(box, s.invalid ? k.danger : k.controlLine, s.invalid ? 1.5f : 1.f, kRadius);
            if (editing)
                p.Fill({ box.x + 2, box.Bottom() - 2, box.w - 4, 2 }, s.invalid ? k.danger : k.accent, 1);
            const std::wstring& text = editing ? *s.editText : c.text;
            const RectF inner{ box.x + 10, box.y, box.w - 20, box.h };
            if (editing && s.selectAll && !text.empty())
                p.Fill({ inner.x - 1, box.y + 6, p.TextWidth(text, Font::Number) + 2, box.h - 12 }, k.accentSoft, 2);
            p.Text(text, inner, Font::Number, Alpha(k.text, c.enabled ? 1.f : 0.45f));
            if (editing && !s.selectAll)
            {
                const float x = inner.x + p.CaretX(text, s.caret, Font::Number);
                p.Line(x, box.y + 7, x, box.Bottom() - 7, k.text, 1.f);
            }
            if (s.focused && !editing)
                FocusRing(p, box, kRadius);
        }

        void DrawSlider(Painter& p, const Control& c, const DrawState& s)
        {
            const auto& k = p.C();
            const float a = c.enabled ? 1.f : 0.42f;
            p.Text(c.text, { c.rect.x, c.rect.y, c.rect.w - 70, 20 }, Font::Body, Alpha(k.text, a));
            p.Text(c.detail, { c.rect.Right() - 70, c.rect.y, 70, 20 }, Font::Number, Alpha(k.textDim, a), 2);
            const float y = c.rect.y + 32;
            const float x0 = c.rect.x + 7, x1 = c.rect.Right() - 7;
            const float x = x0 + static_cast<float>(std::clamp(c.value, 0.0, 1.0)) * (x1 - x0);
            p.Fill({ x0, y - 2, x1 - x0, 4 }, Alpha(k.dark ? k.controlHover : k.controlDown, a), 2);
            p.Fill({ x0, y - 2, x - x0, 4 }, Alpha(k.accent, a), 2);
            const float r = s.pressed ? 8.f : s.hover ? 7.5f : 7.f;
            p.Rt()->FillEllipse(D2D1::Ellipse({ x, y }, r, r), p.Brush(Alpha(k.dark ? D2D1::ColorF(0x45454C) : D2D1::ColorF(1, 1, 1, 1), a)));
            p.Rt()->DrawEllipse(D2D1::Ellipse({ x, y }, r, r), p.Brush(Alpha(k.controlLine, a)), 1.f);
            p.Rt()->FillEllipse(D2D1::Ellipse({ x, y }, r - 3.5f, r - 3.5f), p.Brush(Alpha(k.accent, a)));
            if (s.focused)
                FocusRing(p, { x - r, y - r, 2 * r, 2 * r }, r);
        }

        void DrawRow(Painter& p, const Control& c, const DrawState& s)
        {
            const auto& k = p.C();
            if (c.on)
                p.Fill(c.rect, k.accentSoft, kRadius);
            else if (s.hover)
                p.Fill(c.rect, k.controlHover, kRadius);
            if (c.on)
                p.Fill({ c.rect.x, c.rect.y + 8, 3, c.rect.h - 16 }, k.accent, 1.5f);
            const float detailW = c.detail.empty() ? 0.f : std::min(c.rect.w * 0.6f, p.TextWidth(c.detail, Font::Number) + 4);
            p.Text(c.text, { c.rect.x + 12, c.rect.y, c.rect.w - 24 - detailW - 8, c.rect.h }, Font::Body, k.text);
            p.Text(c.detail, { c.rect.Right() - 12 - detailW, c.rect.y, detailW, c.rect.h }, Font::Number, k.textDim, 2);
        }
    }

    void DrawControl(Painter& p, const Control& c, const DrawState& s)
    {
        switch (c.kind)
        {
        case Control::Kind::Button: DrawButton(p, c, s, false); break;
        case Control::Kind::Primary: DrawButton(p, c, s, true); break;
        case Control::Kind::Icon: DrawIcon(p, c, s); break;
        case Control::Kind::Segment: DrawSegment(p, c, s); break;
        case Control::Kind::Toggle: DrawToggle(p, c, s); break;
        case Control::Kind::Field: DrawField(p, c, s); break;
        case Control::Kind::Slider: DrawSlider(p, c, s); break;
        case Control::Kind::Row: DrawRow(p, c, s); break;
        }
        const bool ringDrawn = c.kind == Control::Kind::Toggle || c.kind == Control::Kind::Field || c.kind == Control::Kind::Slider;
        if (s.focused && !ringDrawn)
            FocusRing(p, c.rect, kRadius);
    }
}
