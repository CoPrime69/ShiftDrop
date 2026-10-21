#pragma once
#include "canvas/Geometry.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Annotate Photos (tool 4): the marks a picture gets, in picture pixels, with hit-testing, grips and
// editing. Plain data and maths (Compose.cpp draws the marks), so the editing is unit-tested.
namespace sd::photo
{
    using canvas::PointD;
    using canvas::RectD;
    using canvas::SizeD;

    enum class Mark
    {
        Arrow,
        Text,
        Rect,
        Ellipse,
        Callout,      // a speech bubble: a box with text and a tail pointing at something
        Number,       // a numbered circle; numbers follow the order the labels were placed in
        Magnifier,    // a loupe: a circle showing part of the picture enlarged
        Image,        // a picture placed on top
        Pen,
        Highlighter,
        Check,        // a tick for a form's box
        Cross,        // a cross for a form's box
    };
    const wchar_t* MarkName(Mark kind);

    // A highlighter's strokes are this see-through, on screen and in a PDF.
    inline constexpr float kHighlighterOpacity = 0.42f;

    // How a text mark's lines line up.
    enum class Align
    {
        Left,
        Centre,
        Right,
    };

    struct Annotation
    {
        uint32_t id = 0;
        Mark kind = Mark::Arrow;
        // Arrow: tail a, head b. Rect, Ellipse, Callout, Image, Check, Cross: corners a and b. Text: a is
        // the top-left corner; with `wrap`, b is the box's bottom-right corner (its height grows to fit
        // the text). Number: a is the centre. Magnifier: a is the spot looked at, b the loupe's centre.
        PointD a, b;
        PointD tip;                    // Callout: where its tail points
        double radius = 40;            // Magnifier: the spot's radius (the loupe is radius * zoom)
        double zoom = 2.5;             // Magnifier
        std::vector<PointD> points;    // Pen, Highlighter
        std::wstring text;             // Text, Callout
        std::wstring image;            // Image: the file placed
        uint32_t color = 0xE81123;     // 0xRRGGBB
        double width = 6;              // stroke width, pixels
        double fontSize = 48;          // pixels
        bool fill = false;             // Rect, Ellipse: filled; Text: on a coloured backdrop
        // Text: the type. An empty font is the Segoe UI that marks had before fonts could be picked.
        std::wstring font;
        int weight = 600;              // 400 regular, 700 bold
        bool italic = false, underline = false;
        Align align = Align::Left;
        bool wrap = false;             // Auto-wrap: lines break at the box's width; otherwise the box fits the text
        bool signature = false;        // Image: a signature (kept for reuse; named so in the panel)
    };

    // The family a text mark is drawn in.
    const wchar_t* FontOf(const Annotation& a);
    // The narrowest a wrapping text box gets.
    double MinTextWidth(const Annotation& a);

    // Which part of a mark the pointer holds.
    enum class Part
    {
        None,
        Body,      // move it all
        A,         // corner / end / spot
        B,         // the opposite corner / end / the loupe
        AB,        // the corner at (a.x, b.y)
        BA,        // the corner at (b.x, a.y)
        Tip,       // a callout's tail
        Size,      // a label's size, a loupe's radius
        Left,      // a box's sides: a text box, a picture
        Right,
        Top,
        Bottom,
    };

    struct Grip
    {
        Part part = Part::None;
        PointD at;
    };

    // Everything Annotate keeps for one picture.
    class Annotations
    {
    public:
        std::vector<Annotation>& Items() { return m_items; }
        const std::vector<Annotation>& Items() const { return m_items; }
        Annotation* Find(uint32_t id);
        const Annotation* Find(uint32_t id) const;
        uint32_t Add(Annotation a);          // gives it an id; returns it
        bool Remove(uint32_t id);
        void BringToFront(uint32_t id);

        // The number a Number label shows: its place among the labels, from 1.
        int NumberOf(uint32_t id) const;

        // The text's measured size (laid out by DirectWrite, which reports it back); an estimate until
        // then. A wrapping text box is measured at its width.
        void SetMeasured(uint32_t id, SizeD size) { m_measured[id] = size; }
        SizeD TextSize(const Annotation& a) const;
        // The box a text mark's lines sit in (without a backdrop's margin).
        RectD TextBox(const Annotation& a) const;

        // The box a mark occupies (for selection outlines and hit-testing).
        RectD Bounds(const Annotation& a) const;
        std::vector<Grip> Grips(const Annotation& a) const;
        // The topmost mark under `at` (within `tolerance` pixels of a line), or 0.
        uint32_t HitTest(PointD at, double tolerance) const;
        Part GripAt(const Annotation& a, PointD at, double tolerance) const;

        // Dragging a part of a mark by (dx, dy) from how it was (`before`). Pictures, ticks and crosses
        // keep their shape unless `free` (Shift). A text box's sides set its width (turning Auto-wrap
        // on); its corners resize the box when it wraps and scale the type when it doesn't.
        void Drag(Annotation& a, const Annotation& before, Part part, PointD from, PointD to, bool free = false) const;

        nlohmann::json ToJson() const;   // {"items":[...]}
        static Annotations FromJson(const nlohmann::json& j);

    private:
        std::vector<Annotation> m_items;
        std::map<uint32_t, SizeD> m_measured;
        uint32_t m_next = 1;
    };

    double DistanceToSegment(PointD p, PointD a, PointD b);

    // The shapes the screen and the PDF writer both draw, so a mark looks the same in each.
    struct ArrowShape
    {
        PointD lineEnd;      // the shaft runs from the tail (a) to here
        PointD head[3];      // the filled tip: point, then the two back corners
        double headStroke = 1;
    };
    ArrowShape ArrowGeometry(const Annotation& a);
    // A callout's tail: a triangle from inside its box to the tip.
    void CalloutTail(const Annotation& a, PointD out[3]);
    double CalloutRadius(const Annotation& a);
    // A tick or a cross in its box: polylines (one for a tick, two for a cross) and their width.
    std::vector<std::vector<PointD>> CheckStrokes(const Annotation& a, double& width);
    // A pen stroke smoothed through the midpoints: from points[0], each segment a quadratic Bézier
    // (control, end); a single point becomes a dot.
    struct QuadSegment
    {
        PointD control, end;
    };
    std::vector<QuadSegment> SmoothStroke(const std::vector<PointD>& points);
    // Default stroke width and font size for a picture of this size, so marks are visible on a phone
    // photo and on a screenshot alike.
    double DefaultStroke(double width, double height);
    double DefaultFont(double width, double height);
    // White or near-black text, whichever reads better on `rgb`.
    uint32_t ContrastingText(uint32_t rgb);
}
