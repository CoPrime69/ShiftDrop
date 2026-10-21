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
    };
    const wchar_t* MarkName(Mark kind);

    struct Annotation
    {
        uint32_t id = 0;
        Mark kind = Mark::Arrow;
        // Arrow: tail a, head b. Rect, Ellipse, Callout, Image: corners a and b. Text and Number: a is
        // the top-left corner / the centre. Magnifier: a is the spot looked at, b the loupe's centre.
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
    };

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
        Size,      // a text's or a label's size, a loupe's radius
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

        // The text's measured size (drawn by Direct2D, which reports it back); an estimate until then.
        void SetMeasured(uint32_t id, SizeD size) { m_measured[id] = size; }
        SizeD TextSize(const Annotation& a) const;

        // The box a mark occupies (for selection outlines and hit-testing).
        RectD Bounds(const Annotation& a) const;
        std::vector<Grip> Grips(const Annotation& a) const;
        // The topmost mark under `at` (within `tolerance` pixels of a line), or 0.
        uint32_t HitTest(PointD at, double tolerance) const;
        Part GripAt(const Annotation& a, PointD at, double tolerance) const;

        // Dragging a part of a mark by (dx, dy) from how it was (`before`).
        void Drag(Annotation& a, const Annotation& before, Part part, PointD from, PointD to) const;

        nlohmann::json ToJson() const;   // {"items":[...]}
        static Annotations FromJson(const nlohmann::json& j);

    private:
        std::vector<Annotation> m_items;
        std::map<uint32_t, SizeD> m_measured;
        uint32_t m_next = 1;
    };

    double DistanceToSegment(PointD p, PointD a, PointD b);
    // Default stroke width and font size for a picture of this size, so marks are visible on a phone
    // photo and on a screenshot alike.
    double DefaultStroke(double width, double height);
    double DefaultFont(double width, double height);
    // White or near-black text, whichever reads better on `rgb`.
    uint32_t ContrastingText(uint32_t rgb);
}
