#include "Accent.h"
#include "EditorChrome.h"
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
                 { "window", t.window }, { "card", t.card }, { "control", t.control }, { "field", t.field } })
        {
            const auto what = theme + " " + name;
            EXPECT_READS(t.text, back, t.window, kText, what + " text");
            EXPECT_READS(t.subtleText, back, t.window, kText, what + " subtle text");
            EXPECT_READS(t.faintText, back, t.window, kText, what + " faint text");
            for (const auto& status : { t.ok, t.warn, t.error, t.info })
                EXPECT_READS(status, back, t.window, kText, what + " status");
        }
        // The selected navigation item holds a name and an icon.
        EXPECT_READS(t.text, Over(t.navSelected, t.window), t.window, kText, theme + " selected item");
        EXPECT_READS(t.subtleText, Over(t.navSelected, t.window), t.window, kText, theme + " selected item's detail");
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
                EXPECT_READS(p.accentMark, p.track, Over(p.panel, under), kMark, what + " progress");
                EXPECT_READS(p.pageLine, p.panel, under, 1.2, what + " file mark's edge");
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
                const auto reads = [&](COLORREF text, COLORREF back) {
                    EXPECT_GE(accent::Contrast(FromColorRef(text), FromColorRef(back)), kText)
                        << what << std::format(": #{:06X} on #{:06X}", FromColorRef(text), FromColorRef(back));
                };
                for (const COLORREF back : { c.background, c.field, c.card })
                    for (const COLORREF text : { c.text, c.subtleText, c.muted, c.problem, c.ok, c.warn, c.info, c.accentText })
                        reads(text, back);
                // The selected navigation item holds a name and an icon.
                reads(c.text, c.navSelected);
                reads(c.subtleText, c.navSelected);
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

TEST(Theme, EditorControlLinesShow)
{
    for (const bool dark : { false, true })
        for (const auto& [name, tones] : Accents())
        {
            const auto c = MakeEditorColors(dark, tones);
            const auto what = std::format("{} {}", dark ? "dark" : "light", name);
            // An off switch and the line under a field are the control's only edge: WCAG's 3:1 for graphics.
            EXPECT_READS(c.outline, c.panel, c.window, kMark, what + " off switch");
            EXPECT_READS(c.outline, c.window, c.window, kMark, what + " off switch on the window");
            EXPECT_READS(c.fieldLine, c.field, c.window, kMark, what + " field line");
            // A chip's outline and a slider's bar are lighter but still show on the panel.
            EXPECT_NE(ToColorRef(c.chipLine), ToColorRef(c.panel)) << what;
            // A chosen chip's or tile's text and check mark read on its tint.
            EXPECT_READS(c.text, Over(c.accentSoft, c.panel), c.window, kText, what + " chosen chip");
            EXPECT_READS(c.accent, Over(c.accentSoft, c.panel), c.window, kMark, what + " chip's check mark");
            // A chosen segment's text on the raised option, an unchosen one's on the track (the window).
            EXPECT_READS(c.text, c.control, c.window, kText, what + " chosen segment");
            EXPECT_READS(c.textDim, c.window, c.window, kText, what + " segment");
        }
    // The kit's values: N200 / #353A44 for chips, N600 / Text 2 for the off switch, N500 / Text 3 under fields.
    EXPECT_EQ(ToColorRef(MakeEditorColors(false, accent::Preset(accent::Choice::Tide)).chipLine), RGB(0xD5, 0xD8, 0xDE));
    EXPECT_EQ(ToColorRef(MakeEditorColors(true, accent::Preset(accent::Choice::Tide)).chipLine), RGB(0x35, 0x3A, 0x44));
    EXPECT_EQ(ToColorRef(MakeEditorColors(false, accent::Preset(accent::Choice::Tide)).outline), RGB(0x4A, 0x4F, 0x5A));
    EXPECT_EQ(ToColorRef(MakeEditorColors(true, accent::Preset(accent::Choice::Tide)).fieldLine), RGB(0x8A, 0x90, 0x9C));
}

TEST(Theme, FileTagLettersRead)
{
    // White letters on every PDF's tag colour, which sits on the window, a card or the picture's well.
    for (const uint32_t tag : brand::kFileTags)
        EXPECT_GE(accent::Contrast(0xFFFFFF, tag), kText) << std::format("#{:06X}", tag);
    EXPECT_EQ(brand::kFileTags[0], 0x0B7A69u);   // A: Tide, as the kit draws it
}

TEST(EditorChrome, FrameSplitsTheWindow)
{
    const auto f = chrome::LayoutFrame(1240, 760);
    // The tool bar along the top, the footer along the bottom, the panel on the right.
    EXPECT_FLOAT_EQ(f.toolBar.bottom, chrome::kToolBarHeight);
    EXPECT_FLOAT_EQ(f.footer.top, 760 - chrome::kFooterHeight);
    EXPECT_FLOAT_EQ(f.panel.right - f.panel.left, chrome::kPanelWidth);
    EXPECT_FLOAT_EQ(f.stage.right, f.panel.left);
    EXPECT_FLOAT_EQ(f.stage.top, f.toolBar.bottom);
    EXPECT_FLOAT_EQ(f.stage.bottom, f.footer.top);
    EXPECT_FLOAT_EQ(f.panel.top, f.stage.top);
    EXPECT_FLOAT_EQ(f.panel.bottom, f.stage.bottom);
    // No panel: the stage takes the width.
    EXPECT_FLOAT_EQ(chrome::LayoutFrame(800, 600, false).stage.right, 800);
    // A window too small for its parts never gets negative ones.
    const auto tiny = chrome::LayoutFrame(100, 50);
    EXPECT_GE(tiny.stage.right, tiny.stage.left);
    EXPECT_GE(tiny.stage.bottom, tiny.stage.top);
    EXPECT_GE(tiny.footer.top, tiny.toolBar.bottom);
}

TEST(EditorChrome, TabsAndSegments)
{
    EXPECT_FLOAT_EQ(chrome::TabWidth(10), chrome::kTabMinWidth);           // "Edit": the kit's 56 DIP minimum
    EXPECT_FLOAT_EQ(chrome::TabWidth(70), 70 + 2 * chrome::kTabPadX);       // "Background"
    // Options side by side, touching (each draws its part of the track), with the kit's padding.
    const auto s = chrome::Segments({ 40, 40, 50 }, 10, 20, 280);
    ASSERT_EQ(s.size(), 3u);
    EXPECT_FLOAT_EQ(s[0].left, 10);
    EXPECT_FLOAT_EQ(s[0].right - s[0].left, 40 + 2 * chrome::kSegmentPadX);
    for (size_t i = 1; i < s.size(); ++i)
        EXPECT_FLOAT_EQ(s[i].left, s[i - 1].right);
    EXPECT_FLOAT_EQ(s[2].right, 10 + 130 + 6 * chrome::kSegmentPadX);
    EXPECT_FLOAT_EQ(s[0].bottom - s[0].top, chrome::kSegmentHeight);
    // A little too wide with the kit's padding: less padding, still one row within the width.
    const auto tight = chrome::Segments({ 50, 50, 50, 50 }, 0, 0, 280);
    ASSERT_EQ(tight.size(), 4u);
    EXPECT_LE(tight[3].right, 280.f);
    EXPECT_LT(tight[1].right - tight[1].left, 50 + 2 * chrome::kSegmentPadX);
    EXPECT_GE(tight[1].right - tight[1].left, 50 + 2 * chrome::kSegmentMinPadX);
    // Too wide even then: no segments (the caller shows chips).
    EXPECT_TRUE(chrome::Segments({ 120, 120, 120 }, 0, 0, 280).empty());
    EXPECT_TRUE(chrome::Segments({}, 0, 0, 280).empty());
}

TEST(EditorChrome, ChipsFlowOntoRows)
{
    // Seven ratios in a 280 DIP panel: rows of what fits, 6 apart.
    const std::vector<float> widths = { 60, 80, 50, 70, 50, 60, 60 };
    const auto r = chrome::Flow(widths, 20, 300, 100, 32, 6, 6);
    ASSERT_EQ(r.size(), widths.size());
    EXPECT_FLOAT_EQ(r[0].left, 20);
    EXPECT_FLOAT_EQ(r[1].left, 86);
    for (const auto& rect : r)
    {
        EXPECT_LE(rect.right, 300.5f);
        EXPECT_FLOAT_EQ(rect.bottom - rect.top, 32);
    }
    EXPECT_FLOAT_EQ(r[3].top, 100);   // 20..80, 86..166, 172..222, 228..298
    EXPECT_FLOAT_EQ(r[4].top, 138);   // a new row
    EXPECT_FLOAT_EQ(r[4].left, 20);
    // An item wider than the row gets a row of its own, cut to the row.
    const auto wide = chrome::Flow({ 50, 400 }, 0, 280, 0, 32, 6, 6);
    EXPECT_FLOAT_EQ(wide[1].left, 0);
    EXPECT_FLOAT_EQ(wide[1].right, 280);
}

TEST(EditorChrome, ToolBarRightGroupAndZoom)
{
    const auto bar = D2D1::RectF(0, 0, 1240, chrome::kToolBarHeight);
    const auto r = chrome::LayoutToolBarRight(bar, true, true);
    // From the right: the zoom field, a divider, redo, undo; all centred on the bar.
    EXPECT_FLOAT_EQ(r.zoom.right, 1240 - chrome::kBarPadX);
    EXPECT_FLOAT_EQ(r.zoom.right - r.zoom.left, chrome::kZoomFieldWidth);
    EXPECT_LT(r.divider.right, r.zoom.left);
    EXPECT_LT(r.redo.right, r.divider.left);
    EXPECT_LT(r.undo.right, r.redo.left);
    EXPECT_FLOAT_EQ(r.left, r.undo.left);
    for (const auto& rect : { r.zoom, r.redo, r.undo })
        EXPECT_FLOAT_EQ((rect.top + rect.bottom) / 2, chrome::kToolBarHeight / 2);
    // Without the zoom there's no divider either.
    const auto noZoom = chrome::LayoutToolBarRight(bar, true, false);
    EXPECT_EQ(noZoom.divider.right - noZoom.divider.left, 0.f);
    EXPECT_FLOAT_EQ(noZoom.redo.right, 1240 - chrome::kBarPadX);

    EXPECT_EQ(chrome::ZoomLabel(1.5, false), L"150%");
    EXPECT_EQ(chrome::ZoomLabel(0.333, true), L"Fit");
    const auto choices = chrome::ZoomChoices();
    ASSERT_FALSE(choices.empty());
    EXPECT_EQ(choices.front().label, L"Fit");
    EXPECT_EQ(choices.front().zoom, 0.0);
    for (size_t i = 2; i < choices.size(); ++i)
        EXPECT_GT(choices[i].zoom, choices[i - 1].zoom);
    for (const auto& c : choices)
        if (c.zoom > 0)
            EXPECT_EQ(chrome::ZoomLabel(c.zoom, false), c.label);
}
