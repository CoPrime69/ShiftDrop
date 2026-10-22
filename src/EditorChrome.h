#pragma once
#include <d2d1.h>

#include <string>
#include <vector>

// Where the parts of an editor's window go (App Editors page): the tool bar along the top (the tools'
// tabs on the left; undo, redo and the zoom on the right), the picture or the timeline in the middle,
// the options panel on the right, and the footer along the bottom (what will be saved on the left,
// Cancel and Apply on the right). Sizes of the kit's controls that the layouts need. Pure: no window,
// so the unit tests check it. In DIPs.
namespace sd::chrome
{
    inline constexpr float kToolBarHeight = 62.f;   // tabs: 6 + icon 16 + 3 + label 16 + 3 + mark 3 + 4, and 6 around
    inline constexpr float kFooterHeight = 56.f;    // 32 DIP buttons with 12 above and below
    inline constexpr float kPanelWidth = 320.f;
    inline constexpr float kPanelPadX = 20.f, kPanelPadY = 18.f;
    inline constexpr float kPanelTitle = 16.f;      // the panel's title, semibold
    inline constexpr float kPanelTitleHeight = 22.f;
    inline constexpr float kPanelGap = 14.f;        // between the panel's rows
    inline constexpr float kSectionLabel = 12.f;    // "Ratio", "Size": semibold, secondary
    inline constexpr float kBarPadX = 14.f;         // the tool bar's sides
    inline constexpr float kFooterPadX = 18.f;
    inline constexpr float kControlHeight = 32.f;   // buttons, fields, chips, drop-downs
    inline constexpr float kToolBarButton = 34.f;   // undo and redo
    inline constexpr float kZoomFieldWidth = 90.f;
    inline constexpr float kTabMinWidth = 56.f, kTabPadX = 12.f, kTabHeight = 50.f;
    // A segmented choice's options (the kit's DrawSegment): text with 16 either side (ControlKit's
    // SegmentWidth), down to 8 where the panel is narrow.
    inline constexpr float kSegmentPadX = 16.f, kSegmentMinPadX = 8.f, kSegmentHeight = 32.f;

    struct FrameRects
    {
        D2D1_RECT_F toolBar{}, stage{}, panel{}, footer{};
    };
    // The window's client area `width` x `height` cut into its parts; `panel` false gives the stage the
    // whole width (an editor without options, such as the "Checking..." step).
    FrameRects LayoutFrame(float width, float height, bool panel = true);

    // A tool tab (icon over name) around a label `labelWidth` wide.
    float TabWidth(float labelWidth);

    // Items of `widths` left to right from (left, top), `gap` apart, onto a new row `rowGap` lower when
    // one would pass `right` (an item wider than the row gets a row of its own, clipped to it).
    std::vector<D2D1_RECT_F> Flow(const std::vector<float>& widths, float left, float right, float top, float height, float gap, float rowGap);
    // Options of a segmented choice side by side from (left, top), touching (each draws its part of the
    // track: ControlKit::DrawSegment), one rectangle for each option of text `textWidths` wide. Their
    // padding shrinks to fit `maxWidth`; empty when even the least padding doesn't fit (the caller
    // shows chips).
    std::vector<D2D1_RECT_F> Segments(const std::vector<float>& textWidths, float left, float top, float maxWidth);

    // The tool bar's right-hand group, from the right edge in: the zoom field (when `zoom`), a divider,
    // redo and undo (when `undo`). Returns where the group starts, so the left side knows its room.
    struct ToolBarRight
    {
        D2D1_RECT_F zoom{}, divider{}, redo{}, undo{};
        float left = 0;
    };
    ToolBarRight LayoutToolBarRight(const D2D1_RECT_F& toolBar, bool undo, bool zoom);

    // The zoom field's list and its text: "Fit" when the picture is fit to the window, else "150%".
    struct ZoomChoice
    {
        std::wstring label;
        double zoom = 0;   // 0: fit
    };
    std::vector<ZoomChoice> ZoomChoices();
    std::wstring ZoomLabel(double zoom, bool fit);
}
