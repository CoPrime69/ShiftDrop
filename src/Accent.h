#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// The accent colour: the hovered wheel slice, the top panel's active section, the Float shelf's
// highlights, and the editors' buttons, switches and selection. Settings › Accent colour picks one of
// the presets (Tide, ShiftDrop's teal, by default), a colour of the user's own, or the Windows accent
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

    // [Appearance] Accent=tide|blue|purple|pink|red|orange|green|graphite|custom|windows and, once a
    // colour of your own was picked, AccentCustom=#RRGGBB (kept while a preset is chosen, so Custom
    // brings it back). A value it doesn't know, or custom without a colour, reads as Tide.
    struct Setting
    {
        Choice choice = Choice::Tide;
        uint32_t custom = kNoColour;
        bool operator==(const Setting&) const = default;
    };
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
    // The lightest dark surface and the darkest light one that accent text sits on: the wheel's tiles in
    // dark mode and the editors' window in light mode.
    inline constexpr uint32_t kDarkSurface = 0x3B3B3F, kLightSurface = 0xF3F3F5;
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
