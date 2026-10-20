#include "PdfTestUtil.h"
#include "PipelineRunner.h"
#include "ocr/Ocr.h"
#include "ocr/TextLayer.h"
#include "pdf/PageRenderer.h"
#include "pdf/PdfContent.h"
#include "pdf/PdfDoc.h"
#include "pdf/TextLayout.h"
#include "pdf/Wic.h"

#include <qpdf/QPDFPageDocumentHelper.hh>

#include <filesystem>

using namespace sd;
using namespace sd::ocr;
using namespace sd::test;

namespace
{
    // ---- Stand-ins for Windows OCR and Windows.Data.Pdf ------------------------------------------
    // They let the searchable-PDF and Copy text paths run anywhere (MinGW tests included). The fake
    // recogniser "reads" two lines at fixed places, as fractions of whatever bitmap it is given.

    int g_recognized = 0;

    bool FakeAvailable(std::wstring*) { return true; }

    Word At(const pdf::Bitmap& image, const wchar_t* text, double x, double y, double w, double h)
    {
        return Word{ text, x * image.width, y * image.height, w * image.width, h * image.height };
    }

    bool FakeRecognize(const pdf::Bitmap& image, Result& out, std::wstring&)
    {
        ++g_recognized;
        out = {};
        out.width = image.width;
        out.height = image.height;
        out.language = L"en-US";
        out.lines.push_back(Line{ L"Hello world", { At(image, L"Hello", 0.1, 0.2, 0.2, 0.1), At(image, L"world", 0.4, 0.2, 0.2, 0.1) } });
        out.lines.push_back(Line{ L"Second line", { At(image, L"Second", 0.1, 0.5, 0.3, 0.1), At(image, L"line", 0.5, 0.5, 0.2, 0.1) } });
        return true;
    }

    // Every page the same displayed size (points), white.
    double g_pageW = 612, g_pageH = 792;

    class FakeRendered final : public pdf::IRenderedPdf
    {
    public:
        explicit FakeRendered(uint32_t pages) : m_pages(pages) {}
        uint32_t PageCount() const override { return m_pages; }
        bool PageSize(uint32_t, double& w, double& h) const override
        {
            w = g_pageW;
            h = g_pageH;
            return true;
        }
        bool Render(uint32_t, uint32_t w, uint32_t h, pdf::Bitmap& out, std::wstring&) const override
        {
            out.width = w;
            out.height = h;
            out.bgra.assign(static_cast<size_t>(w) * h * 4, 255);
            return true;
        }

    private:
        uint32_t m_pages;
    };

    std::unique_ptr<pdf::IRenderedPdf> FakeOpen(const std::wstring& path, std::wstring& error)
    {
        auto pdf = pdf::OpenPdf(path, error);
        if (!pdf)
            return nullptr;
        return std::make_unique<FakeRendered>(static_cast<uint32_t>(pdf->getAllPages().size()));
    }

    // Installs the fakes for one test and puts back whatever was there (the real WinRT hooks in
    // the MSVC build).
    class FakePlatform
    {
    public:
        FakePlatform() : m_ocr(Platform()), m_render(pdf::Renderer())
        {
            Platform() = Hooks{ &FakeAvailable, &FakeRecognize };
            pdf::Renderer() = pdf::RenderHooks{ &FakeOpen };
            g_recognized = 0;
            g_pageW = 612;
            g_pageH = 792;
        }
        ~FakePlatform()
        {
            Platform() = m_ocr;
            pdf::Renderer() = m_render;
        }
        FakePlatform(const FakePlatform&) = delete;
        FakePlatform& operator=(const FakePlatform&) = delete;

    private:
        Hooks m_ocr;
        pdf::RenderHooks m_render;
    };

    std::wstring Joined(const pdf::PageLines& lines)
    {
        std::wstring s;
        for (const auto& l : lines)
            s += l.text + L"|";
        return s;
    }

    pdf::PageLines PageText(const std::wstring& path, size_t page)
    {
        std::wstring error;
        auto doc = pdf::OpenPdf(path, error);
        EXPECT_TRUE(doc) << Narrow(error);
        if (!doc)
            return {};
        auto pages = QPDFPageDocumentHelper(*doc).getAllPages();
        EXPECT_LT(page, pages.size());
        return page < pages.size() ? pdf::ExtractPageLines(pages[page]) : pdf::PageLines{};
    }

    std::wstring ReadClipboard()
    {
        std::wstring text;
        if (!OpenClipboard(nullptr))
            return text;
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
            if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h)))
            {
                text = p;
                GlobalUnlock(h);
            }
        CloseClipboard();
        return text;
    }

    // A white bitmap with black text drawn by GDI, for the real recogniser.
    pdf::Bitmap TextBitmap(const wchar_t* text, int width, int height, int fontPx)
    {
        pdf::Bitmap b;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dc || !dib || !bits)
            return b;
        HGDIOBJ oldBitmap = SelectObject(dc, dib);
        RECT r{ 0, 0, width, height };
        FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        HFONT font = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Arial");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetTextColor(dc, RGB(0, 0, 0));
        SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, fontPx, fontPx, text, static_cast<int>(wcslen(text)));
        GdiFlush();
        b.width = static_cast<uint32_t>(width);
        b.height = static_cast<uint32_t>(height);
        b.bgra.assign(static_cast<const uint8_t*>(bits), static_cast<const uint8_t*>(bits) + static_cast<size_t>(width) * height * 4);
        for (size_t i = 3; i < b.bgra.size(); i += 4)
            b.bgra[i] = 255;
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteObject(font);
        DeleteObject(dib);
        DeleteDC(dc);
        return b;
    }
}

// ---- Formatting -----------------------------------------------------------------------------------

TEST(OcrText, CountWords)
{
    EXPECT_EQ(CountWords(L""), 0u);
    EXPECT_EQ(CountWords(L"   \r\n "), 0u);
    EXPECT_EQ(CountWords(L"Hello, world!"), 2u);
    EXPECT_EQ(CountWords(L"one\ttwo\r\nthree  four"), 4u);
    EXPECT_EQ(CountWords(L"- • -- 42"), 1u);       // punctuation alone is not a word
    EXPECT_EQ(CountWords(L"café дом"), 2u);   // any script's letters
    EXPECT_EQ(CountWords(L"日本　語"), 2u);       // ideographic space separates
}

TEST(OcrText, CopiedMessage)
{
    EXPECT_EQ(CopiedMessage(0, 0), L"No text found");
    EXPECT_EQ(CopiedMessage(1, 1), L"Copied 1 word");
    EXPECT_EQ(CopiedMessage(214, 1), L"Copied 214 words");
    EXPECT_EQ(CopiedMessage(30, 3), L"Copied 30 words from 3 files");
}

TEST(OcrText, LinesFromOcrBecomeParagraphs)
{
    Result r;
    r.width = 1000;
    r.height = 1000;
    r.lines.push_back(Line{ L"First  line", { Word{ L"First", 100, 100, 200, 40 }, Word{ L"line", 320, 100, 150, 40 } } });
    r.lines.push_back(Line{ L"goes on", { Word{ L"goes", 100, 150, 150, 40 }, Word{ L"on", 270, 150, 80, 40 } } });
    r.lines.push_back(Line{ L"Next paragraph", { Word{ L"Next", 100, 400, 150, 40 }, Word{ L"paragraph", 270, 400, 300, 40 } } });
    r.lines.push_back(Line{ L"", {} });
    const auto lines = pdf::LinesFromOcr(r, 0.5);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0].text, L"First line");   // runs of spaces collapse
    EXPECT_DOUBLE_EQ(lines[0].x, 50);
    EXPECT_DOUBLE_EQ(lines[0].endX, 235);
    EXPECT_DOUBLE_EQ(lines[0].y, 430);         // (1000 - 140) * 0.5, y up
    EXPECT_DOUBLE_EQ(lines[0].size, 20);
    EXPECT_EQ(pdf::ToText({ lines }), L"First line\r\ngoes on\r\n\r\nNext paragraph\r\n");
}

// ---- The invisible text layer -------------------------------------------------------------------

TEST(OcrTextLayer, HexCodesAreUtf16)
{
    EXPECT_EQ(HexCodes(L"Ab", false), "<00410062>");
    EXPECT_EQ(HexCodes(L"é", true), "<00E90020>");
    EXPECT_EQ(HexCodes(L"x\U0001F600", false), "<0078>");   // no lone surrogates
}

TEST(OcrTextLayer, IdentityCMapSkipsSurrogates)
{
    const auto cmap = IdentityToUnicodeCMap();
    size_t ranges = 0;
    for (size_t at = cmap.find("> <"); at != std::string::npos; at = cmap.find("> <", at + 1))
        ++ranges;
    // "<hh00> <hhFF> <hh00>" has two "> <" per range, one per codespace line.
    EXPECT_EQ((ranges - 1) / 2, 248u);
    EXPECT_NE(cmap.find("<0000> <00FF> <0000>"), std::string::npos);
    EXPECT_NE(cmap.find("<D700> <D7FF> <D700>"), std::string::npos);
    EXPECT_EQ(cmap.find("<D800>"), std::string::npos);
    EXPECT_NE(cmap.find("<FF00> <FFFF> <FF00>"), std::string::npos);
    int codeBytes = 0;
    const auto map = pdf::ParseToUnicode(cmap, codeBytes);
    EXPECT_EQ(codeBytes, 2);
    EXPECT_EQ(map.at(0x0041), L"A");
    EXPECT_EQ(map.at(0x4E2D), L"中");
    EXPECT_FALSE(map.contains(0xD800));
}

TEST(OcrTextLayer, OperatorsUseRenderModeThreeAndStretch)
{
    PlacedWord w;
    w.text = L"Hi";
    w.x = 10;
    w.y = 20;
    w.width = 30;    // 2 glyphs x 0.5 em x 20 pt = 20 pt natural: 150%
    w.height = 20;
    w.spaceAfter = true;
    EXPECT_EQ(TextLayerOps({ w }, "SDOcr"), "BT\n3 Tr\n/SDOcr 20 Tf 150 Tz 1 0 0 1 10 20 Tm <004800690020> Tj\nET\n");
    w.rotation = 90;
    w.spaceAfter = false;
    EXPECT_NE(TextLayerOps({ w }, "F").find("0 1 -1 0 10 20 Tm <00480069> Tj"), std::string::npos);
    w.text = L"\U0001F600";   // nothing it can map
    EXPECT_EQ(TextLayerOps({ w }, "F"), "");
    EXPECT_EQ(TextLayerOps({}, "F"), "");
}

TEST(OcrTextLayer, ImagePageScalesAndFlips)
{
    Result r;
    r.width = 200;
    r.height = 100;
    r.lines.push_back(Line{ L"a b", { Word{ L"a", 20, 10, 40, 20 }, Word{ L"b", 80, 10, 40, 20 }, Word{ L"", 0, 0, 0, 0 } } });
    const auto words = PlaceOnImagePage(r, 400, 200);
    ASSERT_EQ(words.size(), 2u);
    EXPECT_DOUBLE_EQ(words[0].x, 40);
    EXPECT_DOUBLE_EQ(words[0].y, 140);   // bottom of the box, from the bottom of the page
    EXPECT_DOUBLE_EQ(words[0].width, 80);
    EXPECT_DOUBLE_EQ(words[0].height, 40);
    EXPECT_TRUE(words[0].spaceAfter);
    EXPECT_TRUE(words[1].spaceAfter);   // the empty third word is dropped, but it still counted as "more"
    EXPECT_TRUE(PlaceOnImagePage(Result{}, 100, 100).empty());
}

TEST(OcrTextLayer, PdfPageUndoesEveryRotation)
{
    // A 100 x 200 pt page whose crop box starts at (10, 20). The bitmap shows it as displayed; one
    // word whose box's bottom-left corner is displayed 10 pt right of and 30 pt below the top-left.
    const double box[4] = { 10, 20, 110, 220 };
    struct Case
    {
        int rotate;
        uint32_t w, h;   // displayed bitmap (1 px = 1 pt)
        double ux, uy;   // where the baseline starts in user space
    };
    const Case cases[] = {
        { 0, 100, 200, 10 + 10, 220 - 30 },
        { 90, 200, 100, 10 + 30, 20 + 10 },
        { 180, 100, 200, 110 - 10, 20 + 30 },
        { 270, 200, 100, 110 - 30, 220 - 10 },
        { -90, 200, 100, 110 - 30, 220 - 10 },
    };
    for (const auto& c : cases)
    {
        Result r;
        r.width = c.w;
        r.height = c.h;
        r.lines.push_back(Line{ L"w", { Word{ L"w", 10, 20, 50, 10 } } });
        const auto words = PlaceOnPdfPage(r, box, c.rotate);
        ASSERT_EQ(words.size(), 1u) << c.rotate;
        EXPECT_DOUBLE_EQ(words[0].x, c.ux) << c.rotate;
        EXPECT_DOUBLE_EQ(words[0].y, c.uy) << c.rotate;
        EXPECT_DOUBLE_EQ(words[0].width, 50) << c.rotate;
        EXPECT_EQ(words[0].rotation, ((c.rotate % 360) + 360) % 360);
    }
}

// ---- Searchable PDF and Copy text (fake OCR) ----------------------------------------------------

TEST(OcrSearchable, ImageBecomesAPdfWhoseTextReadsBack)
{
    FakePlatform fake;
    TempDir dir;
    Recipes recipes(dir);
    const auto image = dir / L"scan.png";
    ASSERT_HRESULT_SUCCEEDED(pdf::EncodeImageFile(Pattern(400, 200), image, false, 90, 72));
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    const auto& recipe = recipes.Get("ocr-searchable-pdf");
    EXPECT_TRUE(runner.Applies(recipe, image));
    EXPECT_TRUE(runner.Accepts(recipe, image, nullptr));
    const auto result = runner.Run(recipe, 0, image, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(std::filesystem::path(result.output).filename().wstring(), L"scan (searchable).pdf");
    EXPECT_EQ(result.message, L"Searchable PDF: 4 words");
    EXPECT_EQ(g_recognized, 1);
    // The text layer is real text to a PDF reader: our own extractor gets it back, in place.
    const auto lines = PageText(result.output, 0);
    EXPECT_EQ(Joined(lines), L"Hello world|Second line|");
    ASSERT_FALSE(lines.empty());
    EXPECT_NEAR(lines[0].x, 40, 0.5);
    EXPECT_NEAR(lines[0].y, 140, 0.5);
    EXPECT_NEAR(lines[0].size, 20, 0.5);
    // ...and it is invisible: render mode 3 in the page's content.
    std::wstring error;
    auto doc = pdf::OpenPdf(result.output, error);
    ASSERT_TRUE(doc);
    auto page = QPDFPageDocumentHelper(*doc).getAllPages().front();
    std::string all;
    for (auto& stream : page.getPageContents())
    {
        auto buf = stream.getStreamData(qpdf_dl_generalized);
        all.append(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    }
    EXPECT_NE(all.find("3 Tr"), std::string::npos);
}

TEST(OcrSearchable, ScannedPagesGetTextAndTextPagesAreLeftAlone)
{
    FakePlatform fake;
    TempDir dir;
    Recipes recipes(dir);
    const auto path = dir / L"mixed.pdf";
    // Page 1 has a text layer; page 2 is a "scan" (just a filled box), shown turned a quarter.
    WriteTextPdf(path, { "BT /F1 12 Tf 72 700 Td (This page has text) Tj ET", "0 0 0 rg 72 72 100 100 re f" }, { 0, 90 });
    g_pageW = 792;
    g_pageH = 612;
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    const auto& recipe = recipes.Get("ocr-searchable-pdf");
    ASSERT_TRUE(runner.Accepts(recipe, path, nullptr));
    const auto result = runner.Run(recipe, 0, path, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(result.message, L"Text added to 1 of 2 pages");
    EXPECT_EQ(g_recognized, 1);
    EXPECT_EQ(Joined(PageText(result.output, 0)), L"This page has text|");
    // The sideways page reads upright again (the extractor turns it to reading order).
    EXPECT_EQ(Joined(PageText(result.output, 1)), L"Hello world|Second line|");

    // A PDF that already has text everywhere is skipped, not copied.
    const auto done = dir / L"done.pdf";
    WriteTextPdf(done, { "BT /F1 12 Tf 72 700 Td (Already searchable) Tj ET" });
    const auto skipped = runner.Run(recipe, 0, done, {});
    EXPECT_FALSE(skipped.ok);
    EXPECT_TRUE(skipped.skipped);
    EXPECT_EQ(skipped.error, L"already searchable: every page has text");
}

TEST(OcrCopyText, CopiesEveryFilesTextToTheClipboard)
{
    FakePlatform fake;
    TempDir dir;
    Recipes recipes(dir);
    const auto pdfPath = dir / L"a.pdf";
    WriteTextPdf(pdfPath, { "BT /F1 12 Tf 72 700 Td (This page has text) Tj ET" });
    const auto image = dir / L"b.png";
    ASSERT_HRESULT_SUCCEEDED(pdf::EncodeImageFile(Pattern(400, 200), image, false, 90, 72));
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    const auto& recipe = recipes.Get("ocr-copy-text");
    EXPECT_TRUE(runner.Applies(recipe, pdfPath));
    EXPECT_TRUE(runner.Applies(recipe, image));
    EXPECT_FALSE(runner.Applies(recipe, dir / L"c.txt"));
    const auto* engine = runner.EngineFor(recipe);
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->IsBatch(recipe));
    const auto result = runner.RunBatch(recipe, 0, { pdfPath, image }, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_TRUE(result.output.empty());   // nothing written next to the files
    EXPECT_EQ(result.message, L"Copied 8 words from 2 files");
    EXPECT_EQ(g_recognized, 1);           // the PDF's own text needed no OCR
    const auto clip = ReadClipboard();
    EXPECT_TRUE(clip.starts_with(L"This page has text\r\n")) << Narrow(clip);
    EXPECT_NE(clip.find(L"Hello world"), std::wstring::npos) << Narrow(clip);
    EXPECT_NE(clip.find(L"Second line"), std::wstring::npos) << Narrow(clip);
    EXPECT_EQ(CountFiles(dir, L".txt"), 0u);
}

TEST(OcrCopyText, WithoutOcrOnlyTextPdfsAreAccepted)
{
    TempDir dir;
    Recipes recipes(dir);
    const Hooks saved = Platform();
    Platform() = Hooks{};
    const auto pdfPath = dir / L"a.pdf";
    WriteTextPdf(pdfPath, { "BT /F1 12 Tf 72 700 Td (This page has text) Tj ET" });
    const auto image = dir / L"b.png";
    const HRESULT written = pdf::EncodeImageFile(Pattern(64, 64), image, false, 90, 72);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    std::wstring why;
    const bool pdfOk = runner.Accepts(recipes.Get("ocr-copy-text"), pdfPath, nullptr);
    const bool imageOk = runner.Accepts(recipes.Get("ocr-copy-text"), image, &why);
    const bool searchableOk = runner.Accepts(recipes.Get("ocr-searchable-pdf"), pdfPath, nullptr);
    Platform() = saved;
    ASSERT_HRESULT_SUCCEEDED(written);
    EXPECT_TRUE(pdfOk);
    EXPECT_FALSE(imageOk);
    EXPECT_EQ(why, L"Windows OCR is not available in this build");
    EXPECT_FALSE(searchableOk);
}

// ---- Windows OCR itself (runs where it is compiled in and a language is installed) --------------

TEST(OcrWindows, ReadsPrintedText)
{
    std::wstring why;
    if (!Available(&why))
        GTEST_SKIP() << "Windows OCR is not usable here: " << Narrow(why);
    const auto bitmap = TextBitmap(L"Quick brown fox 2024", 900, 160, 56);
    ASSERT_FALSE(bitmap.Empty());
    Result result;
    std::wstring error;
    bool ok = false;
    RunMta([&] { ok = Recognize(bitmap, result, error); });
    ASSERT_TRUE(ok) << Narrow(error);
    EXPECT_EQ(result.width, 900u);
    EXPECT_EQ(result.height, 160u);
    ASSERT_FALSE(result.lines.empty());
    const auto text = pdf::ToText({ pdf::LinesFromOcr(result, 1.0) });
    EXPECT_NE(text.find(L"brown"), std::wstring::npos) << Narrow(text);
    // Boxes are in the bitmap's pixels: the first word starts near where GDI drew it.
    ASSERT_FALSE(result.lines[0].words.empty());
    EXPECT_NEAR(result.lines[0].words[0].x, 56, 30);
}
