#include "PathUtil.h"
#include "PdfTestUtil.h"
#include "PipelineRunner.h"
#include "canvas/Covers.h"
#include "pdf/ImagePdf.h"
#include "pdf/PageEdits.h"
#include "pdf/PageRenderer.h"
#include "pdf/PagePlan.h"
#include "pdf/PdfCompress.h"
#include "pdf/PdfContent.h"
#include "pdf/PdfDoc.h"
#include "pdf/PdfText.h"
#include "pdf/ScrollList.h"
#include "pdf/TextLayout.h"
#include "pdf/Wic.h"
#include "redact/RedactTool.h"

#include <qpdf/QPDFPageDocumentHelper.hh>

#include <chrono>
#include <climits>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

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
    EXPECT_EQ(FileName(result.output), L"a (converted).pdf");
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
    EXPECT_EQ(FileName(result.output), L"second (merged).pdf");
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
    EXPECT_EQ(FileName(result.output), L"doc (organized).pdf");
    auto pdf = Open(result.output);
    auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
    ASSERT_EQ(pages.size(), 2u);
    EXPECT_EQ(Box(pages[0])[2], 120);
    EXPECT_EQ(pages[1].getAttribute("/Rotate", false).getIntValue(), 270);

    organize.steps[0].params["plan"] = PlanToJson({ { 0, 7, 0 } });
    EXPECT_FALSE(runner.RunBatch(organize, 0, { a }, {}).ok);
}

// ---- The page view: Merge's More..., Edit PDF ---------------------------------------------------

namespace
{
    // Everything QPDF finds in a PDF, objects and decoded streams, for "is this text anywhere in it".
    std::string AllContent(const std::wstring& path)
    {
        auto pdf = Open(path);
        std::string all;
        if (!pdf)
            return all;
        for (auto& obj : pdf->getAllObjects())
        {
            if (obj.isStream())
            {
                all += obj.getDict().unparse();
                try
                {
                    auto data = obj.getStreamData(qpdf_dl_all);
                    all.append(reinterpret_cast<const char*>(data->getBuffer()), data->getSize());
                }
                catch (const std::exception&)
                {
                    auto raw = obj.getRawStreamData();
                    all.append(reinterpret_cast<const char*>(raw->getBuffer()), raw->getSize());
                }
            }
            else
                all += obj.unparse();
            all += "\n";
        }
        return all;
    }

    std::string ContentsOf(QPDFPageObjectHelper& page)
    {
        auto data = page.getObjectHandle().getKey("/Contents").getStreamData(qpdf_dl_all);
        return std::string(reinterpret_cast<const char*>(data->getBuffer()), data->getSize());
    }

    // Three Letter pages saying SECRETONE, SECRETTWO and SECRETTHREE. Page 2 also has a bookmark
    // pointing at it, a filled-in form field and a tag with alternate text quoting it.
    std::wstring SecretsPdf(const TempDir& dir, const std::wstring& name)
    {
        const auto path = dir / name;
        PdfWriter w(path);
        const int catalog = w.NewObject(), pages = w.NewObject(), font = w.NewObject();
        const int outlines = w.NewObject(), bookmark = w.NewObject(), field = w.NewObject(), tags = w.NewObject();
        w.Object(font, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
        const char* words[] = { "SECRETONE", "SECRETTWO", "SECRETTHREE" };
        std::string kids;
        int second = 0;
        for (int i = 0; i < 3; ++i)
        {
            const int c = w.NewObject(), page = w.NewObject();
            w.Stream(c, "", std::format("BT /F1 24 Tf 72 700 Td ({}) Tj ET", words[i]));
            w.Object(page, std::format("<< /Type /Page /Parent {} 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 {} 0 R >> >> /Contents {} 0 R{} >>",
                                       pages, font, c, i == 1 ? std::format(" /Annots [{} 0 R] /StructParents 0", field) : std::string()));
            kids += std::format(" {} 0 R", page);
            if (i == 1)
                second = page;
        }
        w.Object(field, std::format("<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /V (SECRETFIELD) /Rect [72 600 300 630] /P {} 0 R >>", second));
        w.Object(outlines, std::format("<< /Type /Outlines /First {0} 0 R /Last {0} 0 R /Count 1 >>", bookmark));
        w.Object(bookmark, std::format("<< /Title (Chapter) /Parent {} 0 R /Dest [{} 0 R /Fit] >>", outlines, second));
        w.Object(tags, std::format("<< /Type /StructTreeRoot /K << /S /P /Pg {} 0 R /Alt (SECRETALT) >> >>", second));
        w.Object(pages, std::format("<< /Type /Pages /Kids [{} ] /Count 3 >>", kids));
        w.Object(catalog, std::format("<< /Type /Catalog /Pages {} 0 R /Outlines {} 0 R /AcroForm << /Fields [{} 0 R] >> /StructTreeRoot {} 0 R "
                                      "/MarkInfo << /Marked true >> >>",
                                      pages, outlines, field, tags));
        EXPECT_TRUE(w.Finish(catalog, 0));
        return path;
    }

    // A redacted copy of a Letter page: a picture with a black box over (100, 100, 200 x 50) px,
    // made by the same Redact the page view uses, wrapped as the plan's one-page PDF.
    std::wstring RedactedPage(const TempDir& dir, const std::wstring& name)
    {
        const auto picture = dir / (name + L" page.png"), redacted = dir / (name + L".png"), page = dir / (name + L".pdf");
        EXPECT_HRESULT_SUCCEEDED(EncodeImageFile(Pattern(306, 396), picture, false, 0, 36));
        bool changed = false;
        std::wstring error;
        const auto params = redact::CoversToJson({ picture }, { { { { 100, 100, 200, 50 }, canvas::CoverKind::Solid, 0x000000 } } });
        EXPECT_TRUE(RenderPageEdit(PageTool::Redact, params, picture, redacted, changed, error)) << Narrow(error);
        EXPECT_TRUE(changed);
        EXPECT_TRUE(WritePicturePage(redacted, 2.0, page, error)) << Narrow(error);
        return page;
    }
}

TEST(PagePlan, AllPagesInListOrder)
{
    EXPECT_EQ(AllPages({ 2, 1 }, { 1, 0 }), (PagePlan{ { 1, 0, 0 }, { 0, 0, 0 }, { 0, 1, 0 } }));
    EXPECT_EQ(AllPages({ 2, 1 }, { 1, 5 }), (PagePlan{ { 1, 0, 0 } }));   // unknown files are skipped
}

TEST(PagePlan, ReorderAcrossFilesDeleteRotateAndRevert)
{
    // A has 2 pages, B 3. Move B's first page to the front, turn it, leave out A's second page.
    PagePlan plan = AllPages({ 2, 3 }, { 0, 1 });
    auto moved = MoveItems(plan, { 2 }, 0);
    EXPECT_EQ(moved, (std::vector<size_t>{ 0 }));
    RotatePages(plan, moved, 90);
    RemoveItems(plan, { 2 });
    EXPECT_EQ(plan, (PagePlan{ { 1, 0, 90 }, { 0, 0, 0 }, { 1, 1, 0 }, { 1, 2, 0 } }));
    EXPECT_FALSE(FilesStayTogether(plan));
    EXPECT_EQ(PagesKept(plan, 0), 1u);
    EXPECT_EQ(PagesKept(plan, 1), 3u);

    // An edited page keeps its place and turn; Revert gives back the original.
    plan[2].edited = L"C:\\edits\\edit 1-1.pdf";
    EXPECT_EQ(EditedPages(plan), 1u);
    PagePlan back;
    std::string error;
    ASSERT_TRUE(PlanFromJson(PlanToJson(plan), back, error)) << error;
    EXPECT_EQ(back, plan);
    EXPECT_EQ(PlanToJson(plan)[2].size(), 4u);
    EXPECT_EQ(PlanToJson(plan)[1].size(), 3u);
    EXPECT_FALSE(PlanFromJson(nlohmann::json::parse(R"([[0, 1, 0, ""]])"), back, error));
    EXPECT_FALSE(PlanFromJson(nlohmann::json::parse(R"([[0, 1, 0, 7]])"), back, error));
    RevertPages(plan, { 2, 9 });
    EXPECT_TRUE(plan[2].edited.empty());
    EXPECT_EQ(EditedPages(plan), 0u);
}

TEST(PagePlan, BackToTheListKeepsOrRegroupsTheEdits)
{
    // A (file 0) 2 pages, B (file 1) 2 pages; A's second page turned and B's first edited.
    PagePlan plan = AllPages({ 2, 2 }, { 0, 1 });
    plan[1].rotation = 180;
    plan[2].edited = L"x.pdf";
    bool regrouped = true;

    // The same list: nothing changes.
    EXPECT_EQ(PlanForFileOrder(plan, { 0, 1 }, { 0, 1 }, &regrouped), plan);
    EXPECT_FALSE(regrouped);

    // Files swapped: each file's pages follow it, with their turns and edits.
    auto swapped = PlanForFileOrder(plan, { 0, 1 }, { 1, 0 }, &regrouped);
    EXPECT_EQ(swapped, (PagePlan{ { 1, 0, 0, L"x.pdf" }, { 1, 1, 0 }, { 0, 0, 0 }, { 0, 1, 180 } }));
    EXPECT_FALSE(regrouped);

    // A file left out of the list: its pages go, the rest keep their order.
    EXPECT_EQ(PlanForFileOrder(plan, { 0, 1 }, { 1 }, &regrouped), (PagePlan{ { 1, 0, 0, L"x.pdf" }, { 1, 1, 0 } }));
    EXPECT_FALSE(regrouped);

    // Pages mixed across files, then the files swapped: each goes back to its own file (and the
    // window is told, so it can say so).
    PagePlan mixed{ { 0, 0, 0 }, { 1, 0, 0, L"x.pdf" }, { 0, 1, 180 }, { 1, 1, 0 } };
    EXPECT_EQ(PlanForFileOrder(mixed, { 0, 1 }, { 1, 0 }, &regrouped), (PagePlan{ { 1, 0, 0, L"x.pdf" }, { 1, 1, 0 }, { 0, 0, 0 }, { 0, 1, 180 } }));
    EXPECT_TRUE(regrouped);

    // To the engine: file indices counted in list order.
    EXPECT_EQ(InListOrder(swapped, { 1, 0 }), (PagePlan{ { 0, 0, 0, L"x.pdf" }, { 0, 1, 0 }, { 1, 0, 0 }, { 1, 1, 180 } }));
    EXPECT_EQ(InListOrder(swapped, { 1 }), (PagePlan{ { 0, 0, 0, L"x.pdf" }, { 0, 1, 0 } }));
}

namespace
{
    // Pages of 612 x 792 points, each painted one grey (darker for later pages); "missing.pdf" can't
    // be opened. Stands in for Windows.Data.Pdf so the page view's render queue runs anywhere.
    class FakeRenderedPdf final : public IRenderedPdf
    {
    public:
        uint32_t PageCount() const override { return 3; }
        bool PageSize(uint32_t, double& widthPt, double& heightPt) const override
        {
            widthPt = 612;
            heightPt = 792;
            return true;
        }
        bool Render(uint32_t page, uint32_t widthPx, uint32_t heightPx, Bitmap& out, std::wstring&) const override
        {
            out.width = widthPx;
            out.height = heightPx;
            out.bgra.assign(static_cast<size_t>(widthPx) * heightPx * 4, static_cast<uint8_t>(200 - page * 40));
            return true;
        }
    };

    std::unique_ptr<IRenderedPdf> OpenFake(const std::wstring& path, std::wstring& error)
    {
        if (path == L"missing.pdf")
        {
            error = L"no such file";
            return nullptr;
        }
        return std::make_unique<FakeRenderedPdf>();
    }

    // What a render queue hands over, from its worker.
    struct Delivered
    {
        std::mutex mutex;
        std::condition_variable arrived;
        std::vector<RenderedPage> pages;

        RenderQueue::Target Target(const std::shared_ptr<Delivered>& self)
        {
            return [self](std::unique_ptr<RenderedPage> page) {
                {
                    std::scoped_lock lock(self->mutex);
                    self->pages.push_back(std::move(*page));
                }
                self->arrived.notify_all();
                return true;
            };
        }
        bool WaitFor(size_t count)
        {
            std::unique_lock lock(mutex);
            return arrived.wait_for(lock, std::chrono::seconds(10), [&] { return pages.size() >= count; });
        }
        RenderedPage At(size_t i)
        {
            std::scoped_lock lock(mutex);
            return pages.at(i);
        }
    };
}

TEST(PageRender, PicturesAreKeptPerFileAndEdit)
{
    const std::vector<std::wstring> files{ L"C:\\a.pdf", L"C:\\b.pdf" };
    PageRef ref{ 1, 3, 90 };
    // A page of a file: that file's page, whatever its turn (the view turns the picture).
    EXPECT_EQ(KeyOf(ref, files), (PageKey{ L"C:\\b.pdf", 3 }));
    EXPECT_EQ(KeyOf({ 1, 3, 0 }, files), KeyOf(ref, files));
    EXPECT_NE(KeyOf({ 0, 3, 0 }, files), KeyOf(ref, files));
    // Edited: its own one-page PDF, so the edit shows, not the original's picture.
    ref.edited = L"C:\\edits\\edit 1-1.pdf";
    EXPECT_EQ(KeyOf(ref, files), (PageKey{ L"C:\\edits\\edit 1-1.pdf", 0 }));
    // Edited again: a new file and a new key, so the second edit is rendered too.
    PageRef again = ref;
    again.edited = L"C:\\edits\\edit 2-1.pdf";
    EXPECT_NE(KeyOf(again, files), KeyOf(ref, files));
    // Reverted: back to the original's key (and its picture, already rendered).
    PagePlan plan{ again };
    RevertPages(plan, { 0 });
    EXPECT_EQ(KeyOf(plan[0], files), (PageKey{ L"C:\\b.pdf", 3 }));
    // A file the plan does not know: nothing to render.
    EXPECT_TRUE(KeyOf({ 5, 0, 0 }, files).path.empty());
}

TEST(PageRender, PreviewIsRenderedAtTheScreensPixels)
{
    const double letter = 612.0 / 792.0;
    // A Letter page in a 400 x 600 DIP box: as wide as the box, at 100%, 150% and 200%.
    EXPECT_EQ(PreviewSide(letter, 0, 400, 600, 1.0), 518u);   // 400 / letter = 517.6 DIPs tall
    EXPECT_EQ(PreviewSide(letter, 0, 400, 600, 1.5), 776u);
    EXPECT_EQ(PreviewSide(letter, 0, 400, 600, 2.0), 1035u);
    // Turned a quarter: it lies on its side, as tall as the box is wide.
    EXPECT_EQ(PreviewSide(letter, 90, 400, 600, 1.5), 600u);
    EXPECT_EQ(PreviewSide(letter, 270, 400, 600, 1.5), 600u);
    EXPECT_EQ(PreviewSide(letter, 180, 400, 600, 1.5), 776u);
    // A wide box: as tall as the box.
    EXPECT_EQ(PreviewSide(letter, 0, 1200, 500, 1.25), 625u);
    // Never a sliver, never past what a bitmap can hold.
    EXPECT_EQ(PreviewSide(letter, 0, 2, 2, 1.0), 64u);
    EXPECT_EQ(PreviewSide(letter, 0, 4000, 6000, 3.0), 4096u);
    EXPECT_EQ(PreviewSide(0, 0, 400, 400, 1.0), 400u);   // shape not known yet
}

TEST(PageRender, QueueWakesForEveryRequestAfterItWentIdle)
{
    auto delivered = std::make_shared<Delivered>();
    {
        RenderQueue queue(&OpenFake);
        // Asked for before it has a target: nothing is drawn yet, and the preview goes first.
        queue.Thumbnails({ { { L"a.pdf", 0 }, 136 }, { { L"a.pdf", 1 }, 136 } });
        queue.Preview({ { L"a.pdf", 2 }, 700 });
        queue.SetTarget(delivered->Target(delivered));
        ASSERT_TRUE(delivered->WaitFor(3));
        EXPECT_TRUE(delivered->At(0).job.preview);
        EXPECT_EQ(delivered->At(0).job.key, (PageKey{ L"a.pdf", 2 }));
        EXPECT_EQ(delivered->At(0).pixels.height, 700u);   // fitted in 700 pixels
        EXPECT_EQ(delivered->At(0).pixels.width, 541u);
        EXPECT_EQ(delivered->At(1).job.key.page, 0u);
        EXPECT_EQ(delivered->At(1).pixels.height, 136u);
        EXPECT_DOUBLE_EQ(delivered->At(2).widthPt, 612);

        // Everything is drawn and the worker sleeps. A new page's preview (the user stepped to page
        // 2) wakes it: it used to sleep on, leaving the thumbnail scaled up as the preview.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        queue.Preview({ { L"a.pdf", 1 }, 900 });
        ASSERT_TRUE(delivered->WaitFor(4)) << "the preview asked for after the first pages was never drawn";
        EXPECT_TRUE(delivered->At(3).job.preview);
        EXPECT_EQ(delivered->At(3).job.key.page, 1u);
        EXPECT_EQ(delivered->At(3).pixels.height, 900u);

        // An edited page's thumbnail, asked for later still (after the edit was saved), is drawn too;
        // a file that can't be opened comes back empty rather than never.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        queue.Thumbnails({ { { L"edit 1-1.pdf", 0 }, 136 }, { { L"missing.pdf", 0 }, 136 } });
        ASSERT_TRUE(delivered->WaitFor(6)) << "the edited page's thumbnail was never drawn";
        EXPECT_EQ(delivered->At(4).job.key.path, L"edit 1-1.pdf");
        EXPECT_FALSE(delivered->At(4).pixels.Empty());
        EXPECT_TRUE(delivered->At(5).pixels.Empty());

        // What waits for the worker to let go of the files runs once it has.
        queue.WhenDone([delivered] {
            {
                std::scoped_lock lock(delivered->mutex);
                delivered->pages.push_back({});
            }
            delivered->arrived.notify_all();
        });
    }
    EXPECT_TRUE(delivered->WaitFor(7));

    // A queue that never started runs it at once when it goes.
    bool ran = false;
    {
        RenderQueue idle(&OpenFake);
        idle.WhenDone([&] { ran = true; });
    }
    EXPECT_TRUE(ran);
}

TEST(PageEdits, EachEditGetsFilesOfItsOwn)
{
    TempDir dir;
    const std::wstring folder = dir.path().wstring();
    EXPECT_EQ(EditFileStem(folder, false, 3, 1), dir / L"page 3-1");
    EXPECT_EQ(EditFileStem(folder, true, 3, 2), dir / L"edit 3-2");
    EXPECT_EQ(FreeEditSerial(folder, 0), 1u);
    EXPECT_EQ(FreeEditSerial(folder, 4), 5u);
    // A page view opened again on the folder (Back, then More...) counts past the edits in it.
    for (const auto* name : { L"edit 1-1.png", L"edit 1-1.pdf", L"edit 3-2.pdf", L"page 2-1.png", L"notes 9-1.txt", L"edit x-1.pdf" })
        std::ofstream(dir.path() / name) << "x";
    EXPECT_EQ(FreeEditSerial(folder, 0), 4u);
    EXPECT_EQ(FreeEditSerial(folder, 7), 8u);
    EXPECT_EQ(FreeEditSerial(dir / L"gone", 2), 3u);
}

TEST(PageEdits, RedactionLandsOnThePageWhereItWasDrawn)
{
    // A Letter page as the page view hands it to the Redact editor: rendered at kEditDpi.
    uint32_t pw = 0, ph = 0;
    PixelSize(612, 792, kEditDpi, kEditMaxSide, pw, ph);
    ASSERT_EQ(pw, 1700u);
    ASSERT_EQ(ph, 2200u);
    const double ppp = 612.0 / pw;   // points per picture pixel
    TempDir dir;
    const auto picture = dir / L"page 1-1.png", redacted = dir / L"edit 1-1.png", page = dir / L"edit 1-1.pdf";
    const Bitmap original = Pattern(pw, ph);
    ASSERT_HRESULT_SUCCEEDED(EncodeImageFile(original, picture, false, 0, 72.0 / ppp));

    // One line boxed in the editor, in the picture's pixels (a drag leaves fractions).
    const canvas::RectD drawn{ 150.4, 400.6, 900.2, 40.3 };
    const auto params = redact::CoversToJson({ picture }, { { { drawn, canvas::CoverKind::Solid, 0x000000 } } });
    bool changed = false;
    std::wstring error;
    ASSERT_TRUE(RenderPageEdit(PageTool::Redact, params, picture, redacted, changed, error)) << Narrow(error);
    ASSERT_TRUE(changed);
    ASSERT_TRUE(WritePicturePage(redacted, ppp, page, error)) << Narrow(error);

    // The edited page is the original's size with the picture over all of it, its first row at the
    // top: picture pixel (x, y) is the point (x * ppp, 792 - y * ppp).
    auto pdf = Open(page);
    ASSERT_TRUE(pdf);
    auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
    ASSERT_EQ(pages.size(), 1u);
    const auto box = Box(pages[0]);
    EXPECT_NEAR(box[2], 612, 0.01);
    EXPECT_NEAR(box[3], 792, 0.01);
    EXPECT_NE(ContentsOf(pages[0]).find("q 612 0 0 792 0 0 cm /Im0 Do Q"), std::string::npos);
    auto image = FirstImage(pages[0]);
    ASSERT_TRUE(image.isStream());
    ASSERT_EQ(image.getDict().getKey("/Width").getIntValue(), 1700);
    ASSERT_EQ(image.getDict().getKey("/Height").getIntValue(), 2200);
    const auto data = image.getStreamData(qpdf_dl_generalized);
    ASSERT_EQ(data->getSize(), size_t{ 1700 } * 2200 * 3);

    // Black exactly over the drawn box (widened to whole pixels: 150..1050 x 400..440), every other
    // pixel as it was: the box is no bigger than drawn and nothing else on the page changed.
    int left = INT_MAX, top = INT_MAX, right = -1, bottom = -1;
    size_t black = 0, changedOutside = 0;
    const uint8_t* rgb = data->getBuffer();
    for (uint32_t y = 0; y < ph; ++y)
        for (uint32_t x = 0; x < pw; ++x)
        {
            const uint8_t* p = rgb + (static_cast<size_t>(y) * pw + x) * 3;
            const uint8_t* o = &original.bgra[(static_cast<size_t>(y) * pw + x) * 4];
            if (p[0] == 0 && p[1] == 0 && p[2] == 0)
            {
                ++black;
                left = std::min<int>(left, x), top = std::min<int>(top, y);
                right = std::max<int>(right, x), bottom = std::max<int>(bottom, y);
            }
            else if (p[0] != o[2] || p[1] != o[1] || p[2] != o[0])
                ++changedOutside;
        }
    EXPECT_EQ(left, 150);
    EXPECT_EQ(top, 400);
    EXPECT_EQ(right, 1050);
    EXPECT_EQ(bottom, 440);
    EXPECT_EQ(black, size_t{ 901 } * 41);
    EXPECT_EQ(changedOutside, 0u);
    // In page space (points, y up) that is the drawn box times ppp, to within a pixel.
    EXPECT_NEAR(left * ppp, drawn.x * ppp, ppp);
    EXPECT_NEAR((right + 1) * ppp, drawn.Right() * ppp, ppp);
    EXPECT_NEAR(792 - top * ppp, 792 - drawn.y * ppp, ppp);
    EXPECT_NEAR(792 - (bottom + 1) * ppp, 792 - drawn.Bottom() * ppp, ppp);
}

TEST(ScrollList, ListFindsRowsAndGaps)
{
    // Merge's file list: one column of 40-DIP rows in a 100-DIP viewport.
    ScrollList list{ 0, 300, 40, 1 };
    EXPECT_EQ(list.ItemAt(10, 45, 5), 1);
    EXPECT_EQ(list.ItemAt(10, 210, 5), -1);   // past the last row
    EXPECT_EQ(list.InsertAt(10, 15, 5), 0u);  // the nearest gap
    EXPECT_EQ(list.InsertAt(10, 25, 5), 1u);
    EXPECT_EQ(list.InsertAt(10, 999, 5), 5u);
    list.Reveal(4, 5, 100);                   // the last row shown whole
    EXPECT_FLOAT_EQ(list.offset, 100);
    EXPECT_EQ(list.ItemAt(10, 5, 5), 2);
    list.Wheel(120, 2, 5, 100);               // a notch up: two rows
    EXPECT_FLOAT_EQ(list.offset, 20);
    list.Wheel(-1200, 2, 5, 100);
    EXPECT_FLOAT_EQ(list.offset, 100);        // never past the end
    EXPECT_TRUE(list.EdgeScroll(5, 100, 5));
    EXPECT_FLOAT_EQ(list.offset, 88);
    EXPECT_FALSE(list.EdgeScroll(50, 100, 5));
    EXPECT_EQ(list.Visible(5, 100), (std::pair<size_t, size_t>{ 2, 5 }));
}

TEST(ScrollList, GridFindsCellsAndGapsInARow)
{
    // The page view: 3 columns of 100 x 120 DIPs, 7 pages.
    ScrollList grid{ 0, 100, 120, 3 };
    EXPECT_EQ(grid.Rows(7), 3u);
    EXPECT_EQ(grid.ItemAt(150, 130, 7), 4);
    EXPECT_EQ(grid.ItemAt(150, 250, 7), -1);   // row 3 has only page 7
    EXPECT_EQ(grid.ItemAt(310, 10, 7), -1);    // right of the last column
    EXPECT_EQ(grid.InsertAt(140, 130, 7), 4u); // nearest gap in the second row: before page 5
    EXPECT_EQ(grid.InsertAt(290, 130, 7), 6u); // at the row's end
    EXPECT_EQ(grid.InsertAt(290, 250, 7), 7u); // never past the end
    grid.Reveal(6, 7, 200);
    EXPECT_FLOAT_EQ(grid.offset, 160);
    EXPECT_EQ(grid.Visible(7, 200), (std::pair<size_t, size_t>{ 3, 7 }));
}

TEST(ScrollList, DragStartsPastTheThreshold)
{
    DragGesture drag;
    EXPECT_FALSE(drag.Move(50, 50, 4));   // not pressed
    drag.Press(10, 10);
    EXPECT_FALSE(drag.Move(13, 12, 4));
    EXPECT_TRUE(drag.Move(10, 15, 4));    // once
    EXPECT_FALSE(drag.Move(10, 30, 4));
    EXPECT_TRUE(drag.dragging);
    drag.Reset();
    EXPECT_FALSE(drag.pressed || drag.dragging);
}

TEST(PdfPages, MergeFollowsAPagePlanAcrossFiles)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto a = ImagesPdf(dir, L"a.pdf", 2, 100, 50);   // pages 100 and 110 pt wide
    const auto b = ImagesPdf(dir, L"b.pdf", 3, 200, 80);   // 200, 210, 220
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    Recipe merge = recipes.Get("pdf-merge");
    // b's last page first and turned, then a's second, then b's first upside down; the rest left out.
    merge.steps[0].params["plan"] = PlanToJson({ { 1, 2, 90 }, { 0, 1, 0 }, { 1, 0, 180 } });
    const auto result = runner.RunBatch(merge, 0, { a, b }, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(FileName(result.output), L"a (merged).pdf");
    EXPECT_EQ(result.message, L"Merged 2 PDFs into 3 pages");
    auto pdf = Open(result.output);
    auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
    ASSERT_EQ(pages.size(), 3u);
    EXPECT_EQ(Box(pages[0])[2], 220);
    EXPECT_EQ(pages[0].getAttribute("/Rotate", false).getIntValue(), 90);
    EXPECT_EQ(Box(pages[1])[2], 110);
    EXPECT_EQ(Box(pages[2])[2], 200);
    EXPECT_EQ(pages[2].getAttribute("/Rotate", false).getIntValue(), 180);
}

TEST(PdfPages, EditedPagesLandInPlaceAndRedactionRemovesTheText)
{
    TempDir dir;
    const auto a = SecretsPdf(dir, L"a.pdf");
    const auto edited = RedactedPage(dir, L"edit");
    {
        // The redaction is in the picture's pixels: black where the box went, untouched elsewhere.
        Bitmap redacted;
        std::wstring error;
        ASSERT_TRUE(DecodeImageFile(PictureOf(edited), redacted, error)) << Narrow(error);
        const uint8_t* inside = &redacted.bgra[(120u * redacted.width + 150u) * 4];
        EXPECT_EQ(inside[0] + inside[1] + inside[2], 0);
        const Bitmap original = Pattern(306, 396);
        EXPECT_TRUE(std::equal(original.bgra.begin(), original.bgra.begin() + 4 * 50, redacted.bgra.begin()));
    }

    const auto out = dir / L"out.pdf";
    std::wstring error;
    ASSERT_TRUE(AssemblePages({ a }, { { 0, 0, 0 }, { 0, 1, 90, edited }, { 0, 2, 0 } }, out, {}, {}, error)) << Narrow(error);
    {
        auto pdf = Open(out);
        auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
        ASSERT_EQ(pages.size(), 3u);
        EXPECT_NE(ContentsOf(pages[0]).find("SECRETONE"), std::string::npos);
        EXPECT_NE(ContentsOf(pages[2]).find("SECRETTHREE"), std::string::npos);
        // The edited page sits second, the original's size, turned as the plan says, a picture only.
        EXPECT_EQ(Box(pages[1])[2], 612);
        EXPECT_EQ(Box(pages[1])[3], 792);
        EXPECT_EQ(pages[1].getAttribute("/Rotate", false).getIntValue(), 90);
        EXPECT_EQ(pages[1].getImages().size(), 1u);
        EXPECT_FALSE(pages[1].getObjectHandle().hasKey("/Annots"));
    }
    // Nothing of the original second page is left anywhere in the file: not its text, not its form
    // field's value (the bookmark still points at the emptied page), not its tag's alternate text.
    const auto content = AllContent(out);
    EXPECT_NE(content.find("SECRETONE"), std::string::npos);
    EXPECT_EQ(content.find("SECRETTWO"), std::string::npos);
    EXPECT_EQ(content.find("SECRETFIELD"), std::string::npos);
    EXPECT_EQ(content.find("SECRETALT"), std::string::npos);

    // Reverted (no edited copy): the page is the original again, text and all.
    const auto reverted = dir / L"reverted.pdf";
    ASSERT_TRUE(AssemblePages({ a }, { { 0, 0, 0 }, { 0, 1, 90 }, { 0, 2, 0 } }, reverted, {}, {}, error)) << Narrow(error);
    EXPECT_NE(AllContent(reverted).find("SECRETTWO"), std::string::npos);

    // A page left out is emptied too, so its text can't ride along with the bookmark.
    const auto without = dir / L"without.pdf";
    ASSERT_TRUE(AssemblePages({ a }, { { 0, 2, 0 }, { 0, 0, 0 } }, without, {}, {}, error)) << Narrow(error);
    const auto rest = AllContent(without);
    EXPECT_EQ(rest.find("SECRETTWO"), std::string::npos);
    EXPECT_EQ(rest.find("SECRETFIELD"), std::string::npos);
    EXPECT_NE(rest.find("SECRETTHREE"), std::string::npos);

    // An edited page whose file is gone is an error, not a silent original.
    EXPECT_FALSE(AssemblePages({ a }, { { 0, 1, 0, dir / L"gone.pdf" } }, dir / L"gone out.pdf", {}, {}, error));
}

TEST(PdfPages, EditSliceSavesOneAndMergesSeveral)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto a = SecretsPdf(dir, L"report.pdf");
    const auto b = ImagesPdf(dir, L"extra.pdf", 2, 100, 50);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    Recipe edit = recipes.Get("pdf-edit");
    EXPECT_TRUE(runner.Applies(edit, a));
    EXPECT_FALSE(runner.Applies(edit, dir / L"x.png"));

    // One PDF: "(edited)", the edited page in place.
    const auto page = RedactedPage(dir, L"edit");
    edit.steps[0].params["plan"] = PlanToJson({ { 0, 1, 0, page }, { 0, 0, 0 } });
    auto result = runner.RunBatch(edit, 0, { a }, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(FileName(result.output), L"report (edited).pdf");
    EXPECT_EQ(result.message, L"Saved 2 pages, 1 edited");
    {
        auto pdf = Open(result.output);
        auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
        ASSERT_EQ(pages.size(), 2u);
        EXPECT_EQ(pages[0].getImages().size(), 1u);
        EXPECT_NE(ContentsOf(pages[1]).find("SECRETONE"), std::string::npos);
    }
    EXPECT_EQ(AllContent(result.output).find("SECRETTWO"), std::string::npos);

    // Several: merged, named after the first, in the plan's order.
    edit.steps[0].params["plan"] = PlanToJson({ { 1, 2, 0 }, { 0, 1, 0 } });   // report's page 3, then extra's page 2
    result = runner.RunBatch(edit, 0, { b, a }, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(FileName(result.output), L"extra (merged).pdf");
    auto pdf = Open(result.output);
    auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
    ASSERT_EQ(pages.size(), 2u);
    EXPECT_NE(ContentsOf(pages[0]).find("SECRETTHREE"), std::string::npos);
    EXPECT_EQ(Box(pages[1])[2], 110);

    // Without a plan (the window was not used): every page as it is.
    edit.steps[0].params.erase("plan");
    result = runner.RunBatch(edit, 0, { a }, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(Open(result.output)->getAllPages().size(), 3u);
}

TEST(PdfPages, EditedPagesFolderGoesAfterTheExport)
{
    EXPECT_EQ(PageTools().size(), 4u);
    const auto folder = NewEditFolder();
    ASSERT_FALSE(folder.empty());
    EXPECT_TRUE(std::filesystem::is_directory(folder));
    TempDir dir;
    const auto a = ImagesPdf(dir, L"a.pdf", 2, 100, 50);
    const auto page = (std::filesystem::path(folder) / L"edit 1-1.pdf").wstring();
    ImagesPdf(dir, L"one.pdf", 1, 300, 40);
    std::filesystem::copy_file(dir / L"one.pdf", page);

    Recipes recipes(dir);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    Recipe edit = recipes.Get("pdf-edit");
    edit.steps[0].params["plan"] = PlanToJson({ { 0, 0, 0, page }, { 0, 1, 0 } });
    const auto result = runner.RunBatch(edit, 0, { a }, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    auto pdf = Open(result.output);
    auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
    EXPECT_EQ(Box(pages[0])[2], 300);
    EXPECT_FALSE(std::filesystem::exists(folder));   // read once, then removed

    // Only folders the page view made are ever removed.
    RemoveEditFolder(dir.path().wstring());
    EXPECT_TRUE(std::filesystem::exists(a));
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
    EXPECT_EQ(FileName(result.output), L"big (compressed).pdf");
    EXPECT_LT(FileSize(result.output), FileSize(big) / 2);
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
    EXPECT_EQ(FileName(result.output), L"big (reduced).pdf");
    EXPECT_LE(FileSize(result.output), FileSize(big) / 10);
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
    EXPECT_EQ(Joined(lines), L"Big Title|Hello world|sep arated|café|");
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
    EXPECT_EQ(Joined(lines), L"First line|Second line|");
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
    EXPECT_EQ(Joined(text.pages[0]), L"Page one text|");
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
    EXPECT_EQ(FileName(md.output), L"notes (converted).md");
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

namespace
{
    // A column of `count` lines of body text, `width` wide, from baseline `top` down, 12 pt apart.
    PageLines Column(double x, double width, double top, int count, const std::wstring& word)
    {
        PageLines lines;
        for (int i = 0; i < count; ++i)
            lines.push_back({ x, top - 12.0 * i, x + width, 10, std::format(L"{} {}", word, i + 1) });
        return lines;
    }

    // `a` and `b` taken in turns, the way a PDF that draws row by row (or OCR) lists them.
    PageLines Interleave(const PageLines& a, const PageLines& b)
    {
        PageLines out;
        for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
        {
            if (i < a.size())
                out.push_back(a[i]);
            if (i < b.size())
                out.push_back(b[i]);
        }
        return out;
    }

    std::wstring Runs(const std::vector<PageLines>& runs)
    {
        std::wstring s;
        for (const auto& run : runs)
        {
            s += L"[";
            for (size_t i = 0; i < run.size(); ++i)
                s += (i ? L"," : L"") + run[i].text;
            s += L"]";
        }
        return s;
    }

    std::wstring Joined(const std::wstring& word, int from, int to)
    {
        std::wstring s;
        for (int i = from; i <= to; ++i)
            s += (i > from ? L" " : L"") + std::format(L"{} {}", word, i);
        return s;
    }
}

TEST(TextLayout, TwoColumnsReadLeftThenRight)
{
    // A title across both columns, then two columns drawn row by row.
    PageLines page{ { 72, 760, 540, 18, L"Title across both" } };
    const auto both = Interleave(Column(72, 220, 720, 12, L"left"), Column(320, 220, 720, 12, L"right"));
    page.insert(page.end(), both.begin(), both.end());
    double left = 0, right = 0;
    ASSERT_TRUE(FindGutter(page, left, right));
    EXPECT_DOUBLE_EQ(left, 292);
    EXPECT_DOUBLE_EQ(right, 320);
    const auto runs = ReadingOrder(page);
    ASSERT_EQ(runs.size(), 3u) << Narrow(Runs(runs));
    EXPECT_EQ(runs[0].size(), 1u);
    EXPECT_EQ(runs[1].front().text, L"left 1");
    EXPECT_EQ(runs[1].back().text, L"left 12");
    EXPECT_EQ(runs[2].front().text, L"right 1");
    EXPECT_EQ(Narrow(ToMarkdown({ page })),
              "# Title across both\n\n" + Narrow(Joined(L"left", 1, 12)) + "\n\n" + Narrow(Joined(L"right", 1, 12)) + "\n");
    const auto text = Narrow(ToText({ page }));
    EXPECT_TRUE(text.starts_with("Title across both\r\n\r\nleft 1\r\nleft 2\r\n")) << text;
    EXPECT_NE(text.find("left 12\r\n\r\nright 1\r\nright 2\r\n"), std::string::npos) << text;
}

TEST(TextLayout, FullWidthHeadingSplitsTheColumnsIntoBands)
{
    // Top band, a heading across the page, bottom band; drawn right column first, then the heading,
    // then the left column, bottom to top.
    auto topLeft = Column(72, 220, 720, 6, L"a"), topRight = Column(320, 220, 720, 6, L"b");
    auto lowLeft = Column(72, 220, 600, 6, L"c"), lowRight = Column(320, 220, 600, 6, L"d");
    PageLines page;
    page.insert(page.end(), topRight.begin(), topRight.end());
    page.insert(page.end(), lowRight.begin(), lowRight.end());
    page.push_back({ 72, 630, 540, 14, L"Results across the page" });
    page.insert(page.end(), lowLeft.rbegin(), lowLeft.rend());
    page.insert(page.end(), topLeft.rbegin(), topLeft.rend());
    const auto runs = ReadingOrder(page);
    ASSERT_EQ(runs.size(), 5u) << Narrow(Runs(runs));
    EXPECT_EQ(runs[0].front().text, L"a 1");
    EXPECT_EQ(runs[0].back().text, L"a 6");
    EXPECT_EQ(runs[1].front().text, L"b 1");
    EXPECT_EQ(runs[2].front().text, L"Results across the page");
    EXPECT_EQ(runs[3].front().text, L"c 1");
    EXPECT_EQ(runs[4].front().text, L"d 1");
    EXPECT_EQ(runs[4].back().text, L"d 6");
    EXPECT_EQ(Narrow(ToMarkdown({ page })), Narrow(Joined(L"a", 1, 6) + L"\n\n" + Joined(L"b", 1, 6) + L"\n\n## Results across the page\n\n" +
                                                   Joined(L"c", 1, 6) + L"\n\n" + Joined(L"d", 1, 6) + L"\n"));
}

TEST(TextLayout, ThreeColumns)
{
    auto page = Interleave(Column(72, 150, 720, 10, L"one"), Column(246, 150, 720, 10, L"two"));
    const auto third = Column(420, 150, 720, 10, L"three");
    page.insert(page.begin() + 5, third.begin(), third.end());
    const auto runs = ReadingOrder(page);
    ASSERT_EQ(runs.size(), 3u) << Narrow(Runs(runs));
    EXPECT_EQ(runs[0].front().text, L"one 1");
    EXPECT_EQ(runs[1].front().text, L"two 1");
    EXPECT_EQ(runs[2].front().text, L"three 1");
    EXPECT_EQ(runs[2].back().text, L"three 10");
}

TEST(TextLayout, SingleColumnPagesKeepTheirOrder)
{
    double left = 0, right = 0;
    // Full-width text with a page number in the middle of the foot.
    auto page = Column(72, 468, 720, 15, L"line");
    page.push_back({ 300, 40, 312, 9, L"3" });
    EXPECT_FALSE(FindGutter(page, left, right));
    ASSERT_EQ(ReadingOrder(page).size(), 1u);
    EXPECT_EQ(ReadingOrder(page)[0].back().text, L"3");

    // A table whose rows came out as one line each (label, a wide gap, value): every row crosses.
    PageLines table;
    for (int i = 0; i < 10; ++i)
        table.push_back({ 72, 700.0 - 14 * i, 540, 10, std::format(L"Label {} value {}", i, i) });
    EXPECT_FALSE(FindGutter(table, left, right));

    // Text in the left half only, ragged: nothing on the other side of any gap.
    PageLines half;
    for (int i = 0; i < 10; ++i)
        half.push_back({ 72, 700.0 - 12 * i, 200.0 + (i % 3) * 30, 10, L"short line" });
    EXPECT_FALSE(FindGutter(half, left, right));

    // One block of text above another, offset to the right: not side by side, so not columns.
    auto stacked = Column(72, 220, 720, 6, L"upper");
    const auto lower = Column(320, 220, 500, 6, L"lower");
    stacked.insert(stacked.end(), lower.begin(), lower.end());
    EXPECT_FALSE(FindGutter(stacked, left, right));

    // Short labels on the left, long values on the right, listed separately: a form, not columns.
    PageLines form;
    for (int i = 0; i < 8; ++i)
        form.push_back({ 72, 700.0 - 14 * i, i == 0 ? 250.0 : 100.0 + (i % 2) * 10, 10, L"Label" });
    for (int i = 0; i < 8; ++i)
        form.push_back({ 300, 700.0 - 14 * i, 540, 10, L"A value that fills the line" });
    EXPECT_FALSE(FindGutter(form, left, right));
}

TEST(TextLayout, PicturesGoBetweenTheirParagraphs)
{
    // One column: a picture between two paragraphs, another under everything.
    PageLines page{
        { 72, 700, 540, 10, L"First paragraph" }, { 72, 688, 540, 10, L"goes on." },
        { 72, 500, 540, 10, L"Second paragraph." },
    };
    std::vector<PagePicture> pictures{ { 72, 520, 300, 660, 7 }, { 72, 100, 300, 300, 8 } };
    auto parts = ToMarkdownParts({ page }, { pictures });
    ASSERT_EQ(parts.size(), 4u);
    EXPECT_EQ(Narrow(parts[0].markdown), "First paragraph goes on.\n");
    EXPECT_EQ(parts[1].picture, 7);
    EXPECT_TRUE(parts[1].markdown.empty());
    EXPECT_EQ(Narrow(parts[2].markdown), "Second paragraph.\n");
    EXPECT_EQ(parts[3].picture, 8);
    // Without pictures, the parts are exactly ToMarkdown.
    parts = ToMarkdownParts({ page }, {});
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0].markdown, ToMarkdown({ page }));

    // Two columns: a picture inside the right column, and one as wide as both under the first band.
    auto columns = Interleave(Column(72, 220, 720, 22, L"left"), Column(320, 220, 720, 5, L"right"));
    const auto rest = Column(320, 220, 520, 6, L"more");
    columns.insert(columns.end(), rest.begin(), rest.end());
    const auto foot = Column(72, 468, 400, 3, L"foot");
    columns.insert(columns.end(), foot.begin(), foot.end());
    std::vector<PagePicture> placed{ { 320, 540, 540, 640, 1 }, { 72, 420, 540, 450, 2 } };
    parts = ToMarkdownParts({ columns }, { placed });
    ASSERT_EQ(parts.size(), 5u);
    EXPECT_EQ(Narrow(parts[0].markdown), Narrow(Joined(L"left", 1, 22) + L"\n\n" + Joined(L"right", 1, 5) + L"\n"));
    EXPECT_EQ(parts[1].picture, 1);
    EXPECT_EQ(Narrow(parts[2].markdown), Narrow(Joined(L"more", 1, 6) + L"\n"));
    EXPECT_EQ(parts[3].picture, 2);
    EXPECT_EQ(Narrow(parts[4].markdown), Narrow(Joined(L"foot", 1, 3) + L"\n"));
}

TEST(PdfText, TwoColumnPdfReadsLeftColumnFirst)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto path = dir / L"paper.pdf";
    // The right column is drawn before the left one.
    std::string content = "BT /F1 18 Tf 72 740 Td (A Paper Title Across The Page) Tj ET\n";
    for (const char* side : { "Right", "Left" })
    {
        const int x = side[0] == 'R' ? 320 : 72;
        content += std::format("BT /F1 10 Tf {} 700 Td 12 TL", x);
        for (int i = 1; i <= 10; ++i)
            content += std::format(" ({} column line {} with words) '", side, i);
        content += " ET\n";
    }
    WriteTextPdf(path, { content });
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    const auto md = runner.Run(recipes.Get("pdf-to-md"), 0, path, {});
    ASSERT_TRUE(md.ok) << Narrow(md.error);
    const auto text = ReadAll(md.output);
    EXPECT_TRUE(text.starts_with("# A Paper Title Across The Page\n\nLeft column line 1 with words Left column line 2")) << text;
    EXPECT_NE(text.find("Left column line 10 with words\n\nRight column line 1 with words"), std::string::npos) << text;
}

TEST(PdfText, KeepsPicturesOnceWithTheirPlaces)
{
    TempDir dir;
    const auto path = dir / L"pictures.pdf";
    Samples photo{ 32, 24, 3, {} };
    for (uint32_t i = 0; i < photo.width * photo.height; ++i)
        photo.data.insert(photo.data.end(), { uint8_t(200), uint8_t(i % 256), uint8_t(40) });
    std::string jpeg;
    ASSERT_HRESULT_SUCCEEDED(EncodeJpeg(photo, 90, jpeg));
    std::string checker;   // 24 x 16, 1 bit per pixel, 3 bytes a row
    for (int y = 0; y < 16; ++y)
        checker += std::string(3, static_cast<char>(y % 2 ? 0x55 : 0xAA));
    std::string alpha;
    for (int i = 0; i < 20 * 20; ++i)
        alpha += static_cast<char>(i % 20 < 10 ? 0 : 255);
    const std::vector<TestImage> images{
        { "/Type /XObject /Subtype /Image /Width 40 /Height 30 /ColorSpace /DeviceRGB /BitsPerComponent 8", RgbRamp(40, 30), true },
        { "/Type /XObject /Subtype /Image /Width 8 /Height 8 /ColorSpace /DeviceGray /BitsPerComponent 8", std::string(64, '\x80'), true },
        { "/Type /XObject /Subtype /Image /Width 32 /Height 24 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode", jpeg },
        { "/Type /XObject /Subtype /Image /Width 24 /Height 16 /ColorSpace [/Indexed /DeviceRGB 1 <FF0000 0000FF>] /BitsPerComponent 1",
          checker, true },
        { "/Type /XObject /Subtype /Image /Width 20 /Height 20 /ColorSpace /DeviceGray /BitsPerComponent 8", std::string(400, '\x60'), true, 5 },
        { "/Type /XObject /Subtype /Image /Width 20 /Height 20 /ColorSpace /DeviceGray /BitsPerComponent 8", alpha, true },
    };
    WritePicturePdf(path,
                    { "BT /F1 12 Tf 72 700 Td (Before the picture.) Tj ET\n"
                      "q 300 0 0 225 72 420 cm /Im0 Do Q\n"
                      "q 40 0 0 40 500 700 cm /Im1 Do Q\n"   // a tiny decoration
                      "BT /F1 12 Tf 72 380 Td (After the picture.) Tj ET",
                      "q 300 0 0 225 72 420 cm /Im0 Do Q\n"   // the same picture again
                      "q 160 0 0 120 72 600 cm /Im2 Do Q\n"
                      "q 96 0 0 64 300 600 cm /Im3 Do Q\n"
                      "q 100 0 0 100 72 300 cm /Im4 Do Q\n"
                      "BT /F1 12 Tf 72 200 Td (Page two text.) Tj ET" },
                    images);
    PdfText text;
    std::wstring error;
    ASSERT_TRUE(ReadPdfText(path, false, {}, {}, text, error, true)) << Narrow(error);
    ASSERT_EQ(text.pictures.size(), 2u);
    ASSERT_EQ(text.pictures[0].size(), 1u);   // the decoration is left out
    ASSERT_EQ(text.pictures[1].size(), 3u);   // the repeat is left out
    ASSERT_EQ(text.images.size(), 4u);
    const auto& first = text.pictures[0][0];
    EXPECT_DOUBLE_EQ(first.left, 72);
    EXPECT_DOUBLE_EQ(first.bottom, 420);
    EXPECT_DOUBLE_EQ(first.right, 372);
    EXPECT_DOUBLE_EQ(first.top, 645);

    // The Flate picture as a PNG with its pixels intact.
    const auto& ramp = text.images[static_cast<size_t>(first.id)];
    EXPECT_EQ(ramp.width, 40u);
    EXPECT_EQ(ramp.height, 30u);
    EXPECT_EQ(ramp.bytes.substr(1, 3), "PNG");
    Samples back;
    ASSERT_HRESULT_SUCCEEDED(DecodeJpeg(ramp.bytes, back));   // WIC reads any format it knows
    ASSERT_EQ(back.channels, 3u);
    const uint8_t* p = &back.data[(3 * 40 + 5) * 3];
    EXPECT_EQ(p[0], 30);
    EXPECT_EQ(p[1], 24);
    EXPECT_EQ(p[2], 128);
    // The JPEG passes through as stored.
    EXPECT_EQ(text.images[static_cast<size_t>(text.pictures[1][0].id)].bytes, jpeg);
    // Indexed, 1 bit: red and blue.
    const auto& indexed = text.images[static_cast<size_t>(text.pictures[1][1].id)];
    ASSERT_HRESULT_SUCCEEDED(DecodeJpeg(indexed.bytes, back));
    EXPECT_EQ(back.data[0], 0);      // first pixel of row 0 (0xAA: bit 1) is blue
    EXPECT_EQ(back.data[2], 255);
    EXPECT_EQ(back.data[3], 255);    // the next one red
    EXPECT_EQ(back.data[5], 0);
    // A soft mask becomes alpha: an RGBA PNG (colour type 6).
    const auto& masked = text.images[static_cast<size_t>(text.pictures[1][2].id)];
    ASSERT_GT(masked.bytes.size(), 26u);
    EXPECT_EQ(masked.bytes[25], 6);

    // The pictures sit between the paragraphs they sat between.
    const auto parts = ToMarkdownParts(text.pages, text.pictures);
    ASSERT_GE(parts.size(), 3u);
    EXPECT_EQ(Narrow(parts[0].markdown), "Before the picture.\n");
    EXPECT_EQ(parts[1].picture, first.id);
    EXPECT_EQ(Narrow(parts[2].markdown), "After the picture.\n");
}

TEST(PdfText, PageSizedScanUnderItsTextLayerIsNotAPicture)
{
    TempDir dir;
    const auto path = dir / L"scan.pdf";
    std::string content = "q 612 0 0 792 0 0 cm /Im0 Do Q\nBT 3 Tr /F1 12 Tf 72 700 Td 14 TL";
    for (int i = 0; i < 8; ++i)
        content += " (Scanned words on a line) '";
    content += " ET";
    WritePicturePdf(path, { content },
                    { { "/Type /XObject /Subtype /Image /Width 40 /Height 30 /ColorSpace /DeviceRGB /BitsPerComponent 8", RgbRamp(40, 30), true } });
    PdfText text;
    std::wstring error;
    ASSERT_TRUE(ReadPdfText(path, false, {}, {}, text, error, true)) << Narrow(error);
    ASSERT_EQ(text.pictures.size(), 1u);
    EXPECT_TRUE(text.pictures[0].empty());
    EXPECT_TRUE(text.images.empty());
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
    EXPECT_EQ(FileName(result.output), L"deck (pages)");
    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(result.output + L"\\deck page 02.png", w, h));
    EXPECT_EQ(w, 2550u);   // 8.5 in at 300 dpi
    EXPECT_EQ(h, 3300u);
}

TEST(PdfRender, AnEditedPageRendersWithItsRedactionWhereItWasDrawn)
{
    if (!RenderingAvailable())
        GTEST_SKIP() << "Windows.Data.Pdf is not compiled into this build";
    TempDir dir;
    const auto source = dir / L"lines.pdf", picture = dir / L"page 1-1.png", redacted = dir / L"edit 1-1.png", page = dir / L"edit 1-1.pdf";
    // Three black "lines of text" 12 pt tall, at 700, 628 and 556 pt from the bottom.
    WriteTextPdf(source, { "0 0 0 rg 72 700 400 12 re f 72 628 400 12 re f 72 556 400 12 re f" });
    RunMta([&] {
        // The page view opens a page for editing as a picture rendered at kEditDpi.
        std::wstring error;
        auto doc = OpenForRendering(source, error);
        ASSERT_TRUE(doc) << Narrow(error);
        double w = 0, h = 0;
        ASSERT_TRUE(doc->PageSize(0, w, h));
        uint32_t pw = 0, ph = 0;
        PixelSize(w, h, kEditDpi, kEditMaxSide, pw, ph);
        Bitmap pixels;
        ASSERT_TRUE(doc->Render(0, pw, ph, pixels, error)) << Narrow(error);
        const double ppp = w / pw;
        ASSERT_HRESULT_SUCCEEDED(EncodeImageFile(pixels, picture, false, 0, 72.0 / ppp));

        // The middle line boxed in red in the editor (66..478 pt across, 622..646 pt up), in pixels.
        const canvas::RectD drawn{ 66 / ppp, (792 - 646) / ppp, 412 / ppp, 24 / ppp };
        const auto params = redact::CoversToJson({ picture }, { { { drawn, canvas::CoverKind::Solid, 0xFF0000 } } });
        bool changed = false;
        ASSERT_TRUE(RenderPageEdit(PageTool::Redact, params, picture, redacted, changed, error)) << Narrow(error);
        ASSERT_TRUE(changed);
        ASSERT_TRUE(WritePicturePage(redacted, ppp, page, error)) << Narrow(error);

        // The edited page renders, as the page view's thumbnail and preview draw it: not blank.
        auto edited = OpenForRendering(page, error);
        ASSERT_TRUE(edited) << Narrow(error);
        double ew = 0, eh = 0;
        ASSERT_TRUE(edited->PageSize(0, ew, eh));
        EXPECT_NEAR(ew, 612, 1);
        EXPECT_NEAR(eh, 792, 1);
        Bitmap shown;
        ASSERT_TRUE(edited->Render(0, 612, 792, shown, error)) << Narrow(error);   // a pixel per point
        ASSERT_EQ(shown.width, 612u);
        const auto at = [&](int xPt, int yPt) { return &shown.bgra[(static_cast<size_t>(792 - yPt) * 612 + static_cast<size_t>(xPt)) * 4]; };
        const auto red = [](const uint8_t* p) { return p[2] > 200 && p[1] < 60 && p[0] < 60; };
        const auto black = [](const uint8_t* p) { return p[0] < 60 && p[1] < 60 && p[2] < 60; };
        const auto white = [](const uint8_t* p) { return p[0] > 200 && p[1] > 200 && p[2] > 200; };
        // The cover sits where it was drawn and is no bigger: the lines above and below stay, and the
        // paper just outside it is white.
        EXPECT_TRUE(red(at(272, 634)));
        EXPECT_TRUE(red(at(70, 625)));
        EXPECT_TRUE(red(at(474, 643)));
        EXPECT_TRUE(black(at(272, 706)));
        EXPECT_TRUE(black(at(272, 562)));
        EXPECT_TRUE(white(at(272, 652)));
        EXPECT_TRUE(white(at(272, 616)));
        EXPECT_TRUE(white(at(60, 634)));
        EXPECT_TRUE(white(at(484, 634)));
    });
}
