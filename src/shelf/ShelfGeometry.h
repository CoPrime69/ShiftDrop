#pragma once
#include <cstddef>
#include <string>
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

    // Where the user left the shelf, kept across restarts in a form that survives a change of
    // resolution, scaling or monitors: the monitor (its work area and device name when it was left)
    // and the panel's place on it as fractions. `x` runs from flush left (0) to flush right (1) over
    // the room the panel has to move in; `y` is its top edge as a fraction of the area's height, so
    // the top stays put when the shelf holds more items. A pinned panel keeps its edge.
    struct Place
    {
        Edge edge = Edge::Right;
        Box work;              // that monitor's work area, pixels, when it was left
        std::wstring device;   // that monitor's device name ("\\.\DISPLAY1"); may be empty
        float x = 1.f, y = 0.2f;   // the default: the right edge, a fifth of the way down
        bool operator==(const Place&) const = default;
    };
    // The place of `panel` (at rest, inside `work`) on the monitor with that work area and device.
    Place PlaceOf(const Box& panel, const Box& work, Edge edge, std::wstring device = {});
    // The panel (`width` by `height`, whole pixels) back at `place` on a monitor with work area
    // `work`: flush against its edge if pinned, always fully inside the area.
    Box PlaceIn(const Place& place, const Box& work, float width, float height);
    // A monitor as the shelf sees it.
    struct Screen
    {
        Box work;
        std::wstring device;
    };
    // Which of `screens` (at least one) a saved place goes back to: the monitor with the same work
    // area (the same device preferred), else the same device (its resolution or scaling changed),
    // else the work area nearest to where the panel was.
    size_t PickScreen(const std::vector<Screen>& screens, const Place& place);

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
        float label;         // a heading row in the list ("Pinned", "Recent conversions")
        float tabThickness, tabLength;
        float snap;          // how close to an edge counts as pinning to it
        size_t maxRows = 5;  // more items scroll
    };
    float PanelHeight(const Metrics& m, size_t count);
    float MaxScroll(const Metrics& m, size_t count);

    // What the list holds and which of the panel's parts show. The shelf's own list is items only,
    // with Clear all and the drag-out footer while it has any and the drop box while it has none. The
    // header's drop-down (Pinned and Recent conversions) is a list of headings and items, with neither.
    enum class RowKind { Item, Label };
    struct ListShape
    {
        std::vector<RowKind> rows;
        bool clear = false, footer = false, drop = false;
        float dropDown = 0;   // the header's drop-down button (the title and its chevron), pixels wide; 0 for none
    };
    ListShape ShelfShape(size_t count);   // the shelf's own list, no drop-down button
    float ContentHeight(const Metrics& m, const ListShape& shape);
    float PanelHeight(const Metrics& m, const ListShape& shape);
    float MaxScroll(const Metrics& m, const ListShape& shape);
    // Where row `index` starts in the list, and how tall it is (scrolled 0).
    float RowTop(const Metrics& m, const ListShape& shape, size_t index);
    float RowHeight(const Metrics& m, const ListShape& shape, size_t index);

    enum class Part { None, Header, Pin, Clear, Close, Item, Reveal, Remove, Footer, DropDown, Open, PinItem, Label };
    struct Hit
    {
        Part part = Part::None;
        size_t index = 0;   // the item, for Item / Reveal / Remove
        bool operator==(const Hit&) const = default;
    };

    // An item's buttons, from the left: pin, Open, Show in folder, × (shown while it is hovered or
    // focused). A heading row has only `row` and `name`.
    struct RowBox
    {
        size_t index;
        RowKind kind = RowKind::Item;
        Box row, thumb, name, detail, pin, open, reveal, remove;
    };
    // The buttons of a row in the order the keyboard (Left and Right) goes through them.
    inline constexpr Part kRowButtons[] = { Part::PinItem, Part::Open, Part::Reveal, Part::Remove };
    const Box& ButtonBox(const RowBox& row, Part part);
    struct Layout
    {
        Box panel, header, title, dropDown, pin, clear, close, list, footer;
        Box drop;                   // an empty shelf's drop target, over the list and the footer
        std::vector<RowBox> rows;   // rows at least partly inside the list, top to bottom
    };
    // Lays out a panel showing `count` items scrolled down by `scroll` pixels (clamped). The header's
    // buttons from the right: close, "Clear all" (clear), pin. An empty shelf has no Clear all and no
    // footer (both empty boxes, never hit; the pin moves next to close): the space below the header is
    // one drop target instead.
    Layout LayOut(const Box& panel, const Metrics& m, size_t count, float scroll);
    // The same for any list (`shape`); `index` of a row is its place in shape.rows.
    Layout LayOut(const Box& panel, const Metrics& m, const ListShape& shape, float scroll);
    // What is under a point. Rows only count inside the list (a scrolled-away row is not hit).
    Hit HitTest(const Layout& layout, float x, float y);
}
