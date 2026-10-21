#pragma once
#include "keyboard/WheelLayout.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// How each slice's label fits its tile, and where the centre's text goes when the drag picture is
// over it (pure math, kept free of Direct2D so it can be unit-tested; Overlay.cpp draws with it).
// Lengths are DIPs, times the wheel's scale (the monitor's DPI and the Size setting) when drawn.
//
// Every tile's label has one style (Brand Guidelines › Wheel type rules): Segoe UI Variable Text,
// kLabelDip Semibold, one colour, on both rings, with or without an icon. The icon is kIconDip on
// both rings, kIconGapDip above the label, the two centred in the tile as one block. A label goes on
// one line, else on two broken at a space (never between a number and its unit: "10 MB" stays
// together), the most even split first. It is never made smaller, shortened or cut off with "...":
// a label that does not fit two lines needs a shorter string in its recipe, which the unit tests
// catch for every wheel the app can show.
//
// Each check is against the tile's real shape: the text's box stays kLabelPadDip inside the tile's
// arcs and its straight sides (so also clear of the centre disc), wherever the tile is on the wheel.
namespace sd::wheel
{
    // Format names, sizes and amounts (PNG, Compress, Under 10 MB) are the label alone; everything
    // else has an icon above it.
    enum class LabelStyle { Name, Icon };

    inline constexpr float kLabelDip = 13.f;      // every label, Semibold
    inline constexpr float kIconDip = 20.f;       // every icon glyph, both rings
    inline constexpr float kIconGapDip = 3.f;     // between the icon and the label
    inline constexpr float kLineHeight = 1.05f;   // a line's room (and the spacing of two), times kLabelDip
    inline constexpr float kLabelPadDip = 1.5f;   // text to the tile's edges
    inline constexpr float kTileGapDip = 4.f;     // the straight gap between tiles

    struct LabelSpec
    {
        std::wstring text;
        LabelStyle style = LabelStyle::Icon;
    };

    // How wide `text` is drawn at kLabelDip Semibold, DIPs.
    using MeasureLabel = std::function<float(const std::wstring& text)>;

    struct FittedLabel
    {
        std::vector<std::wstring> lines;   // one or two
        std::vector<float> widths;         // each line's width, DIPs
        bool icon = false;                 // an icon sits above the lines
        float radius = 0.f;                // the block's centre, on the tile's middle line
        float iconDy = 0.f;                // the icon's centre below the block's centre
        std::vector<float> lineDy;         // each line's centre below the block's centre
        bool fits = true;                  // false: even two lines overflow the tile (the label is too long)

        std::wstring Text() const;         // the lines joined by a space
    };

    // One row of a stacked text block: its width and height. A row of width 0 is only a gap.
    struct Row
    {
        float w = 0.f, h = 0.f;
    };
    std::vector<Row> RowsOf(const FittedLabel& label);

    // Whether `rows`, stacked top to bottom, each centred on the tile's middle line, with the stack
    // centred `radius` from the wheel's centre, stay `pad` inside `slot`'s tile.
    bool RowsFit(const Layout& layout, int slot, const std::vector<Row>& rows, float radius, float pad = kLabelPadDip);

    // Every visible slot's label (labels[i] for layout.slots[i]), as the rules above say.
    std::vector<FittedLabel> FitLabels(const Layout& layout, const std::vector<LabelSpec>& labels, const MeasureLabel& measure);

    struct Rect
    {
        float l = 0.f, t = 0.f, r = 0.f, b = 0.f;
    };

    // Where `label` (fitted to `slot`) sits clearest of the drag picture `picture` (DIPs from the
    // wheel's centre): its own radius when the picture is clear of it, else the spot along the tile's
    // middle line, inside the tile, that the picture covers least (none at all if there is one).
    float ClearOfPicture(const Layout& layout, int slot, const FittedLabel& label, const Rect& picture);
    // How much of `label` at `radius` on `slot` the rectangle covers, DIP².
    float CoveredArea(const Layout& layout, int slot, const FittedLabel& label, float radius, const Rect& picture);

    // ---- The centre -----------------------------------------------------------------------------

    // Two styles only: the main line (the count, a new size, "Not for videos") kCentreMainDip
    // Semibold, on two lines when it is too wide for one; under it the detail kCentreDetailDip Regular.
    inline constexpr float kCentreMainDip = 20.f;
    inline constexpr float kCentreDetailDip = 12.f;

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
