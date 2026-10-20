#include "canvas/Geometry.h"
#include "canvas/RegionTool.h"
#include "canvas/Regions.h"

#include <gtest/gtest.h>
#include <windows.h>

using namespace sd::canvas;

namespace
{
    // The smallest real tool on the shared region logic, to test what every editor inherits.
    class BoxesTool final : public RegionTool
    {
    public:
        BoxesTool() { m_regions.SetBounds({ 0, 0, 1000, 800 }); }
        std::wstring Title() const override { return L"Boxes"; }
        std::wstring ApplyLabel() const override { return L"Apply"; }
        void ImageReady(size_t, int, int) override {}
        void Draw(IPainter&) const override {}
        void BuildPanel(std::vector<Control>&) const override {}
        Response OnControl(const ControlEvent&) override { return {}; }
        nlohmann::json State() const override { return SaveRegions(m_regions); }
        void Restore(const nlohmann::json& s) override { LoadRegions(m_regions, s); }
        bool CanApply(std::wstring*) const override { return true; }
        nlohmann::json Params() const override { return {}; }
        RegionSet& Set() { return m_regions; }
        void Single() { m_single = true; }
    };

    Pointer At(double x, double y, bool shift = false, bool ctrl = false)
    {
        Pointer p;
        p.at = { x, y };
        p.tolerance = 6;
        p.shift = shift;
        p.ctrl = ctrl;
        return p;
    }
}

// ---- View: fit, zoom and pan ------------------------------------------------------------------------

TEST(Canvas, FitCentresTheImageAndNeverEnlargesIt)
{
    ViewTransform v;
    v.SetImage({ 4000, 2000 });
    v.SetViewport({ 0, 0, 1000, 800 });
    v.Fit(0);
    EXPECT_DOUBLE_EQ(v.Zoom(), 0.25);
    EXPECT_DOUBLE_EQ(v.ToView(PointD{ 0, 0 }).x, 0);
    EXPECT_DOUBLE_EQ(v.ToView(PointD{ 0, 0 }).y, 150);   // (800 - 500) / 2
    EXPECT_TRUE(v.IsFit());

    ViewTransform small;
    small.SetImage({ 200, 100 });
    small.SetViewport({ 0, 0, 1000, 800 });
    small.Fit(0);
    EXPECT_DOUBLE_EQ(small.Zoom(), 1.0);   // a small picture is shown at 100%, not blown up
}

TEST(Canvas, ZoomKeepsThePointUnderTheCursor)
{
    ViewTransform v;
    v.SetImage({ 4000, 2000 });
    v.SetViewport({ 0, 0, 1000, 800 });
    v.Fit(0);
    const PointD cursor{ 700, 300 };
    const PointD before = v.ToImage(cursor);
    v.ZoomAt(1.0, cursor);
    const PointD after = v.ToImage(cursor);
    EXPECT_NEAR(after.x, before.x, 1e-9);
    EXPECT_NEAR(after.y, before.y, 1e-9);
    EXPECT_FALSE(v.IsFit());
    EXPECT_DOUBLE_EQ(v.Zoom(), 1.0);
}

TEST(Canvas, WheelStepsSnapToFitAndActualSize)
{
    ViewTransform v;
    v.SetImage({ 4000, 2000 });
    v.SetViewport({ 0, 0, 1000, 800 });
    v.Fit(0);
    const PointD c{ 500, 400 };
    // Up from 25%: 2^(1/4) per notch, so eight notches would pass 100% and stop on it.
    v.Step(8, c);
    EXPECT_DOUBLE_EQ(v.Zoom(), 1.0);
    v.Step(1, c);
    EXPECT_GT(v.Zoom(), 1.0);
    // Back down stops at 100% first, then at the fit zoom (and is "fit" again, so resizing refits).
    v.Step(-40, c);
    EXPECT_DOUBLE_EQ(v.Zoom(), 1.0);
    v.Step(-40, c);
    EXPECT_DOUBLE_EQ(v.Zoom(), 0.25);
    EXPECT_TRUE(v.IsFit());
    v.ZoomAt(1000, c);
    EXPECT_DOUBLE_EQ(v.Zoom(), ViewTransform::kMaxZoom);
}

TEST(Canvas, PanningKeepsPartOfThePictureInView)
{
    ViewTransform v;
    v.SetImage({ 4000, 2000 });
    v.SetViewport({ 0, 0, 1000, 800 });
    v.ZoomAt(1.0, { 0, 0 });
    v.PanBy(-100000, -100000);
    // At least a quarter of the view stays covered by the picture.
    const PointD bottomRight = v.ToView(PointD{ 4000, 2000 });
    EXPECT_GE(bottomRight.x, 250 - 1e-9);
    EXPECT_GE(bottomRight.y, 200 - 1e-9);
    v.PanBy(100000, 100000);
    const PointD topLeft = v.ToView(PointD{ 0, 0 });
    EXPECT_LE(topLeft.x, 750 + 1e-9);
    EXPECT_LE(topLeft.y, 600 + 1e-9);
}

// ---- Regions: hit-testing, dragging, resizing --------------------------------------------------------

TEST(Canvas, HitTestFindsGripsOfSelectedRegionsAndBodies)
{
    RegionSet s;
    s.SetBounds({ 0, 0, 1000, 800 });
    s.Add({ 100, 100, 200, 100 });
    s.Add({ 250, 150, 200, 200 });   // overlaps the first, on top

    EXPECT_EQ(s.HitTest({ 280, 160 }, 6).index, 1);   // the topmost body wins
    EXPECT_EQ(s.HitTest({ 120, 120 }, 6).index, 0);
    EXPECT_FALSE(s.HitTest({ 900, 700 }, 6));
    // Grips only on a selected region.
    EXPECT_EQ(s.HitTest({ 100, 100 }, 6).grip, Grip::Body);
    s.Select(0, false);
    EXPECT_EQ(s.HitTest({ 102, 98 }, 6).grip, Grip::NW);
    EXPECT_EQ(s.HitTest({ 300, 200 }, 6).grip, Grip::SE);
    EXPECT_EQ(s.HitTest({ 200, 104 }, 6).grip, Grip::N);
    EXPECT_EQ(s.HitTest({ 297, 150 }, 6).grip, Grip::E);
    EXPECT_EQ(s.HitTest({ 200, 150 }, 6).grip, Grip::Body);
}

TEST(Canvas, SmallRegionsCanStillBeMovedFromTheMiddle)
{
    RegionSet s;
    s.SetBounds({ 0, 0, 1000, 800 });
    s.Add({ 100, 100, 12, 12 });
    s.Select(0, false);
    EXPECT_EQ(s.HitTest({ 106, 106 }, 10).grip, Grip::Body);
    EXPECT_EQ(s.HitTest({ 100, 100 }, 10).grip, Grip::NW);
}

TEST(Canvas, DraggingMovesEverySelectedRegionInsideTheBounds)
{
    RegionSet s;
    s.SetBounds({ 0, 0, 1000, 800 });
    s.Add({ 100, 100, 100, 100 });
    s.Add({ 400, 100, 100, 100 });
    s.SelectAll();
    s.BeginDrag(s.HitTest({ 150, 150 }, 6), { 150, 150 });
    s.DragTo({ 5000, 170 }, false);
    ASSERT_TRUE(s.EndDrag());
    EXPECT_DOUBLE_EQ(s.Items()[1].rect.Right(), 1000);   // stopped at the edge, together
    EXPECT_DOUBLE_EQ(s.Items()[0].rect.x, 600);
    EXPECT_DOUBLE_EQ(s.Items()[0].rect.y, 120);
    EXPECT_DOUBLE_EQ(s.Items()[1].rect.y, 120);
}

TEST(Canvas, ResizeFlipsPastTheAnchorAndKeepsTheMinimum)
{
    RegionSet s;
    s.SetBounds({ 0, 0, 1000, 800 });
    s.SetMinSize(8);
    const RectD o{ 100, 100, 200, 100 };
    // Dragging the east edge past the west edge flips the box.
    EXPECT_EQ(s.Resized(o, Grip::E, { 60, 999 }, 0, false), (RectD{ 60, 100, 40, 100 }));
    // Never smaller than the minimum.
    EXPECT_EQ(s.Resized(o, Grip::SE, { 101, 101 }, 0, false), (RectD{ 100, 100, 8, 8 }));
    // Never outside.
    EXPECT_EQ(s.Resized(o, Grip::SE, { 2000, 2000 }, 0, false), (RectD{ 100, 100, 900, 700 }));
    // From the centre (Alt): both sides move.
    EXPECT_EQ(s.Resized(o, Grip::E, { 320, 0 }, 0, true), (RectD{ 80, 100, 240, 100 }));
}

TEST(Canvas, AspectLockHoldsTheShape)
{
    RegionSet s;
    s.SetBounds({ 0, 0, 1000, 800 });
    const RectD o{ 100, 100, 200, 100 };
    // A corner with the ratio fixed at 2:1 follows the side pulled further.
    const RectD r = s.Resized(o, Grip::SE, { 500, 150 }, 2.0, false);
    EXPECT_DOUBLE_EQ(r.w / r.h, 2.0);
    EXPECT_DOUBLE_EQ(r.w, 400);
    // Clamped by the bounds, still in shape.
    const RectD big = s.Resized(o, Grip::SE, { 5000, 5000 }, 2.0, false);
    EXPECT_DOUBLE_EQ(big.w / big.h, 2.0);
    EXPECT_LE(big.Right(), 1000);
    EXPECT_LE(big.Bottom(), 800);
    // An edge with a fixed ratio grows the other side around its centre line.
    const RectD edge = s.Resized(o, Grip::E, { 500, 0 }, 2.0, false);
    EXPECT_DOUBLE_EQ(edge.w, 400);
    EXPECT_DOUBLE_EQ(edge.h, 200);
    EXPECT_DOUBLE_EQ(edge.Centre().y, 150);
}

TEST(Canvas, ShiftDrawsSquaresAndAClickDrawsNothing)
{
    BoxesTool t;
    t.PointerDown(At(100, 100));
    t.PointerMove(At(300, 180, true));
    t.PointerUp(At(300, 180, true));
    ASSERT_EQ(t.Regions().Size(), 1u);
    EXPECT_DOUBLE_EQ(t.Regions().Items()[0].rect.w, t.Regions().Items()[0].rect.h);
    EXPECT_DOUBLE_EQ(t.Regions().Items()[0].rect.w, 200);

    // A click (or a tiny slip) on empty picture leaves no region behind.
    t.PointerDown(At(600, 600));
    t.PointerMove(At(601, 601));
    t.PointerUp(At(601, 601));
    t.PointerDown(At(600, 600));
    t.PointerMove(At(603, 602));   // past the click tolerance, but under the 4 px minimum
    t.PointerUp(At(603, 602));
    EXPECT_EQ(t.Regions().Size(), 1u);
}

TEST(Canvas, MultiSelectNudgeAndDelete)
{
    BoxesTool t;
    t.Set().Add({ 10, 10, 50, 50 });
    t.Set().Add({ 100, 10, 50, 50 });
    t.Set().Add({ 200, 10, 50, 50 });
    t.PointerDown(At(30, 30));
    t.PointerUp(At(30, 30));
    t.PointerDown(At(120, 30, false, true));   // Ctrl+click adds
    t.PointerUp(At(120, 30, false, true));
    EXPECT_EQ(t.Regions().SelectedCount(), 2);

    EXPECT_TRUE(t.Key(VK_RIGHT, true, false).changed);   // Shift+arrow: 10 px
    EXPECT_DOUBLE_EQ(t.Regions().Items()[0].rect.x, 20);
    EXPECT_DOUBLE_EQ(t.Regions().Items()[1].rect.x, 110);
    EXPECT_DOUBLE_EQ(t.Regions().Items()[2].rect.x, 200);
    t.Key(VK_UP, true, false);   // already at the top edge after 10 px: stops there
    EXPECT_DOUBLE_EQ(t.Regions().Items()[0].rect.y, 0);
    EXPECT_FALSE(t.Key(VK_UP, false, false).changed);

    EXPECT_TRUE(t.Key(VK_DELETE, false, false).changed);
    ASSERT_EQ(t.Regions().Size(), 1u);
    EXPECT_DOUBLE_EQ(t.Regions().Items()[0].rect.x, 200);
    t.Key('A', false, true);
    EXPECT_EQ(t.Regions().SelectedCount(), 1);
}

TEST(Canvas, StateRoundTripsForUndo)
{
    BoxesTool t;
    t.Set().Add({ 10, 20, 30, 40 }, 2, "face", L"Face");
    const auto saved = t.State();
    t.Set().Clear();
    t.Restore(saved);
    ASSERT_EQ(t.Regions().Size(), 1u);
    const auto& r = t.Regions().Items()[0];
    EXPECT_EQ(r.rect, (RectD{ 10, 20, 30, 40 }));
    EXPECT_EQ(r.style, 2);
    EXPECT_EQ(r.origin, "face");
    EXPECT_EQ(r.label, L"Face");
}

TEST(Canvas, PixelRectsRoundAndClip)
{
    EXPECT_EQ(ToPixels({ 10.4, 10.6, 20.2, 5 }, 100, 100), (RectI{ 10, 11, 21, 5 }));
    EXPECT_EQ(ToPixels({ -10, -10, 50, 50 }, 30, 30), (RectI{ 0, 0, 30, 30 }));
    EXPECT_EQ(ToPixels({ 5.2, 5.2, 0.1, 0.1 }, 30, 30), (RectI{ 5, 5, 1, 1 }));   // a sliver is still a pixel
}
