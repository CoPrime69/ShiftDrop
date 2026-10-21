#pragma once
#include <cstddef>
#include <string_view>
#include <vector>

// The Float shelf's geometry, kept free of Win32 and Direct2D so it can be unit-tested: where the
// panel goes when it is dropped near a screen edge, the slim tab it collapses to, and where each row
// and button is (for drawing and for hit-testing the mouse). Everything is in screen pixels.
namespace sd::shelf
{
    struct Box
    {
        float left = 0, top = 0, right = 0, bottom = 0;

        float Width() const { return right - left; }
        float Height() const { return bottom - top; }
        bool Contains(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
        Box Offset(float dx, float dy) const { return Box{ left + dx, top + dy, right + dx, bottom + dy }; }
        Box Inflated(float d) const { return Box{ left - d, top - d, right + d, bottom + d }; }
        bool operator==(const Box&) const = default;
    };
    Box Lerp(const Box& a, const Box& b, float t);
    Box Union(const Box& a, const Box& b);
    Box Intersect(const Box& a, const Box& b);

    // The screen edge a shelf is pinned to. None = floating where it was put.
    enum class Edge { None, Left, Top, Right, Bottom };
    const char* EdgeName(Edge edge);            // "none", "left", ... (the saved form)
    Edge EdgeFromName(std::string_view name);   // unknown names are None

    // The edge of the work area a panel dropped at `panel` pins to: the nearest edge whose distance
    // to the panel's side is at most `threshold` (a side past the edge counts as touching). None if
    // no edge is that close.
    Edge SnapEdge(const Box& panel, const Box& work, float threshold);
    // `panel` moved (not resized) so it lies inside `work`; a panel larger than the area keeps its
    // top-left corner in it.
    Box ClampInto(const Box& panel, const Box& work);
    // The panel flush against `edge` of `work`, keeping its place along that edge, clamped into the
    // area. Edge::None only clamps.
    Box Dock(const Box& panel, const Box& work, Edge edge);
    // The slim tab a pinned panel collapses to: on the same edge, `thickness` deep and `length` long,
    // centred on the panel's span along the edge and kept inside `work`. For Edge::None the panel.
    Box Tab(const Box& panel, const Box& work, Edge edge, float thickness, float length);
    // The way out of the screen from `edge` (unit vector); zero for None. Show and hide slide along it.
    void Outward(Edge edge, float& dx, float& dy);

    // Sizes for a DPI scale (pixels per DIP).
    struct Metrics
    {
        explicit Metrics(float scale);
        float scale;
        float width;         // panel width
        float header;        // title row with the pin, "Clear all" and close buttons
        float row;           // one item
        float footer;        // the "drag all" handle
        float inset;         // panel edge to content
        float thumb;         // thumbnail square
        float button;        // round icon buttons
        float clearAll;      // the "Clear all" button's width (as high as a round one)
        float radius;        // panel corners
        float tabThickness, tabLength;
        float snap;          // how close to an edge counts as pinning to it
        size_t maxRows = 5;  // more items scroll
    };
    float PanelHeight(const Metrics& m, size_t count);
    float MaxScroll(const Metrics& m, size_t count);

    enum class Part { None, Header, Pin, Clear, Close, Item, Reveal, Remove, Footer };
    struct Hit
    {
        Part part = Part::None;
        size_t index = 0;   // the item, for Item / Reveal / Remove
        bool operator==(const Hit&) const = default;
    };

    struct RowBox
    {
        size_t index;
        Box row, thumb, name, detail, reveal, remove;
    };
    struct Layout
    {
        Box panel, header, title, pin, clear, close, list, footer;
        Box drop;                   // an empty shelf's drop target, over the list and the footer
        std::vector<RowBox> rows;   // rows at least partly inside the list, top to bottom
    };
    // Lays out a panel showing `count` items scrolled down by `scroll` pixels (clamped). The header's
    // buttons from the right: close, "Clear all" (clear), pin. An empty shelf has no Clear all and no
    // footer (both empty boxes, never hit; the pin moves next to close): the space below the header is
    // one drop target instead.
    Layout LayOut(const Box& panel, const Metrics& m, size_t count, float scroll);
    // What is under a point. Rows only count inside the list (a scrolled-away row is not hit).
    Hit HitTest(const Layout& layout, float x, float y);
}
