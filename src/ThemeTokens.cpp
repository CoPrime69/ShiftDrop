#include "Theme.h"

#include <algorithm>
#include <cmath>

// The colour tokens and what each kind of window takes from them (Theme.h). Pure: no Windows calls,
// so the unit tests build them for both themes and any accent. The values are the brand kit's: the
// Brand Guidelines' colour page for the neutrals and status colours, the App UI page for the wheel,
// the top panel, the result card and the shelf, the App Windows and App Editors pages for the rest.
namespace sd::theme
{
    namespace
    {
        using namespace brand;

        D2D1_COLOR_F Ink(float alpha) { return Rgb(kInk, alpha); }
        D2D1_COLOR_F White(float alpha) { return Rgb(0xFFFFFF, alpha); }
        D2D1_COLOR_F Black(float alpha) { return Rgb(0x000000, alpha); }

        double Luminance(const D2D1_COLOR_F& c)
        {
            const auto channel = [](float v) {
                const double x = std::clamp(static_cast<double>(v), 0.0, 1.0);
                return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b);
        }
    }

    D2D1_COLOR_F Rgb(uint32_t rgb, float alpha)
    {
        return D2D1::ColorF(rgb & 0xFFFFFF, alpha);
    }

    uint32_t RgbOf(const D2D1_COLOR_F& c)
    {
        const auto byte = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
        return (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
    }

    D2D1_COLOR_F Over(const D2D1_COLOR_F& top, const D2D1_COLOR_F& bottom)
    {
        const float a = top.a + bottom.a * (1.f - top.a);
        if (a <= 0.f)
            return D2D1::ColorF(0, 0, 0, 0);
        const auto mix = [&](float t, float b) { return (t * top.a + b * bottom.a * (1.f - top.a)) / a; };
        return D2D1::ColorF(mix(top.r, bottom.r), mix(top.g, bottom.g), mix(top.b, bottom.b), a);
    }

    double Contrast(const D2D1_COLOR_F& text, const D2D1_COLOR_F& background, const D2D1_COLOR_F& under)
    {
        const auto back = Over(background, WithAlpha(under, 1.f / std::max(under.a, 1e-3f)));
        const auto fore = Over(text, back);
        const double a = Luminance(fore), b = Luminance(back);
        return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
    }

    Tokens TokensFor(bool dark)
    {
        Tokens t{};
        t.dark = dark;
        t.paper = Rgb(kN0);
        t.paperText = Rgb(kInk);
        t.paperFaint = Rgb(kN500);
        t.paperLine = Rgb(kN200);
        if (dark)
        {
            t.window = Rgb(0x1B1D22);
            t.card = Rgb(0x24272D);
            t.cardLine = White(0.06f);
            t.divider = White(0.06f);
            t.navSelected = White(0.06f);
            t.text = Rgb(kDarkText);
            t.subtleText = Rgb(kDarkText2);
            // Text 3 a step lighter, so hints read on the fields and controls too, not only on the window.
            t.faintText = Rgb(0x949AA5);
            t.control = Rgb(0x2E3138);
            t.controlHover = Rgb(0x353A44);
            t.controlDown = Rgb(0x2A2D33);
            t.controlLine = White(0.08f);
            t.field = Rgb(0x2A2D33);
            t.border = Rgb(kDarkBorder);
            t.strongBorder = Rgb(kDarkText2);
            t.stage = Rgb(0x111317);
            t.track = Rgb(0x16181E);
            t.ok = Rgb(kOkOnDark);
            t.warn = Rgb(kWarnOnDark);
            t.error = Rgb(kErrorOnDark);
            t.info = Rgb(kInfoOnDark);
            t.shadow = Black(0.6f);
        }
        else
        {
            t.window = Rgb(0xF3F4F6);
            t.card = Rgb(kN0);
            t.cardLine = Ink(0.07f);
            t.divider = Ink(0.07f);
            t.navSelected = Ink(0.06f);
            t.text = Rgb(kInk);
            t.subtleText = Rgb(kN600);
            // N500 a step darker, so hints read on the grey window and surface 2 too, not only on white.
            t.faintText = Rgb(0x676D7B);
            t.control = Rgb(kN0);
            t.controlHover = Rgb(kN25);
            t.controlDown = Rgb(kN50);
            t.controlLine = Ink(0.12f);
            t.field = Rgb(kN0);
            t.border = Rgb(kN100);
            t.strongBorder = Rgb(kN600);
            t.stage = Rgb(0xE4E7EB);
            t.track = Rgb(kN50);
            t.ok = Rgb(kOk);
            t.warn = Rgb(kWarn);
            t.error = Rgb(kError);
            t.info = Rgb(kInfo);
            t.shadow = Rgb(0x14161E, 0.16f);
        }
        return t;
    }

    Palette MakePalette(Style style, bool dark, const accent::Variants& tones)
    {
        const auto t = TokensFor(dark);
        const bool glass = style == Style::Glass;
        Palette p{};
        p.dark = dark;
        // The hovered tile and the active section: the accent's light tone with white on it in both
        // themes, a little lighter toward the rim. As a mark (the current wheel's dot, progress) the
        // tone that reads on the theme.
        p.accent = Rgb(tones.light);
        p.textOnAccent = Rgb(accent::kTextOnLight);
        p.accentHi = Rgb(accent::Lighter(tones.light, accent::kTextOnLight, 0.20));
        p.accentMark = Rgb(dark ? tones.dark : tones.light);
        p.panelAccent = p.accent;
        p.panelTextOnAccent = p.textOnAccent;
        p.ok = t.ok;
        p.warn = t.warn;
        p.error = t.error;
        p.info = t.info;
        // The Glass tint keeps the strength the app had: its snapshot blur is lighter than the design's
        // and has no saturation boost, so a weaker tint would let busy backgrounds through the text.
        if (dark)
        {
            p.text = Rgb(0xEEF0F3);
            p.subtleText = Rgb(0xA9AEB8);
            p.shadow = Black(0.5f);
            p.disc = glass ? Rgb(0x22242A, 0.66f) : Rgb(0x1A1C22);
            p.discLine = White(glass ? 0.12f : 0.08f);
            p.tile = glass ? White(0.07f) : Rgb(0x2A2D35);
            p.tileLine = White(glass ? 0.09f : 0.06f);
            p.centre = glass ? Rgb(0x0C0E12, 0.45f) : Rgb(0x141619);
            p.centreLine = White(glass ? 0.12f : 0.08f);
            p.dot = Rgb(0xEEF0F3, 0.28f);
            p.panel = glass ? Rgb(0x1C1E24, 0.86f) : Rgb(0x1F2127);
            p.panelLine = White(0.10f);
            p.panelText = Rgb(0xEEF0F3);
            p.panelTextDim = Rgb(0x9DA2AC);
            p.keycap = White(0.12f);
            p.hover = White(0.07f);
            p.track = Rgb(0x353A44);
            p.page = Rgb(0x1E2128);
            p.pageLine = Rgb(0x353A44);
        }
        else
        {
            p.text = Rgb(0x1B1D23);
            p.subtleText = Rgb(0x5E6370);
            p.shadow = Rgb(0x14161E, 0.18f);
            p.disc = glass ? Rgb(0xFAFBFC, 0.62f) : Rgb(kN0);
            p.discLine = Ink(0.10f);
            p.tile = glass ? White(0.55f) : Rgb(kN50);
            p.tileLine = glass ? Ink(0.10f) : Rgb(kN200);
            p.centre = glass ? White(0.82f) : Rgb(kN0);
            p.centreLine = Ink(0.10f);
            p.dot = Rgb(0x1B1D23, 0.25f);
            p.panel = glass ? White(0.86f) : Rgb(kN0);
            p.panelLine = Ink(0.08f);
            p.panelText = Rgb(0x1B1D23);
            p.panelTextDim = Rgb(0x5E6370);
            p.keycap = Ink(0.07f);
            p.hover = Ink(0.05f);
            p.track = Rgb(kN100);
            p.page = Rgb(kN0);
            p.pageLine = Rgb(kN200);
        }
        p.glassTint = p.disc;
        // The file mark's corner says "done": the accent's own colour in light, its light-on-dark tone in dark.
        p.pageMark = Rgb(dark ? tones.dark : tones.fill);
        // A dimmed tile's label (an action that can't take these files). Disabled text is exempt from
        // the contrast minimum; the reason is spelled out in the centre.
        p.textDim = WithAlpha(p.text, 0.38f);
        return p;
    }

    EditorPalette MakeEditorColors(bool dark, const accent::Variants& tones)
    {
        const auto t = TokensFor(dark);
        EditorPalette c{};
        c.dark = dark;
        // Buttons, switches and accent text: the dark tone with dark text on it in dark mode, the light
        // tone with white text in light mode; hover a little lighter while its text still reads.
        const uint32_t accentRgb = dark ? tones.dark : tones.light, onAccent = dark ? accent::kTextOnDark : accent::kTextOnLight;
        c.accent = Rgb(accentRgb);
        c.accentHover = Rgb(accent::Lighter(accentRgb, onAccent, dark ? 0.34 : 0.12));
        c.onAccent = Rgb(onAccent);
        c.accentSoft = WithAlpha(c.accent, dark ? 0.22f : 0.16f);
        c.window = t.window;
        c.panel = t.card;
        c.stage = t.stage;
        c.track = t.track;
        c.line = t.divider;
        c.text = t.text;
        c.textDim = t.subtleText;
        c.textFaint = t.faintText;
        c.control = t.control;
        c.controlHover = t.controlHover;
        c.controlDown = t.controlDown;
        c.controlLine = t.controlLine;
        c.field = t.field;
        c.thumb = dark ? t.control : t.card;
        c.selected = t.navSelected;
        c.border = t.border;
        c.danger = t.error;
        c.dangerSoft = WithAlpha(c.danger, 0.24f);
        c.onDanger = Rgb(accent::TextOn(dark ? kErrorOnDark : kError));
        c.focus = t.text;
        c.shadow = t.shadow;
        c.ok = t.ok;
        c.warn = t.warn;
        c.info = t.info;
        c.paper = t.paper;
        c.paperText = t.paperText;
        c.paperFaint = t.paperFaint;
        c.paperLine = t.paperLine;
        c.chipLine = dark ? Rgb(0x353A44) : Rgb(kN200);
        c.outline = t.strongBorder;
        c.fieldLine = dark ? Rgb(kDarkText3) : Rgb(kN500);
        return c;
    }

    DialogColors MakeDialog(bool dark, const accent::Variants& tones, int lightBackground)
    {
        const auto t = TokensFor(dark);
        DialogColors c;
        c.dark = dark;
        const auto background = !dark && lightBackground == COLOR_WINDOW ? t.card : t.window;
        const auto opaque = [&](const D2D1_COLOR_F& colour, const D2D1_COLOR_F& on) { return ToColorRef(Over(colour, on)); };
        c.background = ToColorRef(background);
        c.text = ToColorRef(t.text);
        c.field = ToColorRef(t.field);
        c.card = ToColorRef(t.card);
        c.cardLine = opaque(t.cardLine, t.card);
        c.divider = opaque(t.divider, t.card);
        c.navSelected = opaque(t.navSelected, t.window);
        c.subtleText = ToColorRef(t.subtleText);
        c.muted = ToColorRef(t.faintText);
        c.problem = ToColorRef(t.error);
        c.ok = ToColorRef(t.ok);
        c.warn = ToColorRef(t.warn);
        c.info = ToColorRef(t.info);
        c.accentText = ToColorRef(Rgb(dark ? tones.dark : tones.light));
        return c;
    }

    COLORREF ToColorRef(const D2D1_COLOR_F& c)
    {
        const uint32_t rgb = RgbOf(c);
        return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    }

    D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t)
    {
        return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
    }

    D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& c, float alphaFactor)
    {
        return D2D1::ColorF(c.r, c.g, c.b, c.a * alphaFactor);
    }
}
