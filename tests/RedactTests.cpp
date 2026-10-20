#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "canvas/Covers.h"
#include "canvas/Raster.h"
#include "metadata/Metadata.h"
#include "redact/Pii.h"
#include "redact/RedactTool.h"
#include "redact/Vision.h"

using namespace sd;
using namespace sd::canvas;
using namespace sd::redact;
using namespace sd::test;

namespace
{
    // A busy picture (every pixel different), so any change shows.
    Raster Noise(UINT w, UINT h)
    {
        Raster r;
        r.Allocate(w, h);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                UINT n = x * 0x9E3779B9u ^ y * 0x85EBCA6Bu;
                n ^= n >> 15;
                n *= 0x2C1B3C6Du;
                n ^= n >> 12;
                uint8_t* p = r.At(x, y);
                p[0] = static_cast<uint8_t>(n);
                p[1] = static_cast<uint8_t>(n >> 8);
                p[2] = static_cast<uint8_t>(n >> 16);
                p[3] = 255;
            }
        return r;
    }

    bool Inside(const RectI& r, UINT x, UINT y)
    {
        return static_cast<int>(x) >= r.x && static_cast<int>(x) < r.x + r.w && static_cast<int>(y) >= r.y && static_cast<int>(y) < r.y + r.h;
    }

    // Pixels outside every cover are untouched; returns how many inside changed.
    size_t ChangedInside(const Raster& before, const Raster& after, const std::vector<Cover>& covers)
    {
        size_t changed = 0;
        for (UINT y = 0; y < before.height; ++y)
            for (UINT x = 0; x < before.width; ++x)
            {
                const bool same = std::equal(before.At(x, y), before.At(x, y) + 4, after.At(x, y));
                const bool in = std::any_of(covers.begin(), covers.end(), [&](const Cover& c) { return Inside(c.rect, x, y); });
                if (!in)
                    EXPECT_TRUE(same) << "pixel " << x << "," << y << " outside the covers changed";
                else if (!same)
                    ++changed;
            }
        return changed;
    }

    OcrLine Line(std::initializer_list<std::pair<const wchar_t*, RectD>> words)
    {
        OcrLine line;
        for (const auto& [t, b] : words)
            line.words.push_back({ t, b });
        return line;
    }
}

// ---- Rendering ------------------------------------------------------------------------------------

TEST(Redact, SolidCoverIsExactlyTheFill)
{
    const Raster before = Noise(120, 80);
    Raster after = before;
    const std::vector<Cover> covers = { { { 10, 20, 30, 15 }, CoverKind::Solid, 0x123456 } };
    ApplyCovers(after, covers);
    for (int y = 20; y < 35; ++y)
        for (int x = 10; x < 40; ++x)
        {
            const uint8_t* p = after.At(static_cast<UINT>(x), static_cast<UINT>(y));
            ASSERT_EQ(p[0], 0x56);
            ASSERT_EQ(p[1], 0x34);
            ASSERT_EQ(p[2], 0x12);
            ASSERT_EQ(p[3], 255);
        }
    EXPECT_EQ(ChangedInside(before, after, covers), 30u * 15u);
}

TEST(Redact, BlurAndPixelateChangeOnlyTheInside)
{
    const Raster before = Noise(200, 150);
    for (const CoverKind kind : { CoverKind::Blur, CoverKind::Pixelate })
    {
        Raster after = before;
        const std::vector<Cover> covers = { { { 50, 40, 60, 50 }, kind, 0 }, { { 0, 0, 20, 20 }, kind, 0 } };
        ApplyCovers(after, covers);
        // Nearly every covered pixel is different: a strong effect, not a light touch.
        EXPECT_GT(ChangedInside(before, after, covers), (60u * 50u + 400u) * 9 / 10);
    }
}

TEST(Redact, BlurIsStrongEnoughToHideDetail)
{
    // Black and white stripes one pixel wide (like small text) come out as an even grey.
    Raster img;
    img.Allocate(300, 200);
    for (UINT y = 0; y < 200; ++y)
        for (UINT x = 0; x < 300; ++x)
        {
            uint8_t* p = img.At(x, y);
            p[0] = p[1] = p[2] = (x % 2) ? 255 : 0;
            p[3] = 255;
        }
    ApplyCover(img, { { 100, 50, 100, 100 }, CoverKind::Blur, 0 });
    int lo = 255, hi = 0;
    for (UINT y = 60; y < 140; ++y)
        for (UINT x = 110; x < 190; ++x)
        {
            lo = std::min<int>(lo, img.At(x, y)[0]);
            hi = std::max<int>(hi, img.At(x, y)[0]);
        }
    EXPECT_LT(hi - lo, 12);
}

TEST(Redact, PixelateMatchesThePreviewLayer)
{
    // The editor previews pixelation with PixelateAll; a cover must show the very same blocks.
    const Raster before = Noise(240, 160);
    Raster layer = before;
    PixelateAll(layer, PixelBlock(240, 160));
    Raster covered = before;
    const RectI r{ 37, 21, 90, 70 };
    ApplyCover(covered, { r, CoverKind::Pixelate, 0 });
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
            ASSERT_TRUE(std::equal(covered.At(x, y), covered.At(x, y) + 4, layer.At(x, y))) << x << "," << y;
}

TEST(Redact, SolidStaysSolidOverABlur)
{
    const Raster before = Noise(100, 100);
    Raster after = before;
    ApplyCovers(after, { { { 10, 10, 60, 60 }, CoverKind::Blur, 0 }, { { 30, 30, 10, 10 }, CoverKind::Solid, 0xFFFFFF } });
    for (UINT y = 30; y < 40; ++y)
        for (UINT x = 30; x < 40; ++x)
            ASSERT_EQ(after.At(x, y)[0], 255);
}

TEST(Redact, CoversRoundOutwardToWholePixels)
{
    const auto px = ToPixelCovers({ { { 10.6, 20.2, 5.1, 5.1 }, CoverKind::Solid, 0 }, { { 95, 95, 20, 20 }, CoverKind::Blur, 0 } }, 100, 100);
    ASSERT_EQ(px.size(), 2u);
    EXPECT_EQ(px[0].rect, (RectI{ 10, 20, 6, 6 }));    // 10.6..15.7 -> 10..16, 20.2..25.3 -> 20..26
    EXPECT_EQ(px[1].rect, (RectI{ 95, 95, 5, 5 }));    // clipped to the picture
    EXPECT_EQ(px[1].kind, CoverKind::Blur);
    EXPECT_TRUE(ToPixelCovers({ { { 200, 200, 5, 5 }, CoverKind::Solid, 0 } }, 100, 100).empty());
}

TEST(Redact, CoversJsonPerFile)
{
    const std::vector<std::wstring> files = { L"C:\\a\\one.jpg", L"C:\\a\\t\u00e9l\u00e9.png" };
    const auto json = CoversToJson(files, { { { { 1, 2, 3, 4 }, CoverKind::Pixelate, 0 } }, { { { 5, 6, 7, 8 }, CoverKind::Solid, 0xFF0000 } } });
    const auto one = CoversFor(json, files[0]);
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0].rect, (RectD{ 1, 2, 3, 4 }));
    EXPECT_EQ(one[0].kind, CoverKind::Pixelate);
    const auto two = CoversFor(json, files[1]);
    ASSERT_EQ(two.size(), 1u);
    EXPECT_EQ(two[0].rgb, 0xFF0000u);
    EXPECT_TRUE(CoversFor(json, L"C:\\a\\other.jpg").empty());
    EXPECT_TRUE(CoversFor(nlohmann::json::object(), files[0]).empty());
}

// ---- Personal details in text -----------------------------------------------------------------------

TEST(Redact, LuhnAndIban)
{
    EXPECT_TRUE(LuhnValid(L"4111111111111111"));
    EXPECT_TRUE(LuhnValid(L"4111 1111 1111 1111"));
    EXPECT_TRUE(LuhnValid(L"5500-0000-0000-0004"));
    EXPECT_TRUE(LuhnValid(L"378282246310005"));   // 15-digit Amex
    EXPECT_FALSE(LuhnValid(L"4111111111111112"));
    EXPECT_FALSE(LuhnValid(L"0000"));            // too short to be a card
    EXPECT_FALSE(LuhnValid(L"4111a11111111111"));

    EXPECT_TRUE(IbanValid(L"GB82 WEST 1234 5698 7654 32"));
    EXPECT_TRUE(IbanValid(L"DE89370400440532013000"));
    EXPECT_TRUE(IbanValid(L"fr1420041010050500013m02606"));
    EXPECT_FALSE(IbanValid(L"GB82 WEST 1234 5698 7654 33"));
    EXPECT_FALSE(IbanValid(L"GB82"));
}

TEST(Redact, FindsEmailsPhonesCardsAndIbans)
{
    const std::wstring text = L"Mail jane.doe+news@example.co.uk or call +44 20 7946 0958, (555) 123-4567. "
                              L"Card 4111 1111 1111 1111, IBAN GB82 WEST 1234 5698 7654 32.";
    const auto hits = FindPiiInText(text);
    std::vector<std::pair<PiiKind, std::wstring>> found;
    for (const auto& h : hits)
        found.emplace_back(h.kind, text.substr(h.begin, h.end - h.begin));
    ASSERT_EQ(found.size(), 5u);
    EXPECT_EQ(found[0], std::make_pair(PiiKind::Email, std::wstring(L"jane.doe+news@example.co.uk")));
    EXPECT_EQ(found[1].first, PiiKind::Phone);
    EXPECT_EQ(found[1].second, L"+44 20 7946 0958");
    EXPECT_EQ(found[2].first, PiiKind::Phone);
    EXPECT_EQ(found[2].second, L"(555) 123-4567");
    EXPECT_EQ(found[3], std::make_pair(PiiKind::Card, std::wstring(L"4111 1111 1111 1111")));
    EXPECT_EQ(found[4], std::make_pair(PiiKind::Iban, std::wstring(L"GB82 WEST 1234 5698 7654 32")));
}

TEST(Redact, LeavesDatesTimesAndPlainNumbersAlone)
{
    EXPECT_TRUE(FindPiiInText(L"Taken 2026-10-20 at 14:05, 3 of 12 photos, total 1 234.56").empty());
    EXPECT_TRUE(FindPiiInText(L"Order 12345 shipped 20.10.2026").empty());
    for (const auto& h : FindPiiInText(L"Not a card: 4111 1111 1111 1112"))
        EXPECT_NE(h.kind, PiiKind::Card);
    EXPECT_TRUE(FindPiiInText(L"name@localhost is not an address").empty());
}

TEST(Redact, PiiBoxesSpanTheirWords)
{
    const auto lines = std::vector<OcrLine>{
        Line({ { L"Call", { 10, 10, 40, 12 } }, { L"+1", { 55, 10, 20, 12 } }, { L"415", { 80, 10, 30, 12 } }, { L"555", { 115, 10, 30, 12 } },
               { L"0100", { 150, 10, 40, 12 } } }),
        Line({ { L"me:", { 10, 40, 30, 12 } }, { L"a.b@c.org", { 45, 40, 90, 14 } } }),
    };
    const auto hits = FindPii(lines);
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].kind, PiiKind::Phone);
    EXPECT_EQ(hits[0].box, (RectD{ 55, 10, 135, 12 }));
    EXPECT_EQ(hits[1].kind, PiiKind::Email);
    EXPECT_EQ(hits[1].box, (RectD{ 45, 40, 90, 14 }));

    const auto boxes = TextLineBoxes(lines, 1000, 1000);
    ASSERT_EQ(boxes.size(), 2u);
    EXPECT_LT(boxes[0].x, 10);   // a little padding around the line
    EXPECT_GT(boxes[0].Right(), 190);
    const auto face = FaceCover({ 100, 100, 100, 100 }, 1000, 1000);
    EXPECT_LT(face.y, 70);       // more room above, for hair
    EXPECT_GT(face.w, 130);
    EXPECT_EQ(FaceCover({ 0, 0, 100, 100 }, 1000, 1000).x, 0);   // never outside the picture
}

// ---- The editor's model -------------------------------------------------------------------------------

TEST(Redact, ProposalsAreReviewedPerFile)
{
    RedactTool tool({ L"a.jpg", L"b.jpg" }, RedactTool::Mode::Review);
    EXPECT_TRUE(tool.CanApply(nullptr));   // review: saving with no covers still removes the metadata
    tool.ImageReady(0, 400, 300);
    EXPECT_EQ(tool.Propose(1, { { 10, 10, 50, 20 }, { 12, 11, 50, 20 } }, "pii", { L"Email", L"Email" }), 1);   // the repeat is dropped
    EXPECT_EQ(tool.Propose(0, { { 100, 100, 80, 30 } }, "face", { L"Face" }), 1);
    EXPECT_EQ(tool.ProposalCount(), 2u);

    auto covers = tool.AllCovers();
    ASSERT_EQ(covers.size(), 2u);
    ASSERT_EQ(covers[0].size(), 1u);
    ASSERT_EQ(covers[1].size(), 1u);
    EXPECT_EQ(covers[1][0].rect, (RectD{ 10, 10, 50, 20 }));
    EXPECT_EQ(covers[0][0].kind, CoverKind::Solid);

    // Turning down the suggestion on the first picture (select it, Delete).
    tool.PointerDown({ { 140, 115 }, 6 });
    tool.PointerUp({ { 140, 115 }, 6 });
    tool.Key(VK_DELETE, false, false);
    const auto params = tool.Params();
    EXPECT_TRUE(CoversFor(params, L"a.jpg").empty());
    EXPECT_EQ(CoversFor(params, L"b.jpg").size(), 1u);
}

TEST(Redact, RedactModeNeedsACoverAndKeyChoosesTheKind)
{
    RedactTool tool({ L"a.png" }, RedactTool::Mode::Redact);
    tool.ImageReady(0, 400, 300);
    std::wstring why;
    EXPECT_FALSE(tool.CanApply(&why));
    EXPECT_FALSE(why.empty());
    tool.Key('2', false, false);   // blur
    tool.PointerDown({ { 10, 10 }, 6 });
    tool.PointerMove({ { 110, 60 }, 6 });
    tool.PointerUp({ { 110, 60 }, 6 });
    EXPECT_TRUE(tool.CanApply(nullptr));
    const auto covers = CoversFor(tool.Params(), L"a.png");
    ASSERT_EQ(covers.size(), 1u);
    EXPECT_EQ(covers[0].kind, CoverKind::Blur);
    EXPECT_EQ(covers[0].rect, (RectD{ 10, 10, 100, 50 }));
    // Undo snapshots restore the covers.
    const auto state = tool.State();
    tool.Key(VK_DELETE, false, false);
    EXPECT_FALSE(tool.CanApply(nullptr));
    tool.Restore(state);
    EXPECT_TRUE(tool.CanApply(nullptr));
}

// ---- The engine --------------------------------------------------------------------------------------

TEST(Redact, EngineBurnsTheCoversInAndKeepsNoMetadata)
{
    TempDir dir;
    Recipes recipes{ dir };
    std::wstring error;
    auto history = HistoryStore::Open(dir / L"history.db", error);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), history.get());
    const auto source = dir / L"photo.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(source, 200, 100, true));
    const auto other = dir / L"other.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(other, 50, 50, true));

    Recipe recipe = recipes.Get("redact");
    ASSERT_NE(runner.EngineFor(recipe), nullptr);
    EXPECT_TRUE(runner.EngineFor(recipe)->NeedsEditor(recipe));
    const std::vector<std::vector<CoverSpec>> covers = { { { { 20, 10, 40, 30 }, CoverKind::Solid, 0x000000 }, { { 120, 50, 60, 40 }, CoverKind::Blur, 0 } }, {} };
    recipe.steps[0].params = CoversToJson({ source, other }, covers);

    const auto runId = history->BeginRun(recipe, 2);
    const auto r = runner.Run(recipe, runId, source, {});
    const auto skipped = runner.Run(recipe, runId, other, {});
    history->FinishRun(runId);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    EXPECT_EQ(std::filesystem::path(r.output).filename(), L"photo (redacted).png");
    EXPECT_FALSE(r.message.empty());
    EXPECT_TRUE(skipped.skipped);   // nothing was drawn on it: no copy
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);

    Raster in, out;
    ASSERT_HRESULT_SUCCEEDED(LoadRaster(source, in, error));
    ASSERT_HRESULT_SUCCEEDED(LoadRaster(r.output, out, error));
    const auto px = ToPixelCovers(covers[0], 200, 100);
    EXPECT_GT(ChangedInside(in, out, px), 40u * 30u);
    for (UINT y = 10; y < 40; ++y)
        for (UINT x = 20; x < 60; ++x)
            ASSERT_EQ(out.At(x, y)[0] + out.At(x, y)[1] + out.At(x, y)[2], 0);
    EXPECT_TRUE(metadata::VerifyNoMetadata(r.output).clean);
}

// ---- Windows' text recognition and face detection (only where the WinRT provider is linked) ------------

TEST(Redact, VisionSaysWhenItIsUnavailable)
{
    if (vision::CanReadText())
        GTEST_SKIP() << "text recognition is available here";
    std::wstring error;
    Raster r;
    r.Allocate(10, 10);
    EXPECT_TRUE(vision::ReadText(r, &error).empty());
    EXPECT_FALSE(error.empty());
}

TEST(Redact, FaceDetectionFindsNoFaceInABlankPicture)
{
    if (!vision::CanFindFaces())
        GTEST_SKIP() << "Windows face detection is not available here";
    Raster r;
    r.Allocate(320, 240);
    std::fill(r.px.begin(), r.px.end(), uint8_t{ 200 });
    std::wstring error;
    EXPECT_TRUE(vision::FindFaces(r, &error).empty());
    EXPECT_TRUE(error.empty()) << Narrow(error);
}

#ifdef _MSC_VER
TEST(Redact, TextRecognitionReadsAnEmailAddress)
{
    if (!vision::CanReadText())
        GTEST_SKIP() << "no Windows text-recognition language here";
    // Draw the text with GDI: black on white, large and plain.
    const int w = 900, h = 160;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ASSERT_NE(bmp, nullptr);
    auto* old = SelectObject(dc, bmp);
    RECT all{ 0, 0, w, h };
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    HFONT font = CreateFontW(64, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Arial");
    auto* oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, 40, 40, L"jane.doe@example.com", 20);
    GdiFlush();
    Raster r;
    r.Allocate(w, h);
    std::memcpy(r.px.data(), bits, r.px.size());
    for (size_t i = 3; i < r.px.size(); i += 4)
        r.px[i] = 255;
    SelectObject(dc, oldFont);
    SelectObject(dc, old);
    DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);

    std::wstring error;
    const auto lines = vision::ReadText(r, &error);
    ASSERT_TRUE(error.empty()) << Narrow(error);
    ASSERT_FALSE(lines.empty());
    const auto hits = FindPii(lines);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].kind, PiiKind::Email);
    EXPECT_GT(hits[0].box.w, 300);   // where the text is, in the picture's pixels
    EXPECT_LT(hits[0].box.x, 120);
}
#endif
