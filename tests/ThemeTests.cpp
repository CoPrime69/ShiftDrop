#include "Accent.h"
#include "Theme.h"

#include <gtest/gtest.h>

#include <format>
#include <string>
#include <vector>

using namespace sd;
using namespace sd::theme;

namespace
{
    constexpr double kText = accent::kMinContrast;   // WCAG AA for body text
    constexpr double kMark = 3.0;                     // WCAG for graphics: dots, progress, outlines

    uint32_t FromColorRef(COLORREF c) { return (static_cast<uint32_t>(GetRValue(c)) << 16) | (GetGValue(c) << 8) | GetBValue(c); }

    std::string Hex(const D2D1_COLOR_F& c) { return std::format("#{:06X}/{:.2f}", FromColorRef(ToColorRef(c)), c.a); }

    // A few accents: the first presets, the warmest, the greyest, the Windows default blue and two
    // colours of the user's own that have to be moved a long way (gold, pure green).
    std::vector<std::pair<std::string, accent::Variants>> Accents()
    {
        return { { "Tide", accent::Preset(accent::Choice::Tide) },
                 { "Blue", accent::Preset(accent::Choice::Blue) },
                 { "Orange", accent::Preset(accent::Choice::Orange) },
                 { "Graphite", accent::Preset(accent::Choice::Graphite) },
                 { "Windows blue", accent::Derive(0x0067C0) },
                 { "gold", accent::Derive(0xFFB900) },
                 { "green", accent::Derive(0x00FF00) } };
    }

#define EXPECT_READS(text, background, under, minimum, what)                                                                    \
    EXPECT_GE(Contrast(text, background, under), minimum) << (what) << ": " << Hex(text) << " on " << Hex(background) << " over " \
                                                          << Hex(under)
}

TEST(Theme, TokensAreTheKitsColours)
{
    const auto light = TokensFor(false), dark = TokensFor(true);
    EXPECT_EQ(ToColorRef(light.text), RGB(0x15, 0x17, 0x1E));   // Ink
    EXPECT_EQ(ToColorRef(light.card), RGB(0xFF, 0xFF, 0xFF));
    EXPECT_EQ(ToColorRef(light.subtleText), RGB(0x4A, 0x4F, 0x5A));
    EXPECT_EQ(ToColorRef(light.error), RGB(0xC0, 0x2B, 0x2B));
    EXPECT_EQ(ToColorRef(dark.text), RGB(0xF2, 0xF3, 0xF5));
    EXPECT_EQ(ToColorRef(dark.subtleText), RGB(0xB4, 0xB9, 0xC3));
    EXPECT_EQ(ToColorRef(dark.ok), RGB(0x4A, 0xDE, 0x80));
    EXPECT_FALSE(light.dark);
    EXPECT_TRUE(dark.dark);
    // Paper is light in both themes: dark ink is drawn on it.
    EXPECT_EQ(ToColorRef(dark.paper), RGB(0xFF, 0xFF, 0xFF));
}

TEST(Theme, CompositingAndContrast)
{
    const auto grey = Over(D2D1::ColorF(1, 1, 1, 0.5f), D2D1::ColorF(0, 0, 0, 1));
    EXPECT_NEAR(grey.r, 0.5f, 1e-4f);
    EXPECT_FLOAT_EQ(grey.a, 1.f);
    EXPECT_NEAR(Contrast(D2D1::ColorF(0, 0, 0, 1), D2D1::ColorF(1, 1, 1, 1)), 21.0, 1e-6);
    // See-through text counts as laid over its background: white at 10% on black barely shows.
    EXPECT_LT(Contrast(D2D1::ColorF(1, 1, 1, 0.1f), D2D1::ColorF(0, 0, 0, 1)), 1.5);
    // The kit's own check: text 2 on white is 8.21 : 1.
    EXPECT_NEAR(Contrast(Rgb(brand::kN600), Rgb(brand::kN0)), 8.21, 0.01);
}

TEST(Theme, WindowTextReadsInBothThemes)
{
    for (const bool dark : { false, true })
    {
        const auto t = TokensFor(dark);
        const std::string theme = dark ? "dark" : "light";
        for (const auto& [name, back] : std::vector<std::pair<std::string, D2D1_COLOR_F>>{
                 { "window", t.window }, { "card", t.card }, { "selected item", Over(t.navSelected, t.window) } })
        {
            const auto what = theme + " " + name;
            EXPECT_READS(t.text, back, t.window, kText, what + " text");
            EXPECT_READS(t.subtleText, back, t.window, kText, what + " subtle text");
            EXPECT_READS(t.faintText, back, t.window, kText, what + " faint text");
            for (const auto& status : { t.ok, t.warn, t.error, t.info })
                EXPECT_READS(status, back, t.window, kText, what + " status");
        }
        EXPECT_READS(t.text, t.control, t.window, kText, theme + " button");
        EXPECT_READS(t.text, t.field, t.window, kText, theme + " field");
        EXPECT_READS(t.strongBorder, t.card, t.window, kMark, theme + " switch outline");
        EXPECT_READS(t.paperText, t.paper, t.window, kText, theme + " ink on paper");
        EXPECT_READS(t.paperFaint, t.paper, t.window, kText, theme + " hint on paper");
    }
}

TEST(Theme, WheelAndPanelTextReadsForEveryStyleAndAccent)
{
    for (const bool dark : { false, true })
        for (const auto style : { Style::Solid, Style::Glass })
            for (const auto& [name, tones] : Accents())
            {
                const auto p = MakePalette(style, dark, tones);
                // The Glass plate is see-through: judged over a window of the same theme behind it.
                const auto under = TokensFor(dark).window;
                const auto plate = Over(p.disc, under);
                const auto what = std::format("{} {} {}", dark ? "dark" : "light", style == Style::Glass ? "glass" : "solid", name);
                EXPECT_EQ(p.dark, dark);
                EXPECT_READS(p.text, p.tile, plate, kText, what + " tile label");
                EXPECT_READS(p.text, p.centre, plate, kText, what + " centre");
                EXPECT_READS(p.subtleText, p.centre, plate, kText, what + " centre detail");
                EXPECT_READS(p.textOnAccent, p.accent, plate, kText, what + " hovered tile");
                EXPECT_READS(p.textOnAccent, p.accentHi, plate, kText, what + " hovered tile's rim");
                EXPECT_READS(p.accentMark, p.centre, plate, kMark, what + " the wheel's dot");
                EXPECT_READS(p.panelText, p.panel, under, kText, what + " panel");
                EXPECT_READS(p.panelTextDim, p.panel, under, kText, what + " panel's other sections");
                EXPECT_READS(p.panelTextOnAccent, p.panelAccent, Over(p.panel, under), kText, what + " active section");
                EXPECT_READS(p.panelText, Over(p.hover, p.panel), under, kText, what + " hovered row");
                for (const auto& status : { p.ok, p.warn, p.error, p.info })
                    EXPECT_READS(status, p.panel, under, kText, what + " status");
                // A dimmed tile looks dimmed: clearly fainter than an enabled one.
                EXPECT_LT(Contrast(p.textDim, p.tile, plate), Contrast(p.text, p.tile, plate) / 2.0) << what;
            }
}

TEST(Theme, EditorTextReadsForEveryAccent)
{
    for (const bool dark : { false, true })
        for (const auto& [name, tones] : Accents())
        {
            const auto c = MakeEditorColors(dark, tones);
            const auto what = std::format("{} {}", dark ? "dark" : "light", name);
            for (const auto& back : { c.window, c.panel })
            {
                EXPECT_READS(c.text, back, c.window, kText, what + " text");
                EXPECT_READS(c.textDim, back, c.window, kText, what + " secondary text");
                EXPECT_READS(c.textFaint, back, c.window, kText, what + " hint");
                EXPECT_READS(c.accent, back, c.window, kText, what + " accent text");
                EXPECT_READS(c.danger, back, c.window, kText, what + " danger text");
                for (const auto& status : { c.ok, c.warn, c.info })
                    EXPECT_READS(status, back, c.window, kText, what + " status");
            }
            EXPECT_READS(c.onAccent, c.accent, c.window, kText, what + " button");
            EXPECT_READS(c.onAccent, c.accentHover, c.window, kText, what + " hovered button");
            EXPECT_READS(c.onDanger, c.danger, c.window, kText, what + " remove mark");
            EXPECT_READS(c.text, c.control, c.window, kText, what + " control");
            EXPECT_READS(c.text, c.controlHover, c.window, kText, what + " hovered control");
            EXPECT_READS(c.text, c.field, c.window, kText, what + " field");
            EXPECT_READS(c.text, Over(c.selected, c.window), c.window, kText, what + " selected tab");
            EXPECT_READS(c.paperText, c.paper, c.window, kText, what + " ink on paper");
            EXPECT_READS(c.focus, c.window, c.window, kMark, what + " focus ring");
        }
}

TEST(Theme, DialogColoursAreOpaqueAndRead)
{
    for (const bool dark : { false, true })
        for (const int background : { COLOR_WINDOW, COLOR_BTNFACE })
            for (const auto& [name, tones] : Accents())
            {
                const auto c = MakeDialog(dark, tones, background);
                const auto what = std::format("{} {} {}", dark ? "dark" : "light", background == COLOR_WINDOW ? "window" : "button face", name);
                EXPECT_EQ(c.dark, dark);
                for (const COLORREF back : { c.background, c.field, c.card, c.navSelected })
                    for (const COLORREF text : { c.text, c.subtleText, c.muted, c.problem, c.ok, c.warn, c.info, c.accentText })
                        EXPECT_GE(accent::Contrast(FromColorRef(text), FromColorRef(back)), kText)
                            << what << std::format(": #{:06X} on #{:06X}", FromColorRef(text), FromColorRef(back));
                // Lines show on the card they edge.
                EXPECT_NE(c.cardLine, c.card) << what;
                EXPECT_NE(c.divider, c.card) << what;
            }
    // Light windows of standard controls keep their kind: a list window is white, a form window grey.
    EXPECT_EQ(MakeDialog(false, accent::Preset(accent::Choice::Tide), COLOR_WINDOW).background, RGB(0xFF, 0xFF, 0xFF));
    EXPECT_NE(MakeDialog(false, accent::Preset(accent::Choice::Tide), COLOR_BTNFACE).background, RGB(0xFF, 0xFF, 0xFF));
}

TEST(Theme, TheAccentFollowsItsTones)
{
    const auto tide = accent::Preset(accent::Choice::Tide);
    for (const bool dark : { false, true })
    {
        // The hovered tile is the light tone with white on it in both themes, as the kit draws it.
        const auto p = MakePalette(Style::Solid, dark, tide);
        EXPECT_EQ(ToColorRef(p.accent), RGB(0x0B, 0x7A, 0x69));
        EXPECT_EQ(ToColorRef(p.textOnAccent), RGB(0xFF, 0xFF, 0xFF));
        // As a mark (the current wheel's dot) and in the editors, the tone for the theme.
        EXPECT_EQ(ToColorRef(p.accentMark), dark ? RGB(0x2D, 0xD4, 0xB4) : RGB(0x0B, 0x7A, 0x69));
        EXPECT_EQ(ToColorRef(MakeEditorColors(dark, tide).accent), dark ? RGB(0x2D, 0xD4, 0xB4) : RGB(0x0B, 0x7A, 0x69));
    }
}
