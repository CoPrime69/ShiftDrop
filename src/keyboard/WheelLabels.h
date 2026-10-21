#pragma once
#include "keyboard/WheelLayout.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// How each slice's label fits its tile, and where the centre's text goes when the drag picture is
// over it (pure math, kept free of Direct2D so it can be unit-tested; Overlay.cpp draws with it).
// Lengths are DIPs. Text is never cut off with "...":
//
//   1. the label on one line at its full size, else on two lines broken at a space (never between a
//      number and its unit: "10 MB" stays together), most even split first;
//   2. both again, half a DIP smaller each time, down to the style's readable minimum. Wrapping comes
//      before shrinking so a ring's labels keep one size wherever a second line has room;
//   3. only then is the label shortened: "Under 10 MB" becomes "≤10 MB", words it shares with the
//      other labels go, then words from its end. A shortening that would read like another slice's
//      label is never used, so slices that differ still look different;
//   4. if none of that fits, the whole label is drawn smaller than the minimum, as small as it needs.
//
// Each check is against the tile's real shape: the text's box stays kLabelPadDip inside the tile's
// arcs and its straight sides (so also clear of the centre disc), wherever the tile is on the wheel.
namespace sd::wheel
{
    // Format names (PNG, Compress, Under 10 MB) are bold text alone; everything else has an icon
    // above a smaller label.
    enum class LabelStyle { Name, Icon };

    inline constexpr float kNameLabelDip = 18.f, kNameLabelMinDip = 10.f;
    inline constexpr float kIconLabelDip = 13.f, kIconLabelMinDip = 9.5f;
    inline constexpr float kLabelFloorDip = 6.f;   // step 4 stops here
    inline constexpr float kLabelStepDip = 0.5f;
    // The icon glyph at a label's full size; it shrinks with its label, its room with it.
    inline constexpr float kIconFontDip = 22.f;
    inline constexpr float kIconGapDip = 3.f;      // between the icon and the label
    inline constexpr float kLineHeight = 1.1f;     // a line's room (and the spacing of two), times the font size
    inline constexpr float kLabelPadDip = 3.f;     // text to the tile's edges
    inline constexpr float kTileGapDip = 4.f;      // the straight gap between tiles

    float FullSize(LabelStyle style);
    float MinSize(LabelStyle style);

    struct LabelSpec
    {
        std::wstring text;
        LabelStyle style = LabelStyle::Icon;
    };

    // How wide `text` is drawn in `style` at FullSize(style), DIPs. Widths at other sizes are taken
    // in proportion (DirectWrite's natural metrics scale with the font size).
    using MeasureLabel = std::function<float(const std::wstring& text, LabelStyle style)>;

    struct FittedLabel
    {
        std::vector<std::wstring> lines;   // one or two
        float size = 0.f;                  // font size, DIPs
        float radius = 0.f;                // the block's centre, on the tile's middle line
        float iconSize = 0.f;              // the icon glyph's size, DIPs (Icon style)
        float iconDy = 0.f;                // the icon's centre below the block's centre
        std::vector<float> lineDy;         // each line's centre below the block's centre
        float lineHeight = 0.f;
        bool shortened = false;            // step 3: the lines are not the whole label
        bool fits = true;                  // false only when even kLabelFloorDip does not fit

        std::wstring Text() const;         // the lines joined by a space
    };

    // One row of a stacked text block: its width and height. A row of width 0 is only a gap.
    struct Row
    {
        float w = 0.f, h = 0.f;
    };

    // Whether `rows`, stacked top to bottom, each centred on the tile's middle line, with the stack
    // centred `radius` from the wheel's centre, stay `pad` inside `slot`'s tile.
    bool RowsFit(const Layout& layout, int slot, const std::vector<Row>& rows, float radius, float pad = kLabelPadDip);

    // Every visible slot's label (labels[i] for layout.slots[i]), as the steps above say.
    std::vector<FittedLabel> FitLabels(const Layout& layout, const std::vector<LabelSpec>& labels, const MeasureLabel& measure);

    // ---- The centre -----------------------------------------------------------------------------

    struct Rect
    {
        float l = 0.f, t = 0.f, r = 0.f, b = 0.f;
    };

    // The font size for one line of text `fullWidth` wide at `full` that has `room`: `full` when it
    // fits, smaller in proportion, never below `min`.
    float ShrinkToFit(float fullWidth, float full, float min, float room);

    struct CentrePlan
    {
        bool badge = false;     // the picture covers the centre's text: the count goes in a badge beside it
        float x = 0.f, y = 0.f; // the badge's centre, from the wheel's centre
    };

    // The centre's text normally sits in the middle of the centre disc (`text`, from the wheel's
    // centre). When the drag picture (`picture`, the same way; none in click mode) covers any of it,
    // the count moves into a `badgeW` x `badgeH` badge just above or below the picture, on the side
    // with more of the disc left, so the two never overlap. `wasBadge` keeps a badge until the
    // picture is clearly off the text, so it does not flicker at the edge.
    CentrePlan PlanCentre(const Rect& text, const std::optional<Rect>& picture, float badgeW, float badgeH, bool wasBadge);
}
