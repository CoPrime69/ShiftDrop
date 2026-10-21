#include "PathUtil.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "canvas/Raster.h"
#include "crop/CropMath.h"
#include "crop/CropTool.h"
#include "metadata/Metadata.h"

#include <cmath>

using namespace sd;
using namespace sd::crop;
using namespace sd::test;
using canvas::Raster;

namespace
{
    // A picture whose every pixel says where it came from: B = x, G = y, R = 7, opaque.
    Raster Coordinates(UINT w, UINT h)
    {
        Raster r;
        r.Allocate(w, h);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                uint8_t* p = r.At(x, y);
                p[0] = static_cast<uint8_t>(x);
                p[1] = static_cast<uint8_t>(y);
                p[2] = 7;
                p[3] = 255;
            }
        return r;
    }

    CropParams Box(double x, double y, double w, double h, double refW, double refH)
    {
        CropParams p;
        p.box = { x, y, w, h };
        p.refW = refW;
        p.refH = refH;
        return p;
    }
}

TEST(Crop, AspectPresets)
{
    const std::vector<std::string> ids = { "free", "original", "1:1", "4:5", "3:2", "16:9", "9:16", "custom" };
    ASSERT_EQ(AspectPresets().size(), ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
        EXPECT_EQ(AspectPresets()[i].id, ids[i]);
    EXPECT_DOUBLE_EQ(AspectRatio("free", 1, 1, 4000, 3000), 0);
    EXPECT_DOUBLE_EQ(AspectRatio("original", 1, 1, 4000, 3000), 4.0 / 3.0);
    EXPECT_DOUBLE_EQ(AspectRatio("1:1", 1, 1, 4000, 3000), 1.0);
    EXPECT_DOUBLE_EQ(AspectRatio("4:5", 1, 1, 4000, 3000), 0.8);
    EXPECT_DOUBLE_EQ(AspectRatio("16:9", 1, 1, 4000, 3000), 16.0 / 9.0);
    EXPECT_DOUBLE_EQ(AspectRatio("9:16", 1, 1, 4000, 3000), 9.0 / 16.0);
    EXPECT_DOUBLE_EQ(AspectRatio("custom", 21, 9, 4000, 3000), 21.0 / 9.0);
    EXPECT_DOUBLE_EQ(AspectRatio("custom", 0, 9, 4000, 3000), 0);
    EXPECT_EQ(FindAspect("5:4"), nullptr);
}

TEST(Crop, LargestCentredBox)
{
    const auto r = LargestCentred({ 0, 0, 4000, 3000 }, 1.0);
    EXPECT_EQ(r, (canvas::RectD{ 500, 0, 3000, 3000 }));
    const auto wide = LargestCentred({ 0, 0, 4000, 3000 }, 16.0 / 9.0);
    EXPECT_DOUBLE_EQ(wide.w, 4000);
    EXPECT_DOUBLE_EQ(wide.h, 2250);
    EXPECT_DOUBLE_EQ(wide.y, 375);
    EXPECT_EQ(LargestCentred({ 0, 0, 40, 30 }, 0), (canvas::RectD{ 0, 0, 40, 30 }));
}

TEST(Crop, StraightenKeepsTheCornersFilled)
{
    EXPECT_EQ(StraightenBounds(400, 300, 0), (canvas::RectD{ 0, 0, 400, 300 }));
    for (const double deg : { 1.0, -5.0, 12.5, 45.0 })
    {
        const auto b = StraightenBounds(400, 300, deg);
        EXPECT_NEAR(b.w / b.h, 400.0 / 300.0, 1e-9);           // the picture's own shape
        EXPECT_NEAR(b.Centre().x, 200, 1e-9);
        EXPECT_NEAR(b.Centre().y, 150, 1e-9);
        // Every corner of the box, turned back by the angle, lands inside the original picture.
        const double t = deg * 3.14159265358979323846 / 180, c = std::cos(t), s = std::sin(t);
        for (const auto& [x, y] : { std::pair{ b.x, b.y }, { b.Right(), b.y }, { b.x, b.Bottom() }, { b.Right(), b.Bottom() } })
        {
            const double dx = x - 200, dy = y - 150;
            const double qx = c * dx + s * dy + 200, qy = -s * dx + c * dy + 150;
            EXPECT_GE(qx, -1e-6);
            EXPECT_LE(qx, 400 + 1e-6);
            EXPECT_GE(qy, -1e-6);
            EXPECT_LE(qy, 300 + 1e-6);
        }
    }
}

TEST(Crop, FirstPictureGetsTheBoxAsDrawn)
{
    const auto plan = PlanCrop(Box(100, 50, 200, 120, 400, 300), 400, 300);
    EXPECT_EQ(plan.box, (canvas::RectI{ 100, 50, 200, 120 }));
    // A box partly outside is pulled in, never cut short.
    const auto out = PlanCrop(Box(300, 250, 200, 120, 400, 300), 400, 300);
    EXPECT_EQ(out.box, (canvas::RectI{ 200, 180, 200, 120 }));
}

TEST(Crop, BatchIsCentredAndScaledWithEachPicture)
{
    // Drawn on a 400x300 picture: a 200x120 box in the top-left corner.
    const auto params = Box(0, 0, 200, 120, 400, 300);
    // An 800x600 picture: the same shape at twice the size, centred.
    const auto big = PlanCrop(params, 800, 600);
    EXPECT_EQ(big.box, (canvas::RectI{ 200, 180, 400, 240 }));
    // A portrait 300x400 one: scaled by the tighter side (0.75), centred.
    const auto tall = PlanCrop(params, 300, 400);
    EXPECT_EQ(tall.box, (canvas::RectI{ 75, 155, 150, 90 }));
    // "Same place": the box's centre at the same relative spot.
    auto same = params;
    same.samePlace = true;
    EXPECT_EQ(PlanCrop(same, 800, 600).box, (canvas::RectI{ 0, 0, 400, 240 }));
}

TEST(Crop, BatchWithATypedSizeKeepsThePixelSize)
{
    auto params = Box(100, 100, 100, 100, 400, 300);
    params.exactSize = true;
    EXPECT_EQ(PlanCrop(params, 800, 600).box, (canvas::RectI{ 350, 250, 100, 100 }));
    // Too big for a small picture: shrunk in shape to fit, never cropped past the edge.
    EXPECT_EQ(PlanCrop(params, 80, 50).box, (canvas::RectI{ 15, 0, 50, 50 }));
}

TEST(Crop, QuarterTurnsAreExactPixelCopies)
{
    const Raster src = Coordinates(4, 3);
    CropPlan plan;
    plan.quarterTurns = 1;   // clockwise: the bottom-left pixel goes to the top-left
    plan.rotatedW = 3;
    plan.rotatedH = 4;
    plan.box = { 0, 0, 3, 4 };
    Raster out;
    ASSERT_TRUE(RenderCrop(src, plan, out));
    ASSERT_EQ(out.width, 3u);
    ASSERT_EQ(out.height, 4u);
    EXPECT_EQ(out.At(0, 0)[0], 0);   // x of the source
    EXPECT_EQ(out.At(0, 0)[1], 2);   // y: the bottom row
    EXPECT_EQ(out.At(2, 0)[1], 0);   // the top-right pixel came from the top-left
    EXPECT_EQ(out.At(2, 3)[0], 3);
    EXPECT_EQ(out.At(2, 3)[1], 0);

    plan.quarterTurns = 2;
    plan.rotatedW = 4;
    plan.rotatedH = 3;
    plan.box = { 1, 1, 2, 1 };
    ASSERT_TRUE(RenderCrop(src, plan, out));
    EXPECT_EQ(out.At(0, 0)[0], 2);
    EXPECT_EQ(out.At(0, 0)[1], 1);
    EXPECT_EQ(out.At(1, 0)[0], 1);
}

TEST(Crop, StraightenResamplesWithoutEmptyCorners)
{
    const Raster src = Coordinates(200, 100);
    CropParams p;
    p.straighten = 10;
    const auto plan = PlanCrop(p, 200, 100);   // no box: all of the straightened area
    Raster out;
    ASSERT_TRUE(RenderCrop(src, plan, out));
    EXPECT_LT(out.width, 200u);
    for (UINT y = 0; y < out.height; ++y)
        for (UINT x = 0; x < out.width; ++x)
        {
            ASSERT_EQ(out.At(x, y)[3], 255);   // nothing transparent: every output pixel came from the picture
            ASSERT_EQ(out.At(x, y)[2], 7);
        }
}

TEST(Crop, ToolKeepsTheBoxInShapeAndInside)
{
    CropTool tool(3);
    tool.ImageReady(0, 4000, 3000);
    EXPECT_EQ(tool.Box(), (canvas::RectD{ 0, 0, 4000, 3000 }));   // free: all of it
    tool.OnControl({ "aspect", 2 });   // 1:1
    EXPECT_EQ(tool.Box(), (canvas::RectD{ 500, 0, 3000, 3000 }));
    canvas::ControlEvent width{ "width" };
    width.value = 1080;
    tool.OnControl(width);
    EXPECT_DOUBLE_EQ(tool.Box().w, 1080);
    EXPECT_DOUBLE_EQ(tool.Box().h, 1080);
    EXPECT_TRUE(tool.Current().exactSize);

    tool.OnControl({ "rotateRight" });
    auto params = ParseParams(tool.Params());
    EXPECT_EQ(params.quarterTurns, 1);
    EXPECT_DOUBLE_EQ(params.refW, 3000);   // turned: the picture is now portrait
    EXPECT_DOUBLE_EQ(params.refH, 4000);
    EXPECT_DOUBLE_EQ(params.box.w, params.box.h);
    EXPECT_LE(params.box.Right(), 3000);

    canvas::ControlEvent straighten{ "straighten" };
    straighten.value = 7.25;
    tool.OnControl(straighten);
    params = ParseParams(tool.Params());
    EXPECT_DOUBLE_EQ(params.straighten, 7.3);   // one decimal
    const auto bounds = StraightenBounds(3000, 4000, 7.3);
    EXPECT_GE(params.box.x, bounds.x - 1e-9);
    EXPECT_LE(params.box.Right(), bounds.Right() + 1e-9);
    EXPECT_GE(params.box.y, bounds.y - 1e-9);
    EXPECT_LE(params.box.Bottom(), bounds.Bottom() + 1e-9);
    EXPECT_TRUE(tool.CanApply(nullptr));
}

TEST(Crop, ParamsRoundTrip)
{
    CropParams p;
    p.quarterTurns = 3;
    p.straighten = -2.5;
    p.box = { 1, 2, 3, 4 };
    p.refW = 10;
    p.refH = 20;
    p.aspect = "custom";
    p.ratioW = 21;
    p.ratioH = 9;
    p.exactSize = true;
    p.samePlace = true;
    const auto back = ParseParams(ToJson(p));
    EXPECT_EQ(back.quarterTurns, 3);
    EXPECT_DOUBLE_EQ(back.straighten, -2.5);
    EXPECT_EQ(back.box, p.box);
    EXPECT_EQ(back.aspect, "custom");
    EXPECT_DOUBLE_EQ(back.ratioW, 21);
    EXPECT_TRUE(back.exactSize);
    EXPECT_TRUE(back.samePlace);
    EXPECT_EQ(ParseParams({ { "quarterTurns", -1 } }).quarterTurns, 3);
    EXPECT_DOUBLE_EQ(ParseParams({ { "straighten", 90 } }).straighten, 45);
}

TEST(Crop, EngineWritesTheCropInTheSameFormat)
{
    TempDir dir;
    Recipes recipes{ dir };
    std::wstring error;
    auto history = HistoryStore::Open(dir / L"history.db", error);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), history.get());
    const auto source = dir / L"photo.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(source, 400, 300));
    const auto big = dir / L"big.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(big, 800, 600));

    Recipe recipe = recipes.Get("crop");
    EXPECT_EQ(recipe.ring, "tools");
    ASSERT_NE(runner.EngineFor(recipe), nullptr);
    EXPECT_TRUE(runner.EngineFor(recipe)->NeedsEditor(recipe));
    EXPECT_TRUE(runner.Applies(recipe, source));
    EXPECT_FALSE(runner.Applies(recipe, dir / L"clip.mp4"));
    // What the editor hands back.
    const auto chosen = ToJson(Box(100, 50, 200, 120, 400, 300));
    for (const auto& [k, v] : chosen.items())
        recipe.steps[0].params[k] = v;

    const auto runId = history->BeginRun(recipe, 2);
    const auto r = runner.Run(recipe, runId, source, {});
    const auto r2 = runner.Run(recipe, runId, big, {});
    history->FinishRun(runId);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    ASSERT_TRUE(r2.ok) << Narrow(r2.error);
    EXPECT_EQ(FileName(r.output), L"photo (cropped).png");
    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(r.output, w, h));
    EXPECT_EQ(w, 200u);
    EXPECT_EQ(h, 120u);
    ASSERT_TRUE(ImageSize(r2.output, w, h));
    EXPECT_EQ(w, 400u);   // the second picture, twice the size: the same crop, scaled
    EXPECT_EQ(h, 240u);
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);

    // The pixels are the source's, exactly (PNG is lossless).
    Raster in, out;
    ASSERT_HRESULT_SUCCEEDED(canvas::LoadRaster(source, in, error));
    ASSERT_HRESULT_SUCCEEDED(canvas::LoadRaster(r.output, out, error));
    for (UINT y = 0; y < out.height; y += 7)
        for (UINT x = 100; x < out.width; x += 5)   // the opaque half of the test picture
            ASSERT_TRUE(std::equal(out.At(x, y), out.At(x, y) + 4, in.At(x + 100, y + 50))) << x << "," << y;

    // Running it again never overwrites: the next one is "(cropped 2)".
    const auto again = runner.Run(recipe, runId, source, {});
    ASSERT_TRUE(again.ok);
    EXPECT_EQ(FileName(again.output), L"photo (cropped 2).png");
}

// Crop, Redact and the photo tools share one answer to which files they take, and one way of
// writing an edited copy (canvas/PictureEngine.h, canvas/Raster.h: SaveEdited).
TEST(Crop, PictureEnginesShareWhatTheyTakeAndHowCopiesAreWritten)
{
    TempDir dir;
    Recipes recipes{ dir };
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    for (const char* id : { "crop", "redact", "edit-photos" })
    {
        const auto& recipe = recipes.Get(id);
        ASSERT_NE(runner.EngineFor(recipe), nullptr) << id;
        EXPECT_TRUE(runner.Applies(recipe, dir / L"photo.jpg")) << id;
        EXPECT_FALSE(runner.Applies(recipe, dir / L"clip.mp4")) << id;
        std::wstring why;
        EXPECT_FALSE(runner.EngineFor(recipe)->Accepts(recipe, dir / L"notes.txt", &why)) << id;
        EXPECT_EQ(why, L"no image decoder for '.txt' on this PC") << id;
    }

    const Raster picture = Coordinates(40, 30);
    std::wstring error;
    const ImageFormatInfo* format = nullptr;
    std::vector<uint8_t> bytes;
    ASSERT_HRESULT_SUCCEEDED(canvas::EncodeEdited(picture, dir / L"a.JPEG", bytes, error, &format)) << Narrow(error);
    ASSERT_NE(format, nullptr);
    EXPECT_STREQ(format->id, "jpg");
    EXPECT_GT(bytes.size(), 100u);
    const auto target = dir / L"out.bin";
    WriteText(target, "");
    ASSERT_HRESULT_SUCCEEDED(canvas::SaveEdited(picture, dir / L"b.gif", target, error, &format)) << Narrow(error);
    EXPECT_STREQ(format->id, "png");   // GIF is not written back: PNG keeps the pixels
    Raster back;
    ASSERT_HRESULT_SUCCEEDED(canvas::LoadRaster(target, back, error)) << Narrow(error);
    EXPECT_EQ(back.px, picture.px);
}
