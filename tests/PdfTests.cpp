#include "PdfTestUtil.h"
#include "PipelineRunner.h"
#include "pdf/ImagePdf.h"
#include "pdf/PageRenderer.h"
#include "pdf/PagePlan.h"
#include "pdf/PdfCompress.h"
#include "pdf/PdfContent.h"
#include "pdf/PdfDoc.h"
#include "pdf/PdfText.h"
#include "pdf/TextLayout.h"
#include "pdf/Wic.h"

#include <qpdf/QPDFPageDocumentHelper.hh>

#include <filesystem>

using namespace sd;
using namespace sd::pdf;
using namespace sd::test;

namespace
{
    std::unique_ptr<QPDF> Open(const std::wstring& path)
    {
        std::wstring error;
        auto pdf = OpenPdf(path, error);
        EXPECT_TRUE(pdf) << Narrow(error);
        return pdf;
    }

    std::vector<double> Box(QPDFPageObjectHelper& page)
    {
        std::vector<double> v;
        auto box = page.getMediaBox();
        for (int i = 0; i < box.getArrayNItems(); ++i)
            v.push_back(box.getArrayItem(i).getNumericValue());
        return v;
    }

    QPDFObjectHandle FirstImage(QPDFPageObjectHelper& page)
    {
        auto images = page.getImages();
        EXPECT_FALSE(images.empty());
        return images.empty() ? QPDFObjectHandle::newNull() : images.begin()->second;
    }

    // Image PDFs with `pages` pages, each `w` x `h` pixels at 72 dpi (so w x h points).
    std::wstring ImagesPdf(const TempDir& dir, const std::wstring& name, int pages, uint32_t w, uint32_t h, bool noise = false)
    {
        const auto path = dir / name;
        ImagePdfBuilder b(path);
        for (int i = 0; i < pages; ++i)
        {
            ImageSource src;
            src.pixels = Pattern(w + static_cast<uint32_t>(i) * 10, h, noise);
            src.dpiX = src.dpiY = 72;
            std::wstring error;
            EXPECT_TRUE(b.AddPage(src, nullptr, error)) << Narrow(error);
        }
        std::wstring error;
        EXPECT_TRUE(b.Finish(error)) << Narrow(error);
        return path;
    }

    std::wstring Text(const PageLines& lines)
    {
        std::wstring s;
        for (const auto& l : lines)
            s += l.text + L"|";
        return s;
    }

    uint64_t SizeOf(const std::wstring& path) { return std::filesystem::file_size(path); }
}

// ---- Writer -----------------------------------------------------------------------------------

TEST(PdfWriter, Numbers)
{
    EXPECT_EQ(Num(0), "0");
    EXPECT_EQ(Num(12.5), "12.5");
    EXPECT_EQ(Num(-3), "-3");
    EXPECT_EQ(Num(1.0 / 3.0), "0.3333");
    EXPECT_EQ(Num(-0.00001), "0");
    EXPECT_EQ(Num(612.0), "612");
    EXPECT_EQ(LiteralString("a(b)\\c"), "(a\\(b\\)\\\\c)");
    EXPECT_EQ(LiteralString("\xE9"), "(\\351)");
}

TEST(PdfWriter, WritesAFileQpdfReadsBack)
{
    TempDir dir;
    const auto path = dir / L"hand.pdf";
    WriteTextPdf(path, { "BT /F1 12 Tf 72 700 Td (Hello) Tj ET", "BT /F1 12 Tf 72 700 Td (World) Tj ET" });
    auto pdf = Open(path);
    ASSERT_TRUE(pdf);
    auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
    ASSERT_EQ(pages.size(), 2u);
    EXPECT_EQ(Box(pages[0]), (std::vector<double>{ 0, 0, 612, 792 }));
    EXPECT_EQ(pdf->getWarnings().size(), 0u) << "the xref table must be exact";
}

TEST(PdfWriter, PaethRowsHaveAFilterBytePerRow)
{
    const uint8_t px[] = { 1, 2, 3, 4, 5, 6 };   // 2 x 3, gray
    const auto rows = PaethRows(px, 2, 3, 1);
    ASSERT_EQ(rows.size(), 9u);
    EXPECT_EQ(rows[0], 4);
    EXPECT_EQ(rows[3], 4);
    EXPECT_EQ(static_cast<uint8_t>(rows[1]), 1);   // first pixel: no neighbours
    EXPECT_EQ(static_cast<uint8_t>(rows[2]), 1);   // 2 - left(1)
}

// ---- Images -> PDF ------------------------------------------------------------------------------

TEST(ImagePdf, JpegHeaders)
{
    JpegInfo info;
    EXPECT_FALSE(ParseJpeg("not a jpeg", info));
    EXPECT_FALSE(ParseJpeg(std::string("\xFF\xD8\xFF\xD9", 4), info));

    TempDir dir;
    const auto jpg = dir / L"a.jpg";
    ASSERT_HRESULT_SUCCEEDED(EncodeImageFile(Pattern(64, 48), jpg, true, 80, 200));
    ASSERT_TRUE(ParseJpeg(ReadAll(jpg), info));
    EXPECT_EQ(info.width, 64u);
    EXPECT_EQ(info.height, 48u);
    EXPECT_EQ(info.components, 3);
}

TEST(ImagePdf, OrientationPutsTheTopLeftCornerRight)
{
    // Where the stored image's top-left corner (u = 0, v = 1) lands on a W x H page.
    const double W = 200, H = 100;
    const std::pair<double, double> expected[9] = { {}, { 0, H }, { W, H }, { W, 0 }, { 0, 0 }, { 0, H }, { W, H }, { W, 0 }, { 0, 0 } };
    for (int o = 1; o <= 8; ++o)
    {
        const auto m = OrientationMatrix(o, W, H);
        EXPECT_DOUBLE_EQ(m[2] + m[4], expected[o].first) << "orientation " << o;
        EXPECT_DOUBLE_EQ(m[3] + m[5], expected[o].second) << "orientation " << o;
    }
    // 90° clockwise (6): the stored top-right corner goes to the bottom-right.
    const auto m6 = OrientationMatrix(6, W, H);
    EXPECT_DOUBLE_EQ(m6[0] + m6[2] + m6[4], W);
    EXPECT_DOUBLE_EQ(m6[1] + m6[3] + m6[5], 0);
}

TEST(ImagePdf, PageSizeFollowsResolution)
{
    double w = 0, h = 0;
    PageSizeFor(960, 480, 0, 0, w, h);   // no resolution: 96 dpi
    EXPECT_DOUBLE_EQ(w, 720);
    EXPECT_DOUBLE_EQ(h, 360);
    PageSizeFor(3000, 1500, 300, 300, w, h);
    EXPECT_DOUBLE_EQ(w, 720);
    PageSizeFor(100000, 50000, 72, 72, w, h);   // beyond 200 inches: scaled to fit
    EXPECT_DOUBLE_EQ(w, 14400);
    EXPECT_DOUBLE_EQ(h, 7200);
}

TEST(ImagePdf, PngBecomesLosslessPageWithMask)
{
    TempDir dir;
    const auto png = dir / L"t.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(png, 40, 30));   // left half transparent
    ImageSource src;
    std::wstring error;
    ASSERT_TRUE(LoadImageSource(png, src, error)) << Narrow(error);
    EXPECT_TRUE(src.jpeg.empty());
    const auto out = dir / L"t.pdf";
    {
        ImagePdfBuilder b(out);
        ASSERT_TRUE(b.AddPage(src, nullptr, error)) << Narrow(error);
        ASSERT_TRUE(b.Finish(error)) << Narrow(error);
    }
    auto pdf = Open(out);
    ASSERT_TRUE(pdf);
    auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
    ASSERT_EQ(pages.size(), 1u);
    auto image = FirstImage(pages[0]);
    ASSERT_TRUE(image.isStream());
    auto dict = image.getDict();
    EXPECT_EQ(dict.getKey("/Width").getIntValue(), 40);
    EXPECT_EQ(dict.getKey("/Filter").getName(), "/FlateDecode");
    ASSERT_TRUE(dict.getKey("/SMask").isStream());
    // QPDF undoes Flate and the PNG predictor: the samples are the picture's RGB values.
    const auto data = image.getStreamData(qpdf_dl_generalized);
    ASSERT_EQ(data->getSize(), 40u * 30u * 3u);
    const auto mask = dict.getKey("/SMask").getStreamData(qpdf_dl_generalized);
    ASSERT_EQ(mask->getSize(), 40u * 30u);
    EXPECT_EQ(mask->getBuffer()[0], 0);     // left half transparent
    EXPECT_EQ(mask->getBuffer()[39], 255);  // right half opaque
    const uint8_t* p = data->getBuffer() + (5 * 40 + 30) * 3;   // WritePng: R=128, G=y*255/h, B=x*255/w
    EXPECT_EQ(p[0], 128);
    EXPECT_EQ(p[1], 5 * 255 / 30);
    EXPECT_EQ(p[2], 30 * 255 / 40);
}

TEST(ImagePdf, JpegPassesThroughUntouched)
{
    TempDir dir;
    const auto jpg = dir / L"photo.jpg";
    ASSERT_HRESULT_SUCCEEDED(EncodeImageFile(Pattern(120, 80), jpg, true, 85, 300));
    ImageSource src;
    std::wstring error;
    ASSERT_TRUE(LoadImageSource(jpg, src, error)) << Narrow(error);
    ASSERT_FALSE(src.jpeg.empty());
    const auto out = dir / L"photo.pdf";
    {
        ImagePdfBuilder b(out);
        ASSERT_TRUE(b.AddPage(src, nullptr, error));
        ASSERT_TRUE(b.Finish(error));
    }
    auto pdf = Open(out);
    ASSERT_TRUE(pdf);
    auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
    auto image = FirstImage(pages[0]);
    EXPECT_EQ(image.getDict().getKey("/Filter").getName(), "/DCTDecode");
    const auto raw = image.getRawStreamData();
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(raw->getBuffer()), raw->getSize()), ReadAll(jpg));
    // 120 px at 300 dpi = 28.8 pt, when WIC wrote the resolution; 96 dpi = 90 pt otherwise.
    const double width = Box(pages[0])[2];
    EXPECT_TRUE(std::abs(width - 28.8) < 0.01 || std::abs(width - 90) < 0.01) << width;
}

TEST(ImagePdf, RunnerMakesOnePdfFromSeveralImages)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto a = dir / L"a.png", b = dir / L"b.jpg";
    ASSERT_HRESULT_SUCCEEDED(WritePng(a, 50, 50));
    ASSERT_HRESULT_SUCCEEDED(EncodeImageFile(Pattern(60, 40), b, true, 80, 96));
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    const auto& recipe = recipes.Get("images-to-pdf");
    EXPECT_TRUE(runner.Applies(recipe, a));
    EXPECT_FALSE(runner.Applies(recipe, dir / L"x.pdf"));
    const auto* engine = runner.EngineFor(recipe);
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->IsBatch(recipe));
    const auto result = runner.RunBatch(recipe, 0, { a, b }, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(std::filesystem::path(result.output).filename().wstring(), L"a (converted).pdf");
    auto pdf = Open(result.output);
    ASSERT_TRUE(pdf);
    EXPECT_EQ(pdf->getAllPages().size(), 2u);
}

// ---- Page plans ---------------------------------------------------------------------------------

TEST(PagePlan, MoveKeepsOrderOfTheSelection)
{
    std::vector<int> v{ 0, 1, 2, 3, 4, 5 };
    auto sel = MoveItems(v, { 4, 1 }, 0);
    EXPECT_EQ(v, (std::vector<int>{ 1, 4, 0, 2, 3, 5 }));
    EXPECT_EQ(sel, (std::vector<size_t>{ 0, 1 }));

    v = { 0, 1, 2, 3, 4, 5 };
    sel = MoveItems(v, { 0, 1 }, 6);   // to the end
    EXPECT_EQ(v, (std::vector<int>{ 2, 3, 4, 5, 0, 1 }));
    EXPECT_EQ(sel, (std::vector<size_t>{ 4, 5 }));

    v = { 0, 1, 2, 3, 4, 5 };
    sel = MoveItems(v, { 1 }, 4);   // before the item that was at 4
    EXPECT_EQ(v, (std::vector<int>{ 0, 2, 3, 1, 4, 5 }));
    EXPECT_EQ(sel, (std::vector<size_t>{ 3 }));

    v = { 0, 1, 2 };
    sel = MoveItems(v, { 1 }, 1);   // onto itself: nothing moves
    EXPECT_EQ(v, (std::vector<int>{ 0, 1, 2 }));
    EXPECT_EQ(sel, (std::vector<size_t>{ 1 }));

    sel = MoveItems(v, { 7, 2, 2 }, 0);   // out-of-range and repeated indices are ignored
    EXPECT_EQ(v, (std::vector<int>{ 2, 0, 1 }));
    EXPECT_EQ(sel, (std::vector<size_t>{ 0 }));
}

TEST(PagePlan, RotateDuplicateRemove)
{
    PagePlan plan = AllPages({ 2, 1 });
    ASSERT_EQ(plan.size(), 3u);
    EXPECT_EQ(plan[2], (PageRef{ 1, 0, 0 }));

    RotatePages(plan, { 0, 2 }, -90);
    EXPECT_EQ(plan[0].rotation, 270);
    RotatePages(plan, { 0 }, 180);
    EXPECT_EQ(plan[0].rotation, 90);

    const auto copies = DuplicatePages(plan, { 2, 0 });
    ASSERT_EQ(plan.size(), 5u);
    EXPECT_EQ(copies, (std::vector<size_t>{ 1, 4 }));
    EXPECT_EQ(plan[1], plan[0]);
    EXPECT_EQ(plan[4], plan[3]);

    auto focus = RemoveItems(plan, { 1, 4 });
    EXPECT_EQ(plan.size(), 3u);
    EXPECT_EQ(focus, 1);
    focus = RemoveItems(plan, { 2 });
    EXPECT_EQ(focus, 1);   // the last one went: focus the new last
    focus = RemoveItems(plan, { 0, 1 });
    EXPECT_EQ(focus, -1);
}

TEST(PagePlan, JsonRoundTripAndChecks)
{
    const PagePlan plan{ { 1, 0, 90 }, { 0, 3, 0 } };
    PagePlan back;
    std::string error;
    ASSERT_TRUE(PlanFromJson(PlanToJson(plan), back, error)) << error;
    EXPECT_EQ(back, plan);
    EXPECT_FALSE(PlanFromJson(nlohmann::json::parse(R"([[0, 1, 45]])"), back, error));
    EXPECT_FALSE(PlanFromJson(nlohmann::json::parse(R"([[0, 1]])"), back, error));
    EXPECT_FALSE(PlanFromJson(nlohmann::json::parse(R"({"a": 1})"), back, error));
    EXPECT_TRUE(CheckPlan(plan, { 4, 1 }, error));
    EXPECT_FALSE(CheckPlan(plan, { 3, 1 }, error));   // page 4 of file 1 does not exist
    EXPECT_FALSE(CheckPlan({}, { 3 }, error));
}

TEST(PdfPages, MergeAndOrganize)
{
    TempDir dir;
    const auto a = ImagesPdf(dir, L"a.pdf", 2, 100, 50);   // pages 100 and 110 pt wide
    const auto b = ImagesPdf(dir, L"b.pdf", 3, 200, 80);   // 200, 210, 220
    std::vector<uint32_t> counts;
    std::wstring error;
    ASSERT_TRUE(CountPages({ a, b }, counts, error)) << Narrow(error);
    EXPECT_EQ(counts, (std::vector<uint32_t>{ 2, 3 }));

    const auto merged = dir / L"merged.pdf";
    ASSERT_TRUE(AssemblePages({ a, b }, AllPages(counts), merged, {}, {}, error)) << Narrow(error);
    {
        auto pdf = Open(merged);
        ASSERT_TRUE(pdf);
        auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
        ASSERT_EQ(pages.size(), 5u);
        EXPECT_EQ(Box(pages[0])[2], 100);
        EXPECT_EQ(Box(pages[2])[2], 200);
        EXPECT_EQ(Box(pages[4])[2], 220);
    }

    // Organize: b's last page turned, a's first page twice (the copy turned upside down), the rest dropped.
    const auto organized = dir / L"organized.pdf";
    const PagePlan plan{ { 1, 2, 90 }, { 0, 0, 0 }, { 0, 0, 180 } };
    double last = 0;
    ASSERT_TRUE(AssemblePages({ a, b }, plan, organized, {}, [&](double f) { EXPECT_GE(f, last); last = f; }, error)) << Narrow(error);
    EXPECT_DOUBLE_EQ(last, 1.0);
    auto pdf = Open(organized);
    ASSERT_TRUE(pdf);
    auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
    ASSERT_EQ(pages.size(), 3u);
    EXPECT_EQ(Box(pages[0])[2], 220);
    EXPECT_EQ(pages[0].getAttribute("/Rotate", false).getIntValue(), 90);
    EXPECT_EQ(Box(pages[1])[2], 100);
    EXPECT_EQ(Box(pages[2])[2], 100);
    EXPECT_EQ(pages[2].getAttribute("/Rotate", false).getIntValue(), 180);
    EXPECT_NE(pages[1].getObjectHandle().getObjGen(), pages[2].getObjectHandle().getObjGen());
}

TEST(PdfPages, StopRequestCancels)
{
    TempDir dir;
    const auto a = ImagesPdf(dir, L"a.pdf", 2, 100, 50);
    std::stop_source stop;
    stop.request_stop();
    std::wstring error;
    EXPECT_FALSE(AssemblePages({ a }, AllPages({ 2 }), dir / L"out.pdf", stop.get_token(), {}, error));
    EXPECT_EQ(error, kCancelled);
}

TEST(PdfPages, RunnerMergeNeedsTwoAndNamesTheOutput)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto a = ImagesPdf(dir, L"first.pdf", 1, 100, 50);
    const auto b = ImagesPdf(dir, L"second.pdf", 2, 100, 50);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    const auto& merge = recipes.Get("pdf-merge");
    EXPECT_TRUE(runner.Applies(merge, a));
    EXPECT_FALSE(runner.Applies(merge, dir / L"x.png"));
    EXPECT_FALSE(runner.RunBatch(merge, 0, { a }, {}).ok);
    const auto result = runner.RunBatch(merge, 0, { b, a }, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(std::filesystem::path(result.output).filename().wstring(), L"second (merged).pdf");
    EXPECT_EQ(result.message, L"Merged 2 PDFs into 3 pages");
    auto pdf = Open(result.output);
    EXPECT_EQ(pdf->getAllPages().size(), 3u);

    // A renamed text file is not a PDF: the wheel says so.
    const auto fake = dir / L"fake.pdf";
    WriteText(fake, "hello");
    std::wstring why;
    EXPECT_FALSE(runner.Accepts(merge, fake, &why));
    EXPECT_EQ(why, L"not a PDF file");
}

TEST(PdfPages, OrganizeUsesThePlanFromTheEditor)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto a = ImagesPdf(dir, L"doc.pdf", 3, 100, 50);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    Recipe organize = recipes.Get("pdf-organize");
    organize.steps[0].params["plan"] = PlanToJson({ { 0, 2, 0 }, { 0, 0, 270 } });
    const auto result = runner.RunBatch(organize, 0, { a }, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(std::filesystem::path(result.output).filename().wstring(), L"doc (organized).pdf");
    auto pdf = Open(result.output);
    auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
    ASSERT_EQ(pages.size(), 2u);
    EXPECT_EQ(Box(pages[0])[2], 120);
    EXPECT_EQ(pages[1].getAttribute("/Rotate", false).getIntValue(), 270);

    organize.steps[0].params["plan"] = PlanToJson({ { 0, 7, 0 } });
    EXPECT_FALSE(runner.RunBatch(organize, 0, { a }, {}).ok);
}

// ---- Compress -----------------------------------------------------------------------------------

TEST(PdfCompress, TargetSize)
{
    uint32_t w = 0, h = 0;
    TargetSize(3000, 2000, nullptr, 150, w, h);   // nowhere drawn: unchanged
    EXPECT_EQ(w, 3000u);
    const Placement inch{ 72, 48 };              // drawn 1 x 0.667 inch
    TargetSize(3000, 2000, &inch, 150, w, h);
    EXPECT_EQ(w, 150u);
    EXPECT_EQ(h, 100u);
    TargetSize(160, 100, &inch, 150, w, h);      // barely above target: not worth resampling
    EXPECT_EQ(w, 160u);
    TargetSize(100, 60, &inch, 150, w, h);       // below target: never enlarged
    EXPECT_EQ(w, 100u);
}

TEST(PdfCompress, ShrinksPicturesAndNeverKeepsABiggerFile)
{
    TempDir dir;
    Recipes recipes(dir);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    const auto& compress = recipes.Get("pdf-compress");

    // Noisy pixels stored losslessly, drawn at 2 x 2 inches but stored at 600 px: big.
    const auto big = dir / L"big.pdf";
    {
        ImagePdfBuilder b(big);
        ImageSource src;
        src.pixels = Pattern(600, 600, true);
        src.dpiX = src.dpiY = 300;
        std::wstring error;
        ASSERT_TRUE(b.AddPage(src, nullptr, error));
        ASSERT_TRUE(b.Finish(error));
    }
    const auto result = runner.Run(compress, 0, big, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(std::filesystem::path(result.output).filename().wstring(), L"big (compressed).pdf");
    EXPECT_LT(SizeOf(result.output), SizeOf(big) / 2);
    {
        auto pdf = Open(result.output);
        auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
        auto image = FirstImage(pages[0]);
        EXPECT_EQ(image.getDict().getKey("/Filter").getName(), "/DCTDecode");
        EXPECT_EQ(image.getDict().getKey("/Width").getIntValue(), 300);   // 2 inches at 150 dpi
    }

    // A tiny PDF of text: nothing to gain, so no "(compressed)" file is left behind.
    const auto text = dir / L"text.pdf";
    WriteTextPdf(text, { "BT /F1 12 Tf 72 700 Td (Hi) Tj ET" });
    const auto same = runner.Run(compress, 0, text, {});
    EXPECT_FALSE(same.ok);
    EXPECT_TRUE(same.skipped);
    EXPECT_FALSE(std::filesystem::exists(dir / L"text (compressed).pdf"));
    for (const auto& e : std::filesystem::directory_iterator(dir.path()))
        EXPECT_FALSE(e.path().wstring().ends_with(L".sdpart")) << e.path();
}

TEST(PdfCompress, ReduceSizeStepsDownToTheTarget)
{
    TempDir dir;
    Recipes recipes(dir);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    const auto big = dir / L"big.pdf";
    {
        ImagePdfBuilder b(big);
        ImageSource src;
        src.pixels = Pattern(600, 600, true);
        src.dpiX = src.dpiY = 300;
        std::wstring error;
        ASSERT_TRUE(b.AddPage(src, nullptr, error));
        ASSERT_TRUE(b.Finish(error));
    }
    const auto& reduce = recipes.Get("reduce-pdf-90");
    EXPECT_EQ(reduce.ring, "reduce");
    const auto result = runner.Run(reduce, 0, big, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(std::filesystem::path(result.output).filename().wstring(), L"big (reduced).pdf");
    EXPECT_LE(SizeOf(result.output), SizeOf(big) / 10);
    for (const auto& e : std::filesystem::directory_iterator(dir.path()))
        EXPECT_FALSE(e.path().wstring().ends_with(L".sdpart")) << e.path();
}

// ---- Text ---------------------------------------------------------------------------------------

TEST(PdfText, GlyphNames)
{
    EXPECT_EQ(GlyphNameToUnicode("A"), L"A");
    EXPECT_EQ(GlyphNameToUnicode("eacute"), L"é");
    EXPECT_EQ(GlyphNameToUnicode("quoteright"), L"’");
    EXPECT_EQ(GlyphNameToUnicode("uni00E9"), L"é");
    EXPECT_EQ(GlyphNameToUnicode("uni00660069"), L"fi");
    EXPECT_EQ(GlyphNameToUnicode("u1F600"), L"\U0001F600");
    EXPECT_EQ(GlyphNameToUnicode("f_i"), L"fi");
    EXPECT_EQ(GlyphNameToUnicode("a.sc"), L"a");
    EXPECT_EQ(GlyphNameToUnicode("g123"), L"");
}

TEST(PdfText, ToUnicodeCMaps)
{
    const char* cmap = R"(/CIDInit /ProcSet findresource begin
12 dict begin begincmap
1 begincodespacerange <0000> <FFFF> endcodespacerange
2 beginbfchar
<0003> <0020>
<0011> <00660069>
endbfchar
2 beginbfrange
<0024> <0026> <0041>
<0030> <0031> [<0061> <D83DDE00>]
endbfrange
endcmap)";
    int bytes = 0;
    const auto map = ParseToUnicode(cmap, bytes);
    EXPECT_EQ(bytes, 2);
    EXPECT_EQ(map.at(0x03), L" ");
    EXPECT_EQ(map.at(0x11), L"fi");
    EXPECT_EQ(map.at(0x24), L"A");
    EXPECT_EQ(map.at(0x26), L"C");
    EXPECT_EQ(map.at(0x30), L"a");
    EXPECT_EQ(map.at(0x31), L"\U0001F600");
    EXPECT_EQ(map.count(0x27), 0u);
}

TEST(PdfText, ExtractsLinesWithSpacesAndSizes)
{
    TempDir dir;
    const auto path = dir / L"t.pdf";
    WriteTextPdf(path, { "BT /F1 24 Tf 72 700 Td (Big Title) Tj ET\n"
                         "BT /F1 12 Tf 72 650 Td (Hello) Tj ( world) Tj 0 -14 Td [(sep)-300(arated)] TJ ET\n"
                         "BT /F1 12 Tf 72 600 Td (caf\\351) Tj ET" });
    auto pdf = Open(path);
    ASSERT_TRUE(pdf);
    auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
    const auto lines = ExtractPageLines(pages[0]);
    EXPECT_EQ(Text(lines), L"Big Title|Hello world|sep arated|café|");
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_NEAR(lines[0].size, 24, 0.01);
    EXPECT_NEAR(lines[1].size, 12, 0.01);
    EXPECT_NEAR(lines[1].y, 650, 0.01);
    EXPECT_NEAR(lines[2].y, 636, 0.01);
}

TEST(PdfText, SidewaysTextIsTurnedToReadingOrder)
{
    TempDir dir;
    const auto path = dir / L"rot.pdf";
    // Text running up the page (rotated 90° counter-clockwise), two lines.
    WriteTextPdf(path, { "BT /F1 12 Tf 0 1 -1 0 300 100 Tm (First line) Tj 0 -14 Td (Second line) Tj ET" });
    auto pdf = Open(path);
    auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
    const auto lines = ExtractPageLines(pages[0]);
    EXPECT_EQ(Text(lines), L"First line|Second line|");
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_GT(lines[0].y, lines[1].y);   // the second line is below the first in reading order
}

TEST(PdfText, ReadPdfTextWithoutOcr)
{
    TempDir dir;
    const auto path = dir / L"two.pdf";
    WriteTextPdf(path, { "BT /F1 12 Tf 72 700 Td (Page one text) Tj ET", "" });
    PdfText text;
    std::wstring error;
    ASSERT_TRUE(ReadPdfText(path, false, {}, {}, text, error)) << Narrow(error);
    ASSERT_EQ(text.pages.size(), 2u);
    EXPECT_EQ(Text(text.pages[0]), L"Page one text|");
    EXPECT_TRUE(text.pages[1].empty());
    EXPECT_EQ(text.ocrPages, 0u);
}

TEST(PdfText, RunnerWritesTxtAndMarkdown)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto path = dir / L"notes.pdf";
    WriteTextPdf(path, { "BT /F1 24 Tf 72 700 Td (Notes) Tj ET\n"
                         "BT /F1 12 Tf 72 660 Td (A first para-) Tj 0 -14 Td (graph here.) Tj ET\n"
                         "BT /F1 12 Tf 72 610 Td (\\225 One item) Tj ET" });
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    const auto md = runner.Run(recipes.Get("pdf-to-md"), 0, path, {});
    ASSERT_TRUE(md.ok) << Narrow(md.error);
    EXPECT_EQ(std::filesystem::path(md.output).filename().wstring(), L"notes (converted).md");
    EXPECT_EQ(ReadAll(md.output), "# Notes\n\nA first paragraph here.\n\n- One item\n");
    const auto txt = runner.Run(recipes.Get("pdf-to-txt"), 0, path, {});
    ASSERT_TRUE(txt.ok) << Narrow(txt.error);
    EXPECT_EQ(ReadAll(txt.output), "Notes\r\n\r\nA first para-\r\ngraph here.\r\n\r\n\xE2\x80\xA2 One item\r\n");
}

TEST(TextLayout, JoinSpans)
{
    std::vector<TextSpan> spans{
        { 10, 100, 40, 10, L"Hello" }, { 42, 100, 70, 10, L"world" },   // gap 2 pt > 0.15 em: a space
        { 70.5, 100, 80, 10, L"!" },                                  // touching: no space
        { 10, 86, 50, 10, L"Next line" },                             // lower baseline: new line
        { 10, 86, 50, 10, L"Again" },                                 // same baseline but jumps back left
    };
    const auto lines = JoinSpans(spans);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0].text, L"Hello world!");
    EXPECT_EQ(lines[1].text, L"Next line");
    EXPECT_EQ(lines[2].text, L"Again");
}

TEST(TextLayout, MarkdownHeadingsParagraphsAndLists)
{
    PageLines page{
        { 72, 740, 300, 28, L"Big Heading" },
        { 72, 700, 300, 16, L"Smaller heading" },
        { 72, 670, 500, 11, L"Body text that wraps onto" },
        { 72, 656, 500, 11, L"a second line, with a hy-" },
        { 72, 642, 500, 11, L"phenated word." },
        { 72, 610, 500, 11, L"Second paragraph." },
        { 72, 590, 500, 11, L"• first" },
        { 72, 576, 500, 11, L"- second" },
        { 72, 550, 500, 11, L"# not a heading" },
    };
    EXPECT_EQ(Narrow(ToMarkdown({ page })),
              "# Big Heading\n\n## Smaller heading\n\nBody text that wraps onto a second line, with a hyphenated word.\n\n"
              "Second paragraph.\n\n- first\n- second\n\n\\# not a heading\n");
    // Plain text keeps the line breaks and puts blank lines between paragraphs and pages.
    PageLines second{ { 72, 700, 100, 11, L"Page two" } };
    const auto text = Narrow(ToText({ page, second }));
    EXPECT_NE(text.find("Body text that wraps onto\r\na second line, with a hy-\r\nphenated word.\r\n\r\nSecond paragraph."), std::string::npos);
    EXPECT_TRUE(text.ends_with("# not a heading\r\n\r\nPage two\r\n"));
    EXPECT_EQ(ToMarkdown({}), L"");
    EXPECT_EQ(VisibleChars(second), 7u);
}

// ---- Windows.Data.Pdf (runs where the renderer is compiled in: the MSVC build) -------------------

TEST(PdfRender, RendersAPage)
{
    if (!RenderingAvailable())
        GTEST_SKIP() << "Windows.Data.Pdf is not compiled into this build";
    TempDir dir;
    const auto path = dir / L"r.pdf";
    WriteTextPdf(path, { "0 0 0 rg 100 100 200 200 re f" });
    RunMta([&] {
        std::wstring error;
        auto doc = OpenForRendering(path, error);
        ASSERT_TRUE(doc) << Narrow(error);
        EXPECT_EQ(doc->PageCount(), 1u);
        double w = 0, h = 0;
        ASSERT_TRUE(doc->PageSize(0, w, h));
        EXPECT_NEAR(w, 612, 1);
        EXPECT_NEAR(h, 792, 1);
        Bitmap bitmap;
        ASSERT_TRUE(doc->Render(0, 306, 396, bitmap, error)) << Narrow(error);
        ASSERT_EQ(bitmap.width, 306u);
        ASSERT_EQ(bitmap.height, 396u);
        // The black square covers (100..300, 100..300) pt from the bottom: at half scale, pixel
        // (100, 300) from the top is inside it; the corner is white paper.
        const uint8_t* inside = &bitmap.bgra[(300u * 306u + 100u) * 4];
        const uint8_t* corner = &bitmap.bgra[(5u * 306u + 5u) * 4];
        EXPECT_LT(inside[0], 60);
        EXPECT_GT(corner[0], 200);
    });
}

TEST(PdfRender, RunnerSavesEveryPageInAFolder)
{
    if (!RenderingAvailable())
        GTEST_SKIP() << "Windows.Data.Pdf is not compiled into this build";
    TempDir dir;
    Recipes recipes(dir);
    const auto path = dir / L"deck.pdf";
    WriteTextPdf(path, { "BT /F1 40 Tf 72 700 Td (One) Tj ET", "BT /F1 40 Tf 72 700 Td (Two) Tj ET" });
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    ItemResult result;
    RunMta([&] { result = runner.Run(recipes.Get("pdf-to-png"), 0, path, {}); });
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(std::filesystem::path(result.output).filename().wstring(), L"deck (pages)");
    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(result.output + L"\\deck page 02.png", w, h));
    EXPECT_EQ(w, 2550u);   // 8.5 in at 300 dpi
    EXPECT_EQ(h, 3300u);
}
