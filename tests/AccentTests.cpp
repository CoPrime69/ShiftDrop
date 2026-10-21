#include "Accent.h"
#include "Settings.h"
#include "TestUtil.h"

#include <format>

using namespace sd;
using namespace sd::test;
using accent::Choice;

namespace
{
    std::string Hex(uint32_t rgb) { return std::format("#{:06X}", rgb); }

    // The surfaces of each theme that accent text and accent marks sit on (theme::TokensFor and
    // theme::MakePalette): the wheel's plate, tiles and centre, the top panel, the result card and the
    // shelf, and the windows' grey, cards, fields and controls.
    constexpr uint32_t kDarkSurfaces[] = { 0x1A1C22, 0x2A2D35, 0x141619, 0x1F2127, 0x1B1D22, 0x24272D, 0x2A2D33, 0x2E3138 };
    constexpr uint32_t kLightSurfaces[] = { 0xF3F4F6, 0xF6F7F9, 0xFFFFFF };

    // What the app draws with in each theme (theme::Current and theme::EditorColors): in dark mode the
    // dark tone with dark text on it; in light mode the light tone with white text (buttons, accent
    // text) and the colour itself with the text that reads on it (the hovered slice).
    void ExpectReadable(const accent::Variants& v, const std::string& what)
    {
        EXPECT_GE(accent::Contrast(v.dark, accent::kTextOnDark), accent::kMinContrast) << what << " dark " << Hex(v.dark);
        for (const uint32_t surface : kDarkSurfaces)
            EXPECT_GE(accent::Contrast(v.dark, surface), accent::kMinContrast) << what << " dark " << Hex(v.dark) << " on " << Hex(surface);
        EXPECT_GE(accent::Contrast(v.light, accent::kTextOnLight), accent::kMinContrast) << what << " light " << Hex(v.light);
        for (const uint32_t surface : kLightSurfaces)
            EXPECT_GE(accent::Contrast(v.light, surface), accent::kMinContrast) << what << " light " << Hex(v.light) << " on " << Hex(surface);
        EXPECT_GE(accent::Contrast(v.fill, accent::TextOn(v.fill)), accent::kMinContrast) << what << " fill " << Hex(v.fill);
        // The hover and the highlight toward the slice's rim are lighter, and still read.
        EXPECT_GE(accent::Contrast(accent::Lighter(v.dark, accent::kTextOnDark, 0.34), accent::kTextOnDark), accent::kMinContrast) << what;
        EXPECT_GE(accent::Contrast(accent::Lighter(v.light, accent::kTextOnLight, 0.12), accent::kTextOnLight), accent::kMinContrast) << what;
        EXPECT_GE(accent::Contrast(accent::Lighter(v.fill, accent::TextOn(v.fill), 0.20), accent::TextOn(v.fill)), accent::kMinContrast) << what;
        EXPECT_TRUE(accent::Readable(v)) << what;
    }
}

// ---- The colours --------------------------------------------------------------------------------------

TEST(Accent, WindowsIsTheDefaultAndTideIsFirst)
{
    // The default is the accent the user already chose in Windows.
    const accent::Setting none;
    EXPECT_EQ(none.choice, Choice::Windows);
    EXPECT_EQ(accent::Resolve(none, 0x0067C0), accent::Derive(0x0067C0));
    EXPECT_EQ(Preferences{}.accent, none);
    // Tide, ShiftDrop's teal, is the first preset.
    EXPECT_EQ(static_cast<int>(Choice::Tide), 0);
    const auto tide = accent::Preset(Choice::Tide);
    EXPECT_EQ(tide.fill, 0x12A58Eu);
    EXPECT_EQ(tide.dark, 0x2DD4B4u);
    EXPECT_EQ(tide.light, 0x0B7A69u);
    EXPECT_EQ(accent::kTextOnDark, 0x15171Eu);
    EXPECT_EQ(accent::TextOn(tide.fill), accent::kTextOnDark);   // dark text on the teal
    EXPECT_EQ(accent::TextOn(tide.light), accent::kTextOnLight);
}

TEST(Accent, EveryPresetReadsInLightAndDark)
{
    for (int i = 0; i < accent::kPresetCount; ++i)
    {
        const auto choice = static_cast<Choice>(i);
        ExpectReadable(accent::Preset(choice), Narrow(accent::Name(choice)));
        EXPECT_EQ(accent::Resolve({ choice }, 0), accent::Preset(choice));
    }
    // Eight different colours, in the order Settings shows them.
    EXPECT_EQ(std::wstring(accent::Name(Choice::Tide)), L"Tide");
    EXPECT_EQ(std::wstring(accent::Name(Choice::Blue)), L"Blue");
    EXPECT_EQ(std::wstring(accent::Name(Choice::Graphite)), L"Graphite");
    for (int i = 0; i < accent::kPresetCount; ++i)
        for (int j = i + 1; j < accent::kPresetCount; ++j)
            EXPECT_NE(accent::Preset(static_cast<Choice>(i)).fill, accent::Preset(static_cast<Choice>(j)).fill);
}

TEST(Accent, AColourOfYourOwnIsMadeReadable)
{
    // Every colour on a 0x33 grid (black, white, greys, pure and muddy hues) gets three readable tones.
    for (uint32_t r = 0; r <= 0xFF; r += 0x33)
        for (uint32_t g = 0; g <= 0xFF; g += 0x33)
            for (uint32_t b = 0; b <= 0xFF; b += 0x33)
            {
                const uint32_t rgb = (r << 16) | (g << 8) | b;
                ExpectReadable(accent::Derive(rgb), Hex(rgb));
            }
    // A tone that already reads is kept as picked.
    EXPECT_EQ(accent::Derive(0x12A58E).fill, 0x12A58Eu);
    EXPECT_EQ(accent::Derive(0x2DD4B4).dark, 0x2DD4B4u);
    EXPECT_EQ(accent::Derive(0x0B7A69).light, 0x0B7A69u);
    // Too dark for dark mode: lighter there, the same hue; too light for light mode: darker.
    const auto navy = accent::Derive(0x102A6B);
    EXPECT_EQ(navy.light, 0x102A6Bu);
    EXPECT_GT(navy.dark & 0xFF, 0x6Bu);
    const auto yellow = accent::Derive(0xFFE14D);
    EXPECT_EQ(yellow.dark, 0xFFE14Du);
    EXPECT_LT((yellow.light >> 16) & 0xFF, 0xFFu);
    // Custom without a colour (a hand edit) is Tide.
    EXPECT_EQ(accent::Resolve({ Choice::Custom, accent::kNoColour }, 0), accent::Preset(Choice::Tide));
    EXPECT_EQ(accent::Resolve({ Choice::Custom, 0xE91E63 }, 0), accent::Derive(0xE91E63));
}

TEST(Accent, WindowsChoiceUsesTheWindowsColour)
{
    // Windows' default blue, and an accent too light to take white text, both come out readable.
    EXPECT_EQ(accent::Resolve({ Choice::Windows }, 0x0067C0), accent::Derive(0x0067C0));
    ExpectReadable(accent::Resolve({ Choice::Windows }, 0x0067C0), "Windows blue");
    ExpectReadable(accent::Resolve({ Choice::Windows }, 0xFFB900), "Windows gold");
    // Whatever is in the registry here, it is a colour.
    EXPECT_EQ(accent::WindowsRgb() & 0xFF000000u, 0u);
}

TEST(Accent, CurrentFollowsTheSetting)
{
    accent::Set({ Choice::Purple });
    EXPECT_EQ(accent::Current(), accent::Preset(Choice::Purple));
    accent::Set({ Choice::Custom, 0x336699 });
    EXPECT_EQ(accent::Current(), accent::Derive(0x336699));
    accent::Set({ Choice::Windows });
    EXPECT_EQ(accent::Current(), accent::Derive(accent::WindowsRgb()));
    accent::Set({});
    EXPECT_EQ(accent::Current(), accent::Derive(accent::WindowsRgb()));
    accent::Set({ Choice::Tide });
    EXPECT_EQ(accent::Current(), accent::Preset(Choice::Tide));
}

TEST(Accent, ContrastAndMixing)
{
    EXPECT_NEAR(accent::Contrast(0x000000, 0xFFFFFF), 21.0, 1e-9);
    EXPECT_NEAR(accent::Contrast(0x777777, 0x777777), 1.0, 1e-9);
    EXPECT_DOUBLE_EQ(accent::Contrast(0x12A58E, 0x15171E), accent::Contrast(0x15171E, 0x12A58E));
    EXPECT_EQ(accent::Mix(0x000000, 0xFFFFFF, 0.5), 0x808080u);
    EXPECT_EQ(accent::Mix(0x123456, 0xABCDEF, 0.0), 0x123456u);
    EXPECT_EQ(accent::Mix(0x123456, 0xABCDEF, 1.0), 0xABCDEFu);
    // Lighter goes the whole way with dark text, and stops where white text would stop reading.
    EXPECT_EQ(accent::Lighter(0x2DD4B4, accent::kTextOnDark, 0.28), accent::Mix(0x2DD4B4, 0xFFFFFF, 0.28));
    const uint32_t hover = accent::Lighter(0x0B7A69, accent::kTextOnLight, 0.5);
    EXPECT_NE(hover, accent::Mix(0x0B7A69, 0xFFFFFF, 0.5));
    EXPECT_GE(accent::Contrast(hover, 0xFFFFFF), accent::kMinContrast);
}

// ---- settings.ini -------------------------------------------------------------------------------------

TEST(Accent, HexColours)
{
    uint32_t rgb = 0;
    EXPECT_TRUE(accent::ParseHex(L"#12A58E", rgb));
    EXPECT_EQ(rgb, 0x12A58Eu);
    EXPECT_TRUE(accent::ParseHex(L"2dd4b4", rgb));
    EXPECT_EQ(rgb, 0x2DD4B4u);
    for (const wchar_t* bad : { L"", L"#", L"#12A58", L"#12A58E0", L"#12G58E", L"teal", L"# 12A58" })
        EXPECT_FALSE(accent::ParseHex(bad, rgb)) << Narrow(bad);
    EXPECT_EQ(accent::ToHex(0x0B7A69), L"#0B7A69");
    EXPECT_EQ(accent::ToHex(0), L"#000000");
}

TEST(Accent, SavedWithTheOtherPreferences)
{
    TempDir dir;
    const auto ini = dir / L"settings.ini";
    // No file, or no key: the Windows accent, and no colour of your own.
    EXPECT_EQ(LoadPreferences(dir / L"missing.ini").accent, accent::Setting{});
    WriteText(ini, "[Appearance]\r\nTheme=dark\r\nPanelSize=160\r\n");
    EXPECT_EQ(LoadPreferences(ini).accent, accent::Setting{});

    // Every choice goes back and forth, next to the other choices.
    for (int i = 0; i <= static_cast<int>(Choice::Windows); ++i)
    {
        auto p = LoadPreferences(ini);
        p.accent = { static_cast<Choice>(i), 0x336699 };
        SavePreferences(p, ini);
        const auto back = LoadPreferences(ini);
        EXPECT_EQ(back.accent, p.accent) << i;
        EXPECT_EQ(back.theme, ThemeChoice::Dark);
        EXPECT_EQ(back.panelSize, 160);
    }
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"Appearance", L"Accent", L"", value, ARRAYSIZE(value), ini.c_str());
    EXPECT_EQ(std::wstring(value), L"windows");
    GetPrivateProfileStringW(L"Appearance", L"AccentCustom", L"", value, ARRAYSIZE(value), ini.c_str());
    EXPECT_EQ(std::wstring(value), L"#336699");

    // The colour of your own stays while a preset is chosen, so Custom brings it back.
    auto p = LoadPreferences(ini);
    p.accent = { Choice::Pink, 0xE91E63 };
    SavePreferences(p, ini);
    EXPECT_EQ(LoadPreferences(ini).accent, (accent::Setting{ Choice::Pink, 0xE91E63 }));
    // Without one, there is no key.
    p.accent = { Choice::Green };
    SavePreferences(p, ini);
    EXPECT_EQ(GetPrivateProfileStringW(L"Appearance", L"AccentCustom", L"", value, ARRAYSIZE(value), ini.c_str()), 0u);
    EXPECT_EQ(LoadPreferences(ini).accent, accent::Setting{ Choice::Green });
    // Names are read whatever their case.
    WritePrivateProfileStringW(L"Appearance", L"Accent", L"GRAPHITE", ini.c_str());
    EXPECT_EQ(LoadPreferences(ini).accent.choice, Choice::Graphite);
}

TEST(Accent, BadValuesFallBackToWindows)
{
    TempDir dir;
    const auto ini = dir / L"settings.ini";
    const auto load = [&](const char* text) {
        WriteText(ini, std::string("[Appearance]\r\n") + text);
        return LoadPreferences(ini).accent;
    };
    EXPECT_EQ(load("Accent=teal\r\n"), accent::Setting{});
    EXPECT_EQ(load("Accent=\r\n"), accent::Setting{});
    EXPECT_EQ(load("Accent=3\r\n"), accent::Setting{});
    // Custom needs a colour.
    EXPECT_EQ(load("Accent=custom\r\n"), accent::Setting{});
    EXPECT_EQ(load("Accent=custom\r\nAccentCustom=#12G58E\r\n"), accent::Setting{});
    EXPECT_EQ(load("Accent=custom\r\nAccentCustom=purple\r\n"), accent::Setting{});
    EXPECT_EQ(load("Accent=custom\r\nAccentCustom=#7C4DFF\r\n"), (accent::Setting{ Choice::Custom, 0x7C4DFF }));
    // A bad colour of your own leaves a preset as it is.
    EXPECT_EQ(load("Accent=blue\r\nAccentCustom=nope\r\n"), accent::Setting{ Choice::Blue });
    // The other choices read on as before.
    EXPECT_EQ(load("Accent=teal\r\nTheme=light\r\n").choice, Choice::Windows);
    EXPECT_EQ(LoadPreferences(ini).theme, ThemeChoice::Light);
}
