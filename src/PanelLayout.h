#pragma once
#include <vector>

// The top panel's sizes and the switcher's layout, and the easing every panel window animates with,
// kept free of Win32 and Direct2D so they can be unit-tested (Island.cpp draws with them). Lengths
// named ...Dip are DIPs; everything else is pixels.
namespace sd::panel
{
    // The panel is drawn this much larger than the wheel at the same Size setting: at the wheel's own
    // scale its names and hint were too small to read at a glance, above all on 1440p screens.
    inline constexpr float kScale = 1.4f;

    // Settings › Top panel size: a share of that size, 100% (as above) to 300% in 10% steps, saved
    // as [Appearance] PanelSize in settings.ini. It is on top of the tray's Size setting.
    inline constexpr int kSizeMinPercent = 100, kSizeMaxPercent = 300, kSizeStepPercent = 10;
    // `percent` kept to that range and rounded to a step (a hand-edited 155 reads as 160).
    int SizePercent(int percent);

    // Pixels per DIP for the panel, from the wheel's (theme::ScaleFor: DPI times the Size setting)
    // and the Top panel size.
    inline float Scale(float wheelScale, int sizePercent = kSizeMinPercent)
    {
        return wheelScale * kScale * static_cast<float>(SizePercent(sizePercent)) / 100.f;
    }

    inline constexpr float kMaxWidthDip = 720.f;    // the widest the panel gets (the window is this wide)
    inline constexpr float kScreenRoomDip = 8.f;    // never closer than this to the sides of the screen
    inline constexpr float kInsetDip = 8.f;         // panel edge to content
    inline constexpr float kTopGapDip = 10.f;       // work-area top edge to the panel
    inline constexpr float kPadDip = 24.f;          // transparent room around the panel for its shadow
    inline constexpr float kMaxHeightDip = 72.f;    // the tallest the panel gets (a result with two lines)
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

    // `scale` (Scale with the Top panel size), made smaller where it would not fit a monitor whose
    // work area is `workWidth` x `workHeight` pixels, but never smaller than `base` (Scale at 100%,
    // the size before the slider). It fits when the switcher's tabs, with their full names (each
    // name's width in DIPs in `namesDip`) and the smallest gaps, go across the screen, and the
    // window with its shadow room reaches no lower than half way down it.
    float FitScale(float scale, float base, const std::vector<float>& namesDip, float workWidth, float workHeight);

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

    // One frame of exponential easing, as the wheel, the top panel and the Float shelf animate: over
    // `dtMs`, `value` closes the gap to `target` as with time constant `tauMs`, and lands on it once
    // it is within `snap`.
    void Approach(float& value, float target, float dtMs, float tauMs, float snap);
}
