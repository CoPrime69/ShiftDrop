#pragma once
#include "canvas/Geometry.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sd::canvas
{
    // Which part of a region the pointer is on: its body (move) or one of eight grips (resize).
    enum class Grip { None, Body, N, NE, E, SE, S, SW, W, NW };

    struct Region
    {
        uint32_t id = 0;
        RectD rect;
        bool selected = false;
        int style = 0;            // tool-defined (a cover's fill, an annotation's kind...)
        std::string origin;       // "" = drawn by hand; otherwise what proposed it ("text", "face", "email"...)
        std::wstring label;       // shown on the region while it is a proposal ("Face", "Email")
    };

    struct Hit
    {
        int index = -1;
        Grip grip = Grip::None;
        explicit operator bool() const { return index >= 0; }
    };

    // The overlay model shared by every region tool (crop box, redaction covers, later annotations):
    // rectangles in image coordinates with a selection, hit-testing with grips, moving and resizing
    // with an optional fixed aspect ratio, and clamping to the picture. No drawing and no Win32, so
    // it is unit-tested directly.
    class RegionSet
    {
    public:
        void SetBounds(const RectD& bounds);
        const RectD& Bounds() const { return m_bounds; }
        void SetMinSize(double pixels) { m_min = pixels; }
        double MinSize() const { return m_min; }
        // A width/height ratio every resize keeps (a crop preset); 0 = free.
        void SetAspect(double aspect) { m_aspect = aspect; }
        double Aspect() const { return m_aspect; }

        std::vector<Region>& Items() { return m_items; }
        const std::vector<Region>& Items() const { return m_items; }
        size_t Size() const { return m_items.size(); }
        Region* Find(uint32_t id);

        // Adds a region (clamped into the bounds) and returns its index. It is not selected.
        int Add(const RectD& rect, int style = 0, std::string origin = {}, std::wstring label = {});
        void Clear() { m_items.clear(); m_drag = {}; }

        // The grip or body under `at`. Grips of selected regions win (within `tolerance` image pixels
        // of a corner or edge), then the topmost region whose body contains the point.
        Hit HitTest(PointD at, double tolerance) const;

        void Select(int index, bool toggle);   // toggle (Ctrl+click) adds or removes; otherwise selects only it
        void SelectAll();
        void ClearSelection();
        int SelectedCount() const;
        int FirstSelected() const;
        int DeleteSelected();   // returns how many were removed
        // Moves the selection by (dx, dy), as a group, never past the bounds. Returns whether anything moved.
        bool Nudge(double dx, double dy);

        // Dragging: BeginDrag on a hit (moving a body moves every selected region), DragTo as the
        // pointer moves, EndDrag on release. `lockAspect` (Shift) keeps the shape the region had when
        // the drag began; `fromCentre` (Alt) resizes about the centre.
        void BeginDrag(const Hit& hit, PointD at);
        // Starts drawing a new region at `at`; the drag moves its far corner. Returns its index.
        int BeginCreate(PointD at, int style = 0);
        void DragTo(PointD at, bool lockAspect, bool fromCentre = false);
        // Ends the drag. A region being drawn that is still smaller than the minimum size (a click
        // without a drag) is removed; returns false then.
        bool EndDrag();
        void CancelDrag();
        bool Dragging() const { return m_drag.active; }
        bool Creating() const { return m_drag.active && m_drag.creating; }

        // Resizes one rectangle the way a grip drag does (exposed for the tools' size fields and tests).
        RectD Resized(const RectD& original, Grip grip, PointD at, double aspect, bool fromCentre) const;

    private:
        RectD Normalised(RectD r) const;

        struct Drag
        {
            bool active = false;
            bool creating = false;
            int index = -1;
            Grip grip = Grip::None;
            PointD start;
            std::vector<std::pair<int, RectD>> originals;   // every region the drag moves, as it began
        };

        std::vector<Region> m_items;
        RectD m_bounds{ 0, 0, 1, 1 };
        double m_min = 4;
        double m_aspect = 0;
        uint32_t m_nextId = 1;
        Drag m_drag;
    };
}
