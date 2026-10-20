#pragma once
#include "mediaeditor/EditorLayout.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <vector>

// The editor's look: colours that follow the Windows light/dark setting and accent, text styles, and
// a handful of controls drawn with Direct2D (buttons, segmented choices, toggles, sliders, text fields,
// list rows). The window lays the controls out each time something changes and routes input to them.
namespace sd::mediatools::ui
{
    struct Colors
    {
        bool dark = true;
        D2D1_COLOR_F window, panel, stage, track, line, text, textDim, textFaint;
        D2D1_COLOR_F control, controlHover, controlDown, controlLine;
        D2D1_COLOR_F accent, accentHover, onAccent, accentSoft;
        D2D1_COLOR_F danger, dangerSoft, focus, shadow;
    };
    Colors CurrentColors();
    D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a);
    D2D1_RECT_F ToD2D(const RectF& r);

    enum class Font { Title, Body, BodyStrong, Small, Caption, Icon, IconSmall, Number, NumberLarge };

    // Text formats, brushes and drawing helpers for one render target.
    class Painter
    {
    public:
        bool Init(IDWriteFactory* dwrite);
        void Begin(ID2D1RenderTarget* rt, const Colors& colors);
        ID2D1RenderTarget* Rt() const { return m_rt; }
        const Colors& C() const { return m_c; }

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
        Colors m_c{};
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
        enum class Kind { Button, Primary, Icon, Segment, Toggle, Field, Slider, Row };
        int id = 0;              // stable across layouts (focus and hover follow it)
        Kind kind = Kind::Button;
        RectF rect;
        std::wstring text;       // label; a field's value; a row's main text
        std::wstring detail;     // a row's second text, a slider's value, a field's label
        const wchar_t* glyph = nullptr;
        bool on = false;         // chosen segment, toggle on, selected row
        bool enabled = true;
        int segment = 0;         // 0 only, 1 first, 2 middle, 3 last (rounded ends of a segmented group)
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
    void DrawControl(Painter& p, const Control& c, const DrawState& s);
}
