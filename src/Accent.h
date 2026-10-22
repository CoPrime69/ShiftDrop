#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The accent colour: the hovered wheel slice, the top panel's active section, the Float shelf's
// highlights, and the editors' buttons, switches and selection. Settings › Accent colour picks one of
// the user's Windows accent colour (the default), the presets (Tide, ShiftDrop's teal, first), a colour of the user's own, or the Windows accent
// colour; it is saved in settings.ini with the other Settings choices and read each time a window
// asks theme::Current(), so a change shows on the next wheel without a restart.
//
// The model only: the Settings window shows it, theme::Current() and theme::EditorColors() turn it
// into the colours windows draw with. Every colour here is 0xRRGGBB.
namespace sd::accent
{
    // In the order Settings shows the swatches: the presets, then a colour of your own; Windows is the
    // "Use my Windows accent colour" switch.
    enum class Choice { Tide, Blue, Purple, Pink, Red, Orange, Green, Graphite, Custom, Windows };
    inline constexpr int kPresetCount = 8;   // Tide to Graphite
    const wchar_t* Name(Choice choice);       // "Tide", "Blue"...; "Custom colour", "Windows accent colour"

    inline constexpr uint32_t kNoColour = 0xFFFFFFFFu;   // not 0xRRGGBB: no custom colour picked yet

    // [Appearance] Accent=tide|blue|purple|pink|red|orange|green|graphite|custom|windows, the colours of
    // your own as AccentCustoms=#RRGGBB,#RRGGBB (as many as you add, in the order you added them), and
    // AccentCustom=#RRGGBB, the one of them Custom uses (the only one before there could be several:
    // a file with it alone reads as a list of one). No key, a value it doesn't know, or custom without
    // a colour, reads as Windows: the default is the accent the user already chose in Windows
    // (Prakhar 2026-10-21).
    struct Setting
    {
        Choice choice = Choice::Windows;
        uint32_t custom = kNoColour;        // the colour of your own in use while choice is Custom (one of `customs`)
        std::vector<uint32_t> customs;      // every colour of your own, a swatch each after the presets
        bool operator==(const Setting&) const = default;
    };
    // `rgb` added to the colours of your own (once: a colour already there is not added again) and chosen.
    Setting AddCustom(Setting setting, uint32_t rgb);
    // Colour `index` of `customs` chosen.
    Setting ChooseCustom(Setting setting, size_t index);
    // Colour `index` of `customs` deleted. When it was the one in use, the accent falls back to the
    // Windows accent colour when `windows`, else to the first preset (Tide). The presets can't be deleted.
    Setting DeleteCustom(Setting setting, size_t index, bool windows);
    Setting Load(const std::wstring& path);
    void Save(const Setting& setting, const std::wstring& path);
    std::wstring ToHex(uint32_t rgb);                       // "#12A58E"
    bool ParseHex(std::wstring_view text, uint32_t& rgb);   // "#12A58E" or "12a58e"

    // An accent in three tones, each readable with the text drawn on it and next to the surfaces it sits
    // on (at least kMinContrast : 1, WCAG's ratio for body text).
    struct Variants
    {
        uint32_t fill;    // the colour itself: large fills in light mode (the hovered slice), with TextOn(fill) on it
        uint32_t dark;    // on dark surfaces, as text and as fills with kTextOnDark on them
        uint32_t light;   // text and buttons on light surfaces, with kTextOnLight on them
        bool operator==(const Variants&) const = default;
    };
    inline constexpr uint32_t kTextOnDark = 0x15171E, kTextOnLight = 0xFFFFFF;
    // The lightest dark surface and the darkest light one that accent text sits on (theme::TokensFor):
    // the controls in dark mode and the windows' grey in light mode.
    inline constexpr uint32_t kDarkSurface = 0x2E3138, kLightSurface = 0xF3F4F6;
    inline constexpr double kMinContrast = 4.5;

    Variants Preset(Choice choice);    // Tide for Custom and Windows
    Variants Derive(uint32_t rgb);     // a colour of your own or Windows': moved lighter or darker until it reads
    Variants Resolve(const Setting& setting, uint32_t windowsRgb);
    bool Readable(const Variants& v);  // every tone passes with its text and next to its surface

    double Contrast(uint32_t a, uint32_t b);   // WCAG 2 contrast ratio, 1 to 21
    uint32_t TextOn(uint32_t fill);            // kTextOnDark or kTextOnLight, whichever reads better on it
    uint32_t Mix(uint32_t a, uint32_t b, double t);
    // `colour` up to `most` (0..1) of the way to white, as far as `text` on it still reads: a hover or a
    // highlight that stays legible.
    uint32_t Lighter(uint32_t colour, uint32_t text, double most);

    // The accent every window draws with: the setting Set() was given (the Windows accent colour read
    // now, for Windows). UI thread.
    void Set(const Setting& setting);
    Variants Current();
    uint32_t WindowsRgb();   // the user's Windows accent colour
}
