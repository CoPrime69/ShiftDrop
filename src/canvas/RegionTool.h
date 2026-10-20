#pragma once
#include "canvas/CanvasTool.h"
#include "canvas/Regions.h"

namespace sd::canvas
{
    // The pointer and keyboard behaviour every rectangle tool shares, on top of a RegionSet:
    //   drag a grip to resize (Shift keeps the shape, Alt resizes about the centre), drag a body to
    //   move (every selected region moves together), drag on empty picture to draw a new region,
    //   click to select, Ctrl+click to add to the selection, Ctrl+A to select all, Tab / Shift+Tab to
    //   step through regions, arrows to nudge by a pixel (Shift: ten), Delete to remove.
    // Crop uses it with a single region that can't be deleted; Redact with many.
    class RegionTool : public ICanvasTool
    {
    public:
        CursorShape CursorAt(const Pointer& p) const override;
        Response PointerDown(const Pointer& p) override;
        Response PointerMove(const Pointer& p) override;
        Response PointerUp(const Pointer& p) override;
        Response PointerCancel() override;
        Response Hover(const Pointer& p) override;
        Response Key(unsigned vk, bool shift, bool ctrl) override;

        const RegionSet& Regions() const { return m_regions; }

    protected:
        // Called after any change to the regions (a drag step, a nudge, a delete...).
        virtual void RegionsChanged() {}

        static nlohmann::json SaveRegions(const RegionSet& regions);
        static void LoadRegions(RegionSet& regions, const nlohmann::json& saved);

        RegionSet m_regions;
        bool m_single = false;      // one region only: drawing a new one replaces it (Crop)
        bool m_canDelete = true;
        bool m_canCreate = true;
        int m_newStyle = 0;         // style given to regions drawn by hand
        Hit m_hot;                  // under the pointer, for grip highlighting

    private:
        enum class Press { None, Pending, Dragging };
        Press m_press = Press::None;
        PointD m_downAt;
        double m_downTolerance = 0;
        std::vector<Region> m_replaced;   // m_single: the region a new drawing replaces, until it is kept
    };
}
