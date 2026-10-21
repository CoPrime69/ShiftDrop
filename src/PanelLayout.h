#pragma once
#include <vector>

// The top panel's sizes and the switcher's layout, kept free of Win32 and Direct2D so they can be
// unit-tested (Island.cpp draws with them). Lengths named ...Dip are DIPs; everything else is pixels.
namespace sd::panel
{
    // The panel is drawn this much larger than the wheel at the same Size setting: at the wheel's own
    // scale its names and hint were too small to read at a glance, above all on 1440p screens.
    inline constexpr float kScale = 1.4f;
    // Pixels per DIP for the panel, from the wheel's (theme::ScaleFor: DPI times the Size setting).
    inline float Scale(float wheelScale) { return wheelScale * kScale; }

    inline constexpr float kMaxWidthDip = 720.f;    // the widest the panel gets (the window is this wide)
    inline constexpr float kScreenRoomDip = 8.f;    // never closer than this to the sides of the screen
    inline constexpr float kInsetDip = 8.f;         // panel edge to content
    inline constexpr float kSwitcherHeightDip = 44.f;
    inline constexpr float kTabFontDip = 13.f;      // wheel names
    inline constexpr float kHintFontDip = 11.5f;    // "Scroll or Shift+1-4"

    // One switcher tab: padding, icon, gap, name, padding. Then a gap, the hint and its end. The
    // gaps can shrink to the ...Min sizes when the screen is too narrow.
    inline constexpr float kTabPadDip = 12.f, kTabPadMinDip = 7.f;
    inline constexpr float kIconDip = 18.f;
    inline constexpr float kIconGapDip = 7.f, kIconGapMinDip = 5.f;
    inline constexpr float kHintGapDip = 14.f, kHintGapMinDip = 8.f;
    inline constexpr float kHintEndDip = 10.f, kHintEndMinDip = 4.f;

    // The widest the panel's content may be at `scale` on a monitor whose work area is `workWidth`
    // pixels wide: kMaxWidthDip, or less on a narrow screen.
    float MaxContentWidth(float scale, float workWidth);

    struct Switcher
    {
        float pad = 0.f;           // each side of a tab
        float iconGap = 0.f;       // icon to name
        float hintGap = 0.f;       // last tab to the hint
        float hintEnd = 0.f;       // after the hint
        bool hint = false;         // the hint is shown
        float nameScale = 1.f;     // below 1 only when even the bare tabs do not fit: each name ends in "..."
        std::vector<float> tabs;   // each tab's width
        float width = 0.f;         // tabs and hint together (without the panel's inset)
    };
    // The switcher's tabs at `scale`, given each wheel name's width and the hint's width as drawn
    // (0 for no hint). It fits into `available` pixels by giving things up in this order: the gaps
    // shrink, then the hint goes, then the names are shortened. The tabs are the last to give way
    // (their icons and smallest gaps always stay, so `width` can exceed a room narrower than those).
    Switcher FitSwitcher(const std::vector<float>& names, float hint, float scale, float available);
}
