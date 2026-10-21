#include "FileIo.h"
#include "OutputPath.h"
#include "PathUtil.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "canvas/Raster.h"
#include "metadata/Container.h"
#include "metadata/Metadata.h"
#include "photo/Effects.h"
#include "photo/Filters.h"
#include "photo/Gpu.h"
#include "photo/PhotoEngine.h"
#include "photo/PhotoTools.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <random>

using namespace sd;
using namespace sd::photo;
using namespace sd::test;
using canvas::Raster;

namespace
{
    Raster Solid(UINT w, UINT h, uint8_t r, uint8_t g, uint8_t b)
    {
        Raster out;
        out.Allocate(w, h);
        for (size_t i = 0; i < out.px.size(); i += 4)
        {
            out.px[i] = b;
            out.px[i + 1] = g;
            out.px[i + 2] = r;
            out.px[i + 3] = 255;
        }
        return out;
    }

    // Grey `left` on the left half, `right` on the right half.
    Raster Step(UINT w, UINT h, uint8_t left, uint8_t right)
    {
        Raster out = Solid(w, h, left, left, left);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = w / 2; x < w; ++x)
            {
                uint8_t* p = out.At(x, y);
                p[0] = p[1] = p[2] = right;
            }
        return out;
    }

    // Something like a photo: smooth colour gradients, a few shapes and fine noise.
    Raster PhotoLike(UINT w, UINT h, unsigned seed = 1)
    {
        Raster out;
        out.Allocate(w, h);
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> noise(-6, 6);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                const double u = double(x) / w, v = double(y) / h;
                double r = 60 + 150 * u, g = 50 + 120 * v, b = 90 + 80 * (1 - u) * v;
                if ((x / (w / 8 + 1) + y / (h / 6 + 1)) % 3 == 0)
                    r *= 0.6, g *= 0.7, b *= 0.6;
                uint8_t* p = out.At(x, y);
                p[0] = static_cast<uint8_t>(std::clamp(b + noise(rng), 0.0, 255.0));
                p[1] = static_cast<uint8_t>(std::clamp(g + noise(rng), 0.0, 255.0));
                p[2] = static_cast<uint8_t>(std::clamp(r + noise(rng), 0.0, 255.0));
                p[3] = 255;
            }
        return out;
    }

    double Luma(const uint8_t* p) { return 0.2126 * p[2] + 0.7152 * p[1] + 0.0722 * p[0]; }

    // Mean and standard deviation of the luma inside a rectangle.
    void Stats(const Raster& r, UINT x0, UINT y0, UINT x1, UINT y1, double& mean, double& sd)
    {
        double s = 0, s2 = 0;
        size_t n = 0;
        for (UINT y = y0; y < y1; ++y)
            for (UINT x = x0; x < x1; ++x, ++n)
            {
                const double l = Luma(r.At(x, y));
                s += l;
                s2 += l * l;
            }
        mean = s / n;
        sd = std::sqrt(std::max(0.0, s2 / n - mean * mean));
    }

    double MeanAbsDiff(const Raster& a, const Raster& b)
    {
        if (a.width != b.width || a.height != b.height)
            return 1e9;
        double s = 0;
        size_t n = 0;
        for (size_t i = 0; i < a.px.size(); i += 4)
            for (int c = 0; c < 3; ++c, ++n)
                s += std::abs(int(a.px[i + c]) - int(b.px[i + c]));
        return s / n;
    }

    bool HaveGpu() { return gpu::Device::Get() != nullptr; }

    // Wine's Direct2D has no custom effects; everywhere else they must work.
    bool UnderWine()
    {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version");
    }

    HRESULT WriteImage(const std::wstring& path, const Raster& r, const char* exifMake = nullptr)
    {
        const auto& format = canvas::EditedFormatFor(LowerExtension(path));
        std::vector<uint8_t> bytes;
        std::wstring error;
        HRESULT hr = canvas::EncodeRaster(r, format, canvas::EditedQuality(format), bytes, error);
        if (SUCCEEDED(hr) && exifMake)
        {
            std::vector<uint8_t> tagged;
            if (!metadata::SetField(bytes, "exif.0th.271", Widen(exifMake), tagged, error))
                return E_FAIL;
            bytes = std::move(tagged);
        }
        return SUCCEEDED(hr) && WriteFileBytes(path, bytes, WriteMode::Any) ? S_OK : E_FAIL;
    }

    std::wstring FieldValue(const std::wstring& path, const std::string& key)
    {
        for (const auto& f : metadata::ReadMetadata(path).fields)
            if (f.key == key)
                return f.value;
        return {};
    }
}

// ---- Edit Photos: settings ------------------------------------------------------------------------

TEST(Photo, AdjustmentsParseClampAndRoundTrip)
{
    Adjustments a = ParseAdjustments({ { "exposure", 9 }, { "contrast", -250 }, { "clarity", 40 }, { "nonsense", 3 }, { "grain", "x" } });
    EXPECT_DOUBLE_EQ(a.exposure, 3);
    EXPECT_DOUBLE_EQ(a.contrast, -100);
    EXPECT_DOUBLE_EQ(a.clarity, 40);
    EXPECT_DOUBLE_EQ(a.grain, 0);
    EXPECT_FALSE(a.IsIdentity());
    EXPECT_EQ(ParseAdjustments(ToJson(a)), a);
    EXPECT_EQ(ToJson(a).size(), Sliders().size());   // a recipe file shows every key
    Adjustments none;
    none.grainSize = 3;
    EXPECT_TRUE(none.IsIdentity());
    for (const auto& s : Sliders())
    {
        EXPECT_EQ(FindSlider(s.key), &s);
        EXPECT_LT(s.min, s.max);
    }
    EXPECT_EQ(FindSlider("nope"), nullptr);
    for (const auto& p : Presets())
        EXPECT_FALSE(p.values.IsIdentity()) << Narrow(p.label);
}

TEST(Photo, RadiiFollowThePictureSize)
{
    Adjustments a;
    a.denoise = 50;
    const auto small12 = RadiiFor(400, 300, a), big = RadiiFor(4000, 3000, a), huge = RadiiFor(8000, 6000, a);
    EXPECT_DOUBLE_EQ(big.claritySigma, 40);   // 1% of the longer side
    EXPECT_DOUBLE_EQ(small12.claritySigma, 4);
    EXPECT_NEAR(huge.denoiseSpatial, 2 * big.denoiseSpatial, 1e-9);
    EXPECT_GE(small12.sharpenSigma, 0.7);
    EXPECT_FALSE(NeedsDevelop(Adjustments{}));
    a = {};
    a.clarity = 10;
    EXPECT_TRUE(NeedsBlur(a));
    EXPECT_FALSE(NeedsDenoise(a));
}

TEST(Photo, IdentityChangesNothing)
{
    const Raster in = PhotoLike(64, 48);
    Raster out = in;
    ApplyAdjustments(out, Adjustments{}, nullptr, 1.0);
    EXPECT_LT(MeanAbsDiff(in, out), 0.6);
}

TEST(Photo, ExposureSaturationAndTemperature)
{
    const Raster grey = Solid(32, 32, 100, 100, 100);
    Adjustments a;
    a.exposure = 1;
    Raster out = grey;
    ApplyAdjustments(out, a, nullptr, 1.0);
    EXPECT_GT(Luma(out.At(16, 16)), 125);   // one stop up: brighter (in display values, less than double)

    const Raster colour = Solid(32, 32, 200, 80, 40);
    a = {};
    a.saturation = -100;
    out = colour;
    ApplyAdjustments(out, a, nullptr, 1.0);
    const uint8_t* p = out.At(5, 5);
    EXPECT_LE(std::abs(int(p[0]) - int(p[2])), 2);   // grey
    EXPECT_LE(std::abs(int(p[1]) - int(p[2])), 2);

    a = {};
    a.temperature = 60;
    out = grey;
    ApplyAdjustments(out, a, nullptr, 1.0);
    p = out.At(5, 5);
    EXPECT_GT(p[2], p[0] + 10);   // warmer: more red than blue
}

TEST(Photo, DehazeRestoresContrastOfAHazyPicture)
{
    // A clear scene seen through a uniform white haze, I = J t + A (1 - t) with t = 0.45 and
    // A = 0.92, under a band of sky where only the haze shows (t = 0): what the prior relies on.
    constexpr UINT W = 256, H = 192, kSky = 32;
    Raster clear = PhotoLike(W, H, 3);
    for (UINT y = 0; y < kSky; ++y)
        for (UINT x = 0; x < W; ++x)
            std::fill(clear.At(x, y), clear.At(x, y) + 3, uint8_t(235));
    Raster hazy = clear;
    for (UINT y = kSky; y < H; ++y)
        for (UINT x = 0; x < W; ++x)
            for (int c = 0; c < 3; ++c)
                hazy.At(x, y)[c] = static_cast<uint8_t>(std::lround(clear.At(x, y)[c] * 0.45 + 0.92 * 255 * 0.55));

    const HazeModel model = AnalyseHaze(hazy);
    ASSERT_FALSE(model.Empty());
    for (int c = 0; c < 3; ++c)
        EXPECT_NEAR(model.atmosphere[c], 0.92, 0.05);
    // The haze estimate is high in the hazy part.
    EXPECT_GT(model.At(128, 120, W, H, 0.7f), 0.3f);

    // Below the sky (and the analysis patch around its edge).
    constexpr UINT top = kSky + 16;
    double meanBefore = 0, sdBefore = 0, meanAfter = 0, sdAfter = 0, meanClear = 0, sdClear = 0;
    Stats(hazy, 0, top, W, H, meanBefore, sdBefore);
    Stats(clear, 0, top, W, H, meanClear, sdClear);
    Adjustments a;
    a.dehaze = 100;
    Raster out = hazy;
    ApplyAdjustments(out, a, &model, 1.0);
    Stats(out, 0, top, W, H, meanAfter, sdAfter);
    EXPECT_GT(sdAfter, sdBefore * 1.4);   // much more contrast
    EXPECT_LT(meanAfter, meanBefore - 15);   // the white veil is lifted
    Raster outPart, hazyPart, clearPart;
    for (auto* pair : { &outPart, &hazyPart, &clearPart })
        pair->Allocate(W, H - top);
    for (UINT y = top; y < H; ++y)
        for (UINT x = 0; x < W; ++x)
        {
            std::copy_n(out.At(x, y), 4, outPart.At(x, y - top));
            std::copy_n(hazy.At(x, y), 4, hazyPart.At(x, y - top));
            std::copy_n(clear.At(x, y), 4, clearPart.At(x, y - top));
        }
    EXPECT_LT(MeanAbsDiff(outPart, clearPart), MeanAbsDiff(hazyPart, clearPart) * 0.6);   // closer to the clear scene

    // The dark channel of a hazy picture is bright; of the dehazed one, dark.
    const Plane before = DarkChannel(hazy, 4), after = DarkChannel(out, 4);
    double db = 0, da = 0;
    for (UINT y = top; y < H; ++y)
        for (UINT x = 0; x < W; ++x)
            db += before.At(x, y), da += after.At(x, y);
    EXPECT_LT(da, db * 0.7);

    // Negative dehaze adds haze.
    a.dehaze = -60;
    Raster foggier = clear;
    const HazeModel clearModel = AnalyseHaze(clear);
    ApplyAdjustments(foggier, a, &clearModel, 1.0);
    double m = 0, sd = 0;
    Stats(foggier, 0, top, W, H, m, sd);
    EXPECT_LT(sd, sdClear);
}

TEST(Photo, ClarityRaisesLocalContrastAtAnEdge)
{
    const Raster edge = Step(400, 40, 100, 150);
    Adjustments a;
    a.clarity = 100;
    Raster out = edge;
    ApplyAdjustments(out, a, nullptr, 1.0);
    // Next to the edge: the dark side darker, the light side lighter.
    EXPECT_LT(Luma(out.At(196, 20)), Luma(edge.At(196, 20)) - 2);
    EXPECT_GT(Luma(out.At(204, 20)), Luma(edge.At(204, 20)) + 2);
    // Far from it the flat areas stay as they were.
    EXPECT_NEAR(Luma(out.At(20, 20)), Luma(edge.At(20, 20)), 2.5);
    EXPECT_NEAR(Luma(out.At(380, 20)), Luma(edge.At(380, 20)), 2.5);

    a.clarity = -100;   // softer
    out = edge;
    ApplyAdjustments(out, a, nullptr, 1.0);
    EXPECT_GT(Luma(out.At(196, 20)), Luma(edge.At(196, 20)) + 1);
}

TEST(Photo, NoiseReductionKeepsEdges)
{
    Raster noisy = Step(160, 160, 80, 170);
    std::mt19937 rng(9);
    std::normal_distribution<double> n(0, 10);
    for (size_t i = 0; i < noisy.px.size(); i += 4)
    {
        const double d = n(rng);
        for (int c = 0; c < 3; ++c)
            noisy.px[i + c] = static_cast<uint8_t>(std::clamp(noisy.px[i + c] + d, 0.0, 255.0));
    }
    Adjustments a;
    a.denoise = 80;
    Raster out = noisy;
    ApplyAdjustments(out, a, nullptr, 1.0);
    double m0 = 0, sd0 = 0, m1 = 0, sd1 = 0;
    Stats(noisy, 10, 10, 70, 150, m0, sd0);
    Stats(out, 10, 10, 70, 150, m1, sd1);
    EXPECT_LT(sd1, sd0 * 0.5);   // the flat area is much smoother
    EXPECT_NEAR(m1, m0, 3);
    double l = 0, r = 0, dummy = 0;
    Stats(out, 74, 10, 78, 150, l, dummy);
    Stats(out, 82, 10, 86, 150, r, dummy);
    EXPECT_GT(r - l, 75);         // the edge (90 levels) is still sharp
}

TEST(Photo, SharpenAndGrainAndVignette)
{
    const Raster edge = Step(200, 20, 90, 160);
    Adjustments a;
    a.sharpen = 100;
    Raster out = edge;
    ApplyAdjustments(out, a, nullptr, 1.0);
    EXPECT_LT(Luma(out.At(99, 10)), Luma(edge.At(99, 10)) - 2);
    EXPECT_GT(Luma(out.At(100, 10)), Luma(edge.At(100, 10)) + 2);

    const Raster grey = Solid(128, 128, 128, 128, 128);
    a = {};
    a.grain = 80;
    out = grey;
    ApplyAdjustments(out, a, nullptr, 1.0);
    double m = 0, sd = 0;
    Stats(out, 0, 0, 128, 128, m, sd);
    EXPECT_GT(sd, 2);
    EXPECT_NEAR(m, 128, 4);
    Raster again = grey;
    ApplyAdjustments(again, a, nullptr, 1.0);
    EXPECT_EQ(again.px, out.px);   // the same grain every time (preview = export)

    a = {};
    a.vignette = 80;
    out = grey;
    ApplyAdjustments(out, a, nullptr, 1.0);
    EXPECT_LT(Luma(out.At(1, 1)), Luma(out.At(64, 64)) - 10);
}

TEST(Photo, AutoAdjustBrightensADarkPicture)
{
    Raster dark = PhotoLike(128, 96);
    for (auto& v : dark.px)
        v = static_cast<uint8_t>(v / 3);
    for (size_t i = 3; i < dark.px.size(); i += 4)
        dark.px[i] = 255;
    const Adjustments a = AutoAdjust(dark);
    EXPECT_GT(a.exposure, 0.3);
    EXPECT_FALSE(a.IsIdentity());
}

// ---- Layouts --------------------------------------------------------------------------------------

TEST(Photo, BackgroundLayout)
{
    BackgroundParams p;
    p.padding = 10;
    p.shadow = 0;
    auto l = LayoutBackground(1000, 500, p);
    EXPECT_EQ(l.width, 1200);
    EXPECT_EQ(l.height, 700);
    EXPECT_EQ(l.picture, (RectI{ 100, 100, 1000, 500 }));
    EXPECT_DOUBLE_EQ(l.radius, 500 * 0.25 * 0.12);
    EXPECT_DOUBLE_EQ(l.shadowSigma, 0);

    p.aspect = "1:1";
    p.shadow = 100;
    l = LayoutBackground(1000, 500, p);
    EXPECT_EQ(l.width, 1200);
    EXPECT_EQ(l.height, 1200);
    EXPECT_EQ(l.picture, (RectI{ 100, 350, 1000, 500 }));   // centred
    EXPECT_DOUBLE_EQ(l.shadowSigma, 30);
    EXPECT_GT(l.shadowOffset, 0);

    p.aspect = "9:16";
    l = LayoutBackground(1000, 500, p);
    EXPECT_NEAR(double(l.width) / l.height, 9.0 / 16, 0.002);
    EXPECT_GE(l.picture.x, 100);

    // Never beyond the bitmap limit: the padding gives way.
    p.aspect = "auto";
    p.padding = 40;
    l = LayoutBackground(16000, 9000, p);
    EXPECT_LE(std::max(l.width, l.height), 16384);
    EXPECT_EQ(l.picture.w, 16000);

    EXPECT_EQ(ParseBackground(ToJson(p)).padding, 40);
    EXPECT_EQ(ParseBackground({ { "fill", "image" } }).fill, Fill::Blur);   // no picture chosen
    const auto cover = CoverSource({ 400, 200 }, { 100, 100 });
    EXPECT_EQ(cover, (RectD{ 100, 0, 200, 200 }));
    const auto fit = FitInside({ 400, 200 }, { 0, 0, 100, 100 });
    EXPECT_EQ(fit, (RectD{ 0, 25, 100, 50 }));
}

TEST(Photo, CollageGridRowColumnFeatured)
{
    CollageParams p;
    p.size = 1000;
    p.spacing = 0;
    const std::vector<SizeD> squares(4, SizeD{ 300, 300 });
    auto l = LayoutCollage(squares, p);
    EXPECT_EQ(l.width, 1000);
    EXPECT_EQ(l.height, 1000);
    ASSERT_EQ(l.cells.size(), 4u);
    EXPECT_EQ(l.cells[0].rect, (RectI{ 0, 0, 500, 500 }));
    EXPECT_EQ(l.cells[3].rect, (RectI{ 500, 500, 500, 500 }));

    // Three in a 2x2 grid: the last row's photo is as wide as the row.
    l = LayoutCollage(std::vector<SizeD>(3, SizeD{ 300, 300 }), p);
    ASSERT_EQ(l.cells.size(), 3u);
    EXPECT_EQ(l.cells[2].rect.w, 1000);

    // A row keeps every photo whole: same height, widths by shape.
    p.kind = CollageKind::Row;
    p.spacing = 50;   // 2% of 1000 = 20 px
    const std::vector<SizeD> mixed = { { 400, 300 }, { 300, 400 }, { 800, 400 } };
    l = LayoutCollage(mixed, p);
    EXPECT_EQ(l.gap, 20);
    ASSERT_EQ(l.cells.size(), 3u);
    for (const auto& c : l.cells)
        EXPECT_NEAR(c.rect.h, l.cells[0].rect.h, 1);
    EXPECT_NEAR(double(l.cells[0].rect.w) / l.cells[0].rect.h, 4.0 / 3, 0.02);
    EXPECT_NEAR(double(l.cells[2].rect.w) / l.cells[2].rect.h, 2.0, 0.02);
    EXPECT_EQ(l.cells[0].rect.x, 20);
    EXPECT_NEAR(l.cells[2].rect.x + l.cells[2].rect.w, l.width - 20, 1);

    p.kind = CollageKind::Column;
    l = LayoutCollage(mixed, p);
    for (const auto& c : l.cells)
        EXPECT_NEAR(c.rect.w, l.cells[0].rect.w, 1);
    EXPECT_EQ(l.height, 1000);

    // Featured: the first photo takes two thirds; the order decides which is first.
    p.kind = CollageKind::Featured;
    p.spacing = 0;
    p.aspect = "3:2";
    p.order = { 2, 0, 1 };
    l = LayoutCollage(mixed, p);
    EXPECT_EQ(l.width, 1000);
    EXPECT_EQ(l.height, 667);
    ASSERT_EQ(l.cells.size(), 3u);
    EXPECT_EQ(l.cells[0].image, 2);
    EXPECT_NEAR(l.cells[0].rect.w, 667, 1);
    EXPECT_EQ(CellAt(l, { 10, 10 }), 0);
    EXPECT_EQ(CellAt(l, { 900, 600 }), 2);
    EXPECT_EQ(CellAt(l, { -5, 10 }), -1);

    EXPECT_EQ(NormalisedOrder({ 2, 2, 9, -1, 0 }, 4), (std::vector<int>{ 2, 0, 1, 3 }));
    const auto back = ParseCollage(ToJson(p));
    EXPECT_EQ(back.kind, CollageKind::Featured);
    EXPECT_EQ(back.order, p.order);
    EXPECT_EQ(back.aspect, "3:2");
}

// ---- Annotations ----------------------------------------------------------------------------------

TEST(Photo, AnnotationsHitDragAndJson)
{
    Annotations set;
    Annotation arrow;
    arrow.kind = Mark::Arrow;
    arrow.a = { 10, 10 };
    arrow.b = { 110, 10 };
    arrow.width = 4;
    const uint32_t a = set.Add(arrow);
    Annotation box;
    box.kind = Mark::Rect;
    box.a = { 200, 200 };
    box.b = { 300, 260 };
    const uint32_t r = set.Add(box);
    Annotation n1, n2;
    n1.kind = n2.kind = Mark::Number;
    n1.a = { 500, 500 };
    n2.a = { 600, 500 };
    const uint32_t first = set.Add(n1), second = set.Add(n2);

    EXPECT_EQ(set.HitTest({ 60, 12 }, 2), a);
    EXPECT_EQ(set.HitTest({ 60, 30 }, 2), 0u);
    EXPECT_EQ(set.HitTest({ 200, 230 }, 2), r);     // on the outline
    EXPECT_EQ(set.HitTest({ 250, 230 }, 2), 0u);    // inside an unfilled box: the picture
    set.Find(r)->fill = true;
    EXPECT_EQ(set.HitTest({ 250, 230 }, 2), r);
    EXPECT_EQ(set.NumberOf(first), 1);
    EXPECT_EQ(set.NumberOf(second), 2);
    set.Remove(first);
    EXPECT_EQ(set.NumberOf(second), 1);             // numbers close up

    // Dragging the arrow's head, then the box's corner.
    const Annotation before = *set.Find(a);
    EXPECT_EQ(set.GripAt(before, { 110, 11 }, 3), Part::B);
    set.Drag(*set.Find(a), before, Part::B, { 110, 10 }, { 130, 50 });
    EXPECT_DOUBLE_EQ(set.Find(a)->b.x, 130);
    EXPECT_DOUBLE_EQ(set.Find(a)->b.y, 50);
    EXPECT_DOUBLE_EQ(set.Find(a)->a.x, 10);
    const Annotation boxBefore = *set.Find(r);
    set.Drag(*set.Find(r), boxBefore, Part::Body, { 0, 0 }, { 5, -5 });
    EXPECT_DOUBLE_EQ(set.Find(r)->a.x, 205);
    EXPECT_DOUBLE_EQ(set.Find(r)->b.y, 255);

    Annotation text;
    text.kind = Mark::Text;
    text.text = L"Héllo\nworld";
    text.a = { 40, 300 };
    set.Add(text);
    const auto back = Annotations::FromJson(set.ToJson());
    ASSERT_EQ(back.Items().size(), set.Items().size());
    EXPECT_EQ(back.Items().back().text, L"Héllo\nworld");
    EXPECT_EQ(back.Items()[1].fill, true);
    EXPECT_EQ(Annotations::FromJson({ { "items", { { { "kind", "laser" } } } } }).Items().size(), 0u);
    EXPECT_EQ(ContrastingText(0xFFFFFF), 0x111111u);
    EXPECT_EQ(ContrastingText(0x101010), 0xFFFFFFu);
}

// ---- The editors, without a window -------------------------------------------------------------------

TEST(Photo, EditToolSettingsBecomeParams)
{
    EditTool tool({ L"C:\\a.jpg" }, { { "ask", true }, { "contrast", 12 } });
    tool.SetPicture(0, std::make_shared<Raster>(PhotoLike(80, 60)));
    EXPECT_TRUE(tool.CanApply(nullptr));
    canvas::ControlEvent e{ "exposure" };
    e.value = 0.456;
    tool.OnControl(e);
    EXPECT_DOUBLE_EQ(tool.Params().value("exposure", 0.0), 0.46);
    EXPECT_DOUBLE_EQ(tool.Params().value("contrast", 0.0), 12);
    const auto saved = tool.State();
    tool.OnControl(canvas::ControlEvent{ "preset", 0 });   // Original
    std::wstring why;
    EXPECT_FALSE(tool.CanApply(&why));
    EXPECT_FALSE(why.empty());
    tool.Restore(saved);
    EXPECT_DOUBLE_EQ(tool.Current().exposure, 0.46);
    tool.OnControl(canvas::ControlEvent{ "preset", 1 });   // Auto
    EXPECT_FALSE(tool.Current().IsIdentity());
    const auto rev = tool.Revision();
    tool.PointerDown({});
    EXPECT_TRUE(tool.Comparing());
    EXPECT_GT(tool.Revision(), rev);
    tool.PointerUp({});
    EXPECT_FALSE(tool.Comparing());
    std::vector<canvas::Control> panel;
    tool.BuildPanel(panel);
    EXPECT_GE(panel.size(), 8u);
}

TEST(Photo, AnnotateToolDrawsTypesAndKeepsMarksPerFile)
{
    AnnotateTool tool({ L"C:\\one.png", L"C:\\two.png" });
    tool.SetPicture(0, std::make_shared<Raster>(Solid(1000, 800, 200, 200, 200)));
    EXPECT_FALSE(tool.CanApply(nullptr));
    canvas::Pointer p;
    p.tolerance = 4;
    // An arrow.
    tool.SetMode(AnnotateTool::Mode::Arrow);
    p.at = { 100, 100 };
    tool.PointerDown(p);
    p.at = { 300, 200 };
    tool.PointerMove(p);
    EXPECT_TRUE(tool.PointerUp(p).changed);
    ASSERT_EQ(tool.Marks().Items().size(), 1u);
    EXPECT_EQ(tool.Marks().Items()[0].kind, Mark::Arrow);
    EXPECT_DOUBLE_EQ(tool.Marks().Items()[0].b.x, 300);
    EXPECT_EQ(tool.Selected(), tool.Marks().Items()[0].id);
    // A click without a drag makes no box.
    tool.SetMode(AnnotateTool::Mode::Rect);
    p.at = { 600, 600 };
    tool.PointerDown(p);
    tool.PointerUp(p);
    EXPECT_EQ(tool.Marks().Items().size(), 1u);
    // Text: click, type (Enter starts a new line), Ctrl+Enter ends it.
    tool.SetMode(AnnotateTool::Mode::Text);
    p.at = { 500, 400 };
    tool.PointerDown(p);
    tool.PointerUp(p);
    EXPECT_TRUE(tool.TextInput());
    for (wchar_t c : std::wstring(L"Hi"))
        tool.Char(c);
    tool.Key(VK_RETURN, false, false);
    tool.Char(L'!');
    tool.Key(VK_BACK, false, false);
    tool.Char(L'?');
    EXPECT_TRUE(tool.TextInput());
    EXPECT_TRUE(tool.Key(VK_RETURN, false, true).changed);
    EXPECT_FALSE(tool.TextInput());
    ASSERT_EQ(tool.Marks().Items().size(), 2u);
    EXPECT_EQ(tool.Marks().Items()[1].text, L"Hi\n?");
    // An empty text goes away.
    p.at = { 50, 700 };
    tool.PointerDown(p);
    tool.PointerUp(p);
    tool.Key(VK_ESCAPE, false, false);
    EXPECT_EQ(tool.Marks().Items().size(), 2u);
    // Numbers and a magnifier placed by clicks.
    tool.SetMode(AnnotateTool::Mode::Number);
    p.at = { 800, 100 };
    tool.PointerDown(p);
    tool.PointerUp(p);
    tool.SetMode(AnnotateTool::Mode::Magnifier);
    p.at = { 700, 500 };
    tool.PointerDown(p);
    tool.PointerUp(p);
    ASSERT_EQ(tool.Marks().Items().size(), 4u);
    const auto& loupe = tool.Marks().Items()[3];
    EXPECT_EQ(loupe.kind, Mark::Magnifier);
    EXPECT_GT(std::hypot(loupe.b.x - loupe.a.x, loupe.b.y - loupe.a.y), loupe.radius);   // beside the spot
    // Delete removes the selected one.
    tool.SetMode(AnnotateTool::Mode::Select);
    p.at = { 800, 100 };
    tool.PointerDown(p);
    tool.PointerUp(p);
    EXPECT_TRUE(tool.Key(VK_DELETE, false, false).changed);
    EXPECT_EQ(tool.Marks().Items().size(), 3u);

    // Undo state round trip, then the second file gets its own (empty) set.
    const auto state = tool.State();
    tool.SetPicture(1, std::make_shared<Raster>(Solid(400, 300, 10, 10, 10)));
    EXPECT_TRUE(tool.Marks().Items().empty());
    tool.Restore(state);
    const auto params = tool.Params();
    ASSERT_TRUE(params.contains("perFile"));
    EXPECT_EQ(params["perFile"][Narrow(L"C:\\one.png")]["items"].size(), 3u);
    EXPECT_EQ(params["perFile"][Narrow(L"C:\\two.png")]["items"].size(), 0u);
    EXPECT_EQ(Annotations::FromJson(ParamsForFile(params, L"C:\\one.png")).Items().size(), 3u);
    EXPECT_TRUE(tool.CanApply(nullptr));
}

TEST(Photo, BackgroundToolGrowsTheCanvas)
{
    BackgroundTool tool({ L"C:\\a.png" }, { { "padding", 10 }, { "aspect", "auto" } });
    tool.SetPicture(0, std::make_shared<Raster>(Solid(400, 200, 1, 2, 3)));
    EXPECT_DOUBLE_EQ(tool.Size().w, 480);
    EXPECT_DOUBLE_EQ(tool.Size().h, 280);
    canvas::ControlEvent e{ "aspect", 1 };   // 1:1
    tool.OnControl(e);
    EXPECT_DOUBLE_EQ(tool.Size().w, tool.Size().h);
    tool.OnControl(canvas::ControlEvent{ "fill", 0 });
    tool.OnControl(canvas::ControlEvent{ "color1", 1 });
    EXPECT_EQ(tool.Params().value("fill", std::string()), "solid");
    EXPECT_EQ(tool.Params().value("color1", 0u), 0x1F1F1Fu);
    EXPECT_EQ(tool.Params().value("aspect", std::string()), "1:1");
}

TEST(Photo, BackgroundFitToPicture)
{
    // The canvas is the picture: no margin, so no spacing and no shadow; the corners still round.
    BackgroundParams p;
    p.aspect = kFitToPicture;
    p.padding = 20;
    p.shadow = 100;
    const auto l = LayoutBackground(1000, 500, p);
    EXPECT_EQ(l.width, 1000);
    EXPECT_EQ(l.height, 500);
    EXPECT_EQ(l.picture, (RectI{ 0, 0, 1000, 500 }));
    EXPECT_DOUBLE_EQ(l.shadowSigma, 0);
    EXPECT_DOUBLE_EQ(l.radius, 500 * 0.25 * 0.12);
    EXPECT_EQ(ParseBackground(ToJson(p)).aspect, kFitToPicture);   // kept in recipes
    EXPECT_EQ(ParseBackground({ { "aspect", "nonsense" } }).aspect, "auto");

    // In the tool it comes after the shapes, and the sliders it makes pointless go.
    BackgroundTool tool({ L"C:\\a.png" }, { { "padding", 10 }, { "aspect", "auto" } });
    tool.SetPicture(0, std::make_shared<Raster>(Solid(400, 200, 1, 2, 3)));
    std::vector<canvas::Control> before;
    tool.BuildPanel(before);
    EXPECT_TRUE(std::any_of(before.begin(), before.end(), [](const canvas::Control& c) { return c.id == "padding"; }));
    const int fit = static_cast<int>(AspectChoices().size());
    tool.OnControl(canvas::ControlEvent{ "aspect", fit });
    EXPECT_DOUBLE_EQ(tool.Size().w, 400);
    EXPECT_DOUBLE_EQ(tool.Size().h, 200);
    EXPECT_EQ(tool.Params().value("aspect", std::string()), kFitToPicture);
    std::vector<canvas::Control> controls;
    tool.BuildPanel(controls);
    const auto aspect = std::find_if(controls.begin(), controls.end(), [](const canvas::Control& c) { return c.id == "aspect"; });
    ASSERT_NE(aspect, controls.end());
    EXPECT_EQ(aspect->selected, fit);
    EXPECT_EQ(aspect->options.back(), L"Fit to picture");
    const auto has = [&](const char* id) { return std::any_of(controls.begin(), controls.end(), [&](const canvas::Control& c) { return c.id == id; }); };
    EXPECT_FALSE(has("padding"));
    EXPECT_FALSE(has("shadow"));
    EXPECT_TRUE(has("radius"));
    tool.OnControl(canvas::ControlEvent{ "aspect", 0 });   // back to Auto: the margin returns
    EXPECT_DOUBLE_EQ(tool.Size().w, 480);
}

TEST(Photo, CollageToolSwapsByDragging)
{
    CollageTool tool({ L"C:\\a.png", L"C:\\b.png", L"C:\\c.png", L"C:\\d.png" }, {});
    std::vector<Picture> pics;
    for (int i = 0; i < 4; ++i)
        pics.push_back(Picture::Of(std::make_shared<Raster>(Solid(60, 60, uint8_t(i * 60), 0, 0))));
    tool.SetPictures(pics);
    EXPECT_TRUE(tool.CanApply(nullptr));
    ASSERT_EQ(tool.Layout().cells.size(), 4u);
    const auto c0 = tool.Layout().cells[0].rect, c3 = tool.Layout().cells[3].rect;
    canvas::Pointer p;
    p.at = { c0.x + 5.0, c0.y + 5.0 };
    tool.PointerDown(p);
    p.at = { c3.x + 5.0, c3.y + 5.0 };
    tool.PointerMove(p);
    EXPECT_TRUE(tool.PointerUp(p).changed);
    EXPECT_EQ(tool.Params()["order"], (nlohmann::json{ 3, 1, 2, 0 }));
    tool.OnControl(canvas::ControlEvent{ "reverse" });
    EXPECT_EQ(tool.Params()["order"], (nlohmann::json{ 0, 2, 1, 3 }));
    tool.OnControl(canvas::ControlEvent{ "layout", 1 });
    EXPECT_EQ(tool.Params().value("layout", std::string()), "row");
    canvas::ControlEvent size{ "size" };
    size.value = 1200;
    tool.OnControl(size);
    EXPECT_EQ(std::max(tool.Size().w, tool.Size().h), 1200);

    CollageTool lonely({ L"C:\\a.png" }, {});
    std::wstring why;
    EXPECT_FALSE(lonely.CanApply(&why));
}

TEST(Photo, ParamsForFileMergesOverTheShared)
{
    const nlohmann::json params = { { "a", 1 }, { "perFile", { { "C:\\x.png", { { "a", 2 }, { "b", 3 } } } } } };
    const auto x = ParamsForFile(params, L"C:\\x.png");
    EXPECT_EQ(x["a"], 2);
    EXPECT_EQ(x["b"], 3);
    EXPECT_FALSE(x.contains("perFile"));
    EXPECT_EQ(ParamsForFile(params, L"C:\\y.png")["a"], 1);
}

// ---- Recipes ----------------------------------------------------------------------------------------

TEST(Photo, RecipesAreToolsAndCollageNeedsTwo)
{
    TempDir dir;
    Recipes recipes{ dir };
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    for (const char* id : { "edit-photos", "annotate-photos", "add-background", "collage" })
    {
        const Recipe& r = recipes.Get(id);
        EXPECT_EQ(r.ring, "tools") << id;
        ASSERT_NE(runner.EngineFor(r), nullptr) << id;
        EXPECT_TRUE(runner.Applies(r, L"C:\\photo.jpg")) << id;
        EXPECT_FALSE(runner.Applies(r, L"C:\\clip.mp4")) << id;
        EXPECT_FALSE(runner.Applies(r, L"C:\\notes.txt")) << id;
        EXPECT_TRUE(runner.EngineFor(r)->NeedsEditor(r)) << id;
    }
    const Recipe& collage = recipes.Get("collage");
    EXPECT_FALSE(runner.AppliesToDrop(collage, 1));
    EXPECT_TRUE(runner.AppliesToDrop(collage, 2));
    EXPECT_TRUE(runner.AppliesToDrop(recipes.Get("edit-photos"), 1));
    EXPECT_TRUE(runner.EngineFor(collage)->IsBatch(collage));
    Recipe quick = recipes.Get("edit-photos");
    quick.steps[0].params["ask"] = false;
    EXPECT_FALSE(runner.EngineFor(quick)->NeedsEditor(quick));
}

// ---- The GPU --------------------------------------------------------------------------------------

TEST(Photo, ShadersCompile)
{
    for (const char* name : { "denoise", "develop", "finish" })
        EXPECT_NE(fx::ShaderSource(name).find("main"), std::string::npos) << name;
    std::wstring error;
    if (!fx::CompileShaders(error) && error.find(L"d3dcompiler") != std::wstring::npos)
        GTEST_SKIP() << "no shader compiler here: " << Narrow(error);
    EXPECT_TRUE(error.empty()) << Narrow(error);
}

TEST(Photo, GpuMatchesTheCpu)
{
    if (!HaveGpu())
        GTEST_SKIP() << "no Direct3D 11 device";
    const Raster in = PhotoLike(320, 240, 5);
    Adjustments a;
    a.exposure = 0.4;
    a.contrast = 20;
    a.highlights = -30;
    a.shadows = 25;
    a.temperature = 15;
    a.vibrance = 30;
    a.clarity = 40;
    a.sharpen = 30;
    a.dehaze = 30;
    a.denoise = 30;
    a.vignette = 20;
    Raster gpuOut, cpuOut;
    std::wstring error;
    RenderInfo info;
    ASSERT_HRESULT_SUCCEEDED(RenderAdjusted(in, a, gpuOut, error, &info, true)) << Narrow(error);
    if (info.device == L"CPU" && UnderWine())
        GTEST_SKIP() << "Wine's Direct2D can't run custom effects: " << Narrow(info.note);
    ASSERT_NE(info.device, L"CPU") << "the photo effects didn't run on the GPU: " << Narrow(info.note);
    ASSERT_HRESULT_SUCCEEDED(RenderAdjusted(in, a, cpuOut, error, nullptr, false));
    EXPECT_LT(MeanAbsDiff(gpuOut, cpuOut), 2.0) << "on " << Narrow(info.device);
}

TEST(Photo, RendersOnTheGpu)
{
    if (!HaveGpu())
        GTEST_SKIP() << "no Direct3D 11 device";
    const Raster in = Solid(400, 300, 30, 120, 200);
    std::wstring error;

    // Background: a solid red border around the picture, the picture untouched in the middle.
    BackgroundParams bp;
    bp.fill = Fill::Solid;
    bp.color1 = 0xFF0000;
    bp.padding = 10;
    bp.radius = 0;
    bp.shadow = 0;
    Raster out;
    ASSERT_HRESULT_SUCCEEDED(RenderBackground(in, bp, nullptr, out, error)) << Narrow(error);
    ASSERT_EQ(out.width, 480u);
    ASSERT_EQ(out.height, 380u);
    EXPECT_EQ(out.At(5, 5)[2], 255);
    EXPECT_EQ(out.At(5, 5)[1], 0);
    EXPECT_NEAR(out.At(240, 190)[0], 200, 2);
    EXPECT_NEAR(out.At(240, 190)[1], 120, 2);

    // Annotate: a filled box shows its colour; elsewhere the picture is unchanged.
    Annotations marks;
    Annotation box;
    box.kind = Mark::Rect;
    box.fill = true;
    box.color = 0x00FF00;
    box.a = { 50, 50 };
    box.b = { 150, 150 };
    marks.Add(box);
    ASSERT_HRESULT_SUCCEEDED(RenderAnnotated(in, marks, {}, out, error)) << Narrow(error);
    ASSERT_EQ(out.width, 400u);
    EXPECT_EQ(out.At(100, 100)[1], 255);
    EXPECT_EQ(out.At(100, 100)[2], 0);
    EXPECT_NEAR(out.At(300, 250)[0], 200, 2);

    // Collage: two pictures side by side on white.
    CollageParams cp;
    cp.kind = CollageKind::Row;
    cp.size = 800;
    cp.spacing = 50;
    std::vector<Picture> pics = { Picture::Of(std::make_shared<Raster>(Solid(200, 200, 255, 0, 0))),
                                  Picture::Of(std::make_shared<Raster>(Solid(200, 200, 0, 0, 255))) };
    ASSERT_HRESULT_SUCCEEDED(RenderCollage(pics, cp, out, error)) << Narrow(error);
    const auto layout = LayoutCollage({ { 200, 200 }, { 200, 200 } }, cp);
    ASSERT_EQ(out.width, static_cast<UINT>(layout.width));
    const auto& left = layout.cells[0].rect;
    EXPECT_EQ(out.At(left.x + left.w / 2, left.y + left.h / 2)[2], 255);
    EXPECT_EQ(out.At(2, 2)[0], 255);   // the white background
    EXPECT_EQ(out.At(2, 2)[1], 255);
}

// ---- End to end through the engine -------------------------------------------------------------------

TEST(Photo, EngineEditsKeepFormatAndExif)
{
    TempDir dir;
    Recipes recipes{ dir };
    std::wstring error;
    auto history = HistoryStore::Open(dir / L"history.db", error);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), history.get());
    const auto source = dir / L"photo.jpg";
    ASSERT_HRESULT_SUCCEEDED(WriteImage(source, PhotoLike(300, 200), "Leica"));
    ASSERT_EQ(FieldValue(source, "exif.0th.271"), L"Leica");

    Recipe recipe = recipes.Get("edit-photos");
    recipe.steps[0].params["ask"] = false;
    recipe.steps[0].params["exposure"] = 0.5;
    recipe.steps[0].params["vibrance"] = 30;
    const auto runId = history->BeginRun(recipe, 1);
    const auto r = runner.Run(recipe, runId, source, {});
    history->FinishRun(runId);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    EXPECT_EQ(FileName(r.output), L"photo (edited).jpg");
    EXPECT_EQ(FieldValue(r.output, "exif.0th.271"), L"Leica");   // the camera stays with the photo
    Raster in, out;
    ASSERT_HRESULT_SUCCEEDED(canvas::LoadRaster(source, in, error));
    ASSERT_HRESULT_SUCCEEDED(canvas::LoadRaster(r.output, out, error));
    ASSERT_EQ(out.width, 300u);
    double mi = 0, mo = 0, sd = 0;
    Stats(in, 0, 0, 300, 200, mi, sd);
    Stats(out, 0, 0, 300, 200, mo, sd);
    EXPECT_GT(mo, mi + 8);   // brighter
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
}

TEST(Photo, EngineAnnotatesBackgroundsAndCollages)
{
    if (!HaveGpu())
        GTEST_SKIP() << "no Direct3D 11 device";
    TempDir dir;
    Recipes recipes{ dir };
    std::wstring error;
    auto history = HistoryStore::Open(dir / L"history.db", error);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), history.get());
    const auto one = dir / L"one.png", two = dir / L"two.png", three = dir / L"three.png";
    ASSERT_HRESULT_SUCCEEDED(WriteImage(one, PhotoLike(300, 200, 1)));
    ASSERT_HRESULT_SUCCEEDED(WriteImage(two, PhotoLike(200, 300, 2)));
    ASSERT_HRESULT_SUCCEEDED(WriteImage(three, PhotoLike(250, 250, 3)));

    // Annotate: marks for one file; the other is skipped, not failed.
    Recipe annotate = recipes.Get("annotate-photos");
    Annotations marks;
    Annotation arrow;
    arrow.kind = Mark::Arrow;
    arrow.a = { 20, 20 };
    arrow.b = { 200, 150 };
    arrow.width = 8;
    marks.Add(arrow);
    annotate.steps[0].params["perFile"] = { { Narrow(one), marks.ToJson() } };
    auto runId = history->BeginRun(annotate, 2);
    const auto a1 = runner.Run(annotate, runId, one, {});
    const auto a2 = runner.Run(annotate, runId, two, {});
    history->FinishRun(runId);
    ASSERT_TRUE(a1.ok) << Narrow(a1.error);
    EXPECT_EQ(FileName(a1.output), L"one (annotated).png");
    EXPECT_FALSE(a2.ok);

    // Background, without asking.
    Recipe background = recipes.Get("add-background");
    background.steps[0].params["ask"] = false;
    background.steps[0].params["aspect"] = "1:1";
    runId = history->BeginRun(background, 1);
    const auto b = runner.Run(background, runId, two, {});
    history->FinishRun(runId);
    ASSERT_TRUE(b.ok) << Narrow(b.error);
    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(b.output, w, h));
    EXPECT_EQ(w, h);
    EXPECT_GT(w, 300u);

    // Collage of three, in the order given.
    Recipe collage = recipes.Get("collage");
    collage.steps[0].params["ask"] = false;
    collage.steps[0].params["size"] = 900;
    collage.steps[0].params["order"] = { 2, 0, 1 };
    runId = history->BeginRun(collage, 3);
    const auto c = runner.RunBatch(collage, runId, { one, two, three }, {});
    history->FinishRun(runId);
    ASSERT_TRUE(c.ok) << Narrow(c.error);
    EXPECT_EQ(FileName(c.output), L"three (collage).png");
    ASSERT_TRUE(ImageSize(c.output, w, h));
    EXPECT_EQ(std::max(w, h), 900u);
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
}

// ---- Timings at 12 MP (printed; the hand checklist in TESTING.md records them) -------------------------

TEST(PhotoTiming, TwelveMegapixels)
{
    const Raster big = PhotoLike(4000, 3000, 7);
    Adjustments a;
    a.exposure = 0.3;
    a.contrast = 15;
    a.shadows = 20;
    a.vibrance = 20;
    a.clarity = 30;
    a.dehaze = 25;
    a.sharpen = 20;
    a.denoise = 20;
    a.grain = 10;
    a.vignette = 15;
    std::wstring error;
    Raster out;
    RenderInfo info;
    if (HaveGpu())
    {
        ASSERT_HRESULT_SUCCEEDED(RenderAdjusted(big, a, out, error, &info, true));
        std::printf("[ timing ] edit 12 MP: %lld ms on %s %s\n", static_cast<long long>(info.ms), Narrow(info.device).c_str(), Narrow(info.note).c_str());
        BackgroundParams bp;
        bp.aspect = "4:5";
        if (SUCCEEDED(RenderBackground(big, bp, nullptr, out, error, &info)))
            std::printf("[ timing ] background 12 MP -> %ux%u: %lld ms on %s\n", out.width, out.height, static_cast<long long>(info.ms),
                        Narrow(info.device).c_str());
        Annotations marks;
        for (int i = 0; i < 20; ++i)
        {
            Annotation m;
            m.kind = i % 2 ? Mark::Arrow : Mark::Ellipse;
            m.a = { 100.0 + i * 150, 200.0 };
            m.b = { 300.0 + i * 150, 900.0 };
            m.width = 16;
            marks.Add(m);
        }
        if (SUCCEEDED(RenderAnnotated(big, marks, {}, out, error, &info)))
            std::printf("[ timing ] annotate 12 MP: %lld ms on %s\n", static_cast<long long>(info.ms), Narrow(info.device).c_str());
        CollageParams cp;
        cp.size = 6000;
        const auto shared = std::make_shared<const Raster>(big);
        if (SUCCEEDED(RenderCollage({ Picture::Of(shared), Picture::Of(shared), Picture::Of(shared), Picture::Of(shared) }, cp, out, error, &info)))
            std::printf("[ timing ] collage of 4 x 12 MP -> %ux%u: %lld ms on %s\n", out.width, out.height, static_cast<long long>(info.ms),
                        Narrow(info.device).c_str());
    }
    ASSERT_HRESULT_SUCCEEDED(RenderAdjusted(big, a, out, error, &info, false));
    std::printf("[ timing ] edit 12 MP: %lld ms on the CPU (fallback)\n", static_cast<long long>(info.ms));
    EXPECT_EQ(out.width, 4000u);
}
