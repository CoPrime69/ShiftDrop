#pragma once
#include "Theme.h"
#include "mediaeditor/EditorLayout.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <vector>

// The timeline editor's text styles and its controls (buttons, segmented choices, toggles, sliders,
// text fields, list rows), in the editor palette (theme::EditorColors). The window lays the controls
// out each time something changes, routes input to them and draws them with EditorFrame's kit.
namespace sd::mediatools::ui
{
    D2D1_RECT_F ToD2D(const RectF& r);

    enum class Font { Title, Body, BodyStrong, Small, Caption, Icon, IconSmall, Number, NumberLarge };

    // Text styles, a brush and drawing helpers for one render target.
    class Painter
    {
    public:
        // The window's text formats (EditorFrame's cache): one-line, vertically centred, trimmed.
        using FormatFn = std::function<IDWriteTextFormat*(float sizeDip, DWRITE_FONT_WEIGHT weight, bool icon)>;
        bool Init(IDWriteFactory* dwrite, const FormatFn& format);
        // Each paint; `device` is the target's DeviceGeneration (the brush is remade for a new one).
        void Begin(ID2D1RenderTarget* rt, const theme::EditorPalette& colors, unsigned device);
        ID2D1RenderTarget* Rt() const { return m_rt; }
        const theme::EditorPalette& C() const { return m_c; }

        ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& color);
        void Fill(const RectF& r, const D2D1_COLOR_F& color, float radius = 0);
        void Stroke(const RectF& r, const D2D1_COLOR_F& color, float width, float radius = 0);
        void Line(float x0, float y0, float x1, float y1, const D2D1_COLOR_F& color, float width = 1);
        // Text in `r`: align 0 left, 1 centre, 2 right; vertically centred. Clipped with an ellipsis.
        void Text(const std::wstring& text, const RectF& r, Font font, const D2D1_COLOR_F& color, int align = 0);
        // Wrapped text from the top of `r`; returns the height used.
        float Paragraph(const std::wstring& text, const RectF& r, Font font, const D2D1_COLOR_F& color);
        float TextWidth(const std::wstring& text, Font font);
        float ParagraphHeight(const std::wstring& text, float width, Font font);
        // Where the caret goes after `chars` characters of `text`, from the left of the text.
        float CaretX(const std::wstring& text, size_t chars, Font font);
        size_t CharAt(const std::wstring& text, float x, Font font);
        IDWriteTextFormat* Format(Font font) const { return m_formats[static_cast<int>(font)].Get(); }

    private:
        Microsoft::WRL::ComPtr<IDWriteTextLayout> Layout(const std::wstring& text, Font font, float w, float h);

        IDWriteFactory* m_dwrite = nullptr;
        ID2D1RenderTarget* m_rt = nullptr;
        unsigned m_device = 0;
        theme::EditorPalette m_c{};
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_formats[9];
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush;
        Microsoft::WRL::ComPtr<IDWriteTypography> m_tabular;
    };

    // Segoe Fluent Icons / MDL2 Assets code points.
    namespace icon
    {
        inline constexpr wchar_t Play[] = L"\uE768";
        inline constexpr wchar_t Pause[] = L"\uE769";
        inline constexpr wchar_t Back[] = L"\uE76B";
        inline constexpr wchar_t Forward[] = L"\uE76C";
        inline constexpr wchar_t ZoomIn[] = L"\uE8A3";
        inline constexpr wchar_t ZoomOut[] = L"\uE71F";
        inline constexpr wchar_t Fit[] = L"\uE9A6";
        inline constexpr wchar_t Delete[] = L"\uE74D";
        inline constexpr wchar_t Add[] = L"\uE710";
        inline constexpr wchar_t Up[] = L"\uE70E";
        inline constexpr wchar_t Down[] = L"\uE70D";
        inline constexpr wchar_t Picture[] = L"\uEB9F";
        inline constexpr wchar_t Warning[] = L"\uE7BA";
        inline constexpr wchar_t Close[] = L"\uE711";
        inline constexpr wchar_t Music[] = L"\uE8D6";
    }

    struct Control
    {
        // Tab: a tool's tab on the tool bar (glyph over text); Segment: an option of a segmented choice
        // (`part` says which part of the track it draws); Chip: an option of a choice that wraps.
        enum class Kind { Button, Primary, Icon, Segment, Toggle, Field, Slider, Row, Tab, Chip };
        int id = 0;              // stable across layouts (focus and hover follow it)
        Kind kind = Kind::Button;
        RectF rect;
        std::wstring text;       // label; a field's value; a row's main text
        std::wstring detail;     // a row's second text, a slider's value, a field's label
        const wchar_t* glyph = nullptr;
        wchar_t icon = 0;        // a tab's icon
        int part = 0;            // a segment's place: alone, first, middle, last (ControlKit::Part)
        bool on = false;         // chosen segment, toggle on, selected row
        bool enabled = true;
        double value = 0;        // slider position 0..1
        std::wstring tip;        // shown in the footer while hovered
        std::function<void()> click;
        std::function<void(double)> slide;                 // slider: new position 0..1
        std::function<void(int)> step;                     // arrow keys on a focused control: -1 / +1
        std::function<bool(const std::wstring&)> commit;   // field: false = not accepted (stays red)
    };

    struct DrawState
    {
        bool hover = false, pressed = false, focused = false;
        const std::wstring* editText = nullptr;   // a field being typed in
        size_t caret = 0;
        bool selectAll = false;
        bool invalid = false;
    };
}
