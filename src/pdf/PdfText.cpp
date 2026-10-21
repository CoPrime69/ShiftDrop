#include "pdf/PdfText.h"
#include "Log.h"
#include "Utf.h"
#include "ocr/TextLayer.h"
#include "pdf/PdfContent.h"
#include "pdf/PdfDoc.h"

#include <qpdf/QPDFPageDocumentHelper.hh>

#include <cmath>
#include <set>

namespace sd::pdf
{
    namespace
    {
        int RotationOf(QPDFPageObjectHelper& page)
        {
            auto rotate = page.getAttribute("/Rotate", false);
            const int r = rotate.isInteger() ? static_cast<int>(rotate.getIntValue()) : 0;
            return ((r % 360) + 360) % 360 / 90 * 90;
        }

        double PageArea(QPDFPageObjectHelper& page)
        {
            auto box = page.getCropBox(false, false);
            double v[4] = { 0, 0, 612, 792 };
            if (box.isArray() && box.getArrayNItems() == 4)
                for (int i = 0; i < 4; ++i)
                    if (box.getArrayItem(i).isNumber())
                        v[i] = box.getArrayItem(i).getNumericValue();
            const double area = std::abs((v[2] - v[0]) * (v[3] - v[1]));
            return area > 1 ? area : 612.0 * 792.0;
        }

        long long IntKey(QPDFObjectHandle dict, const char* key)
        {
            auto v = dict.getKey(key);
            return v.isInteger() ? v.getIntValue() : 0;
        }

        // The page's pictures worth keeping (PdfText::pictures), decoded into `out.images`.
        void KeepPictures(QPDFPageObjectHelper& page, size_t index, std::vector<DrawnImage>& drawn, const PageLines& lines, bool ocrPage,
                          std::set<QPDFObjGen>& seen, PdfText& out)
        {
            const double area = PageArea(page);
            // A scanned page OCR just read: its pictures are the scan, now text.
            double covered = 0;
            for (const auto& d : drawn)
                covered += (d.right - d.left) * (d.top - d.bottom);
            if (ocrPage && covered >= 0.6 * area)
                return;
            auto& kept = out.pictures[index];
            for (auto& d : drawn)
            {
                if (!d.image.isStream() || !seen.insert(d.image.getObjGen()).second)
                    continue;   // drawn before: once is enough (a logo on every page)
                const double w = d.right - d.left, h = d.top - d.bottom;
                auto dict = d.image.getDict();
                if (w < 12 || h < 12 || IntKey(dict, "/Width") < 16 || IntKey(dict, "/Height") < 16)
                    continue;   // a rule, a bullet, an icon
                if (w * h >= 0.8 * area && !lines.empty())
                {
                    // A page-sized picture under most of the page's text: a scan with its text layer, or a
                    // page background.
                    size_t over = 0;
                    for (const auto& line : lines)
                        if (line.x >= d.left - 1 && line.endX <= d.right + 1 && line.y >= d.bottom && line.y <= d.top)
                            ++over;
                    if (2 * over >= lines.size())
                        continue;
                }
                ImageFile file;
                std::string why;
                if (!ImageToFile(d.image, file, why))
                {
                    log::Info(L"pdf pictures: page {} picture left out: {}", index + 1, Widen(why));
                    continue;
                }
                kept.push_back(PagePicture{ d.left, d.bottom, d.right, d.top, static_cast<int>(out.images.size()) });
                out.images.push_back(std::move(file));
            }
        }
    }

    bool RenderForOcr(const IRenderedPdf& doc, uint32_t page, Bitmap& out, std::wstring& error)
    {
        double w = 0, h = 0;
        if (!doc.PageSize(page, w, h))
        {
            error = L"cannot read the page size";
            return false;
        }
        uint32_t pw = 0, ph = 0;
        PixelSize(w, h, 300.0, 4000, pw, ph);
        return doc.Render(page, pw, ph, out, error);
    }

    bool ReadPdfText(const std::wstring& path, bool useOcr, std::stop_token stop, const ProgressFn& progress, PdfText& out,
                     std::wstring& error, bool pictures)
    {
        out = PdfText{};
        auto pdf = OpenPdf(path, error);
        if (!pdf)
            return false;
        std::unique_ptr<IRenderedPdf> rendered;
        bool ocrReady = useOcr && ocr::Available(&out.ocrProblem);
        try
        {
            auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
            if (pictures)
                out.pictures.resize(pages.size());
            std::set<QPDFObjGen> seen;
            for (size_t i = 0; i < pages.size(); ++i)
            {
                if (stop.stop_requested())
                {
                    error = kCancelled;
                    return false;
                }
                std::vector<DrawnImage> drawn;
                PageLines lines = ExtractPageLines(pages[i], pictures ? &drawn : nullptr);
                bool ocrPage = false;
                if (VisibleChars(lines) < kMinTextChars && useOcr)
                {
                    if (ocrReady && !rendered)
                    {
                        rendered = OpenForRendering(path, out.ocrProblem);
                        ocrReady = rendered != nullptr;
                    }
                    Bitmap bitmap;
                    ocr::Result result;
                    std::wstring why;
                    if (ocrReady && RenderForOcr(*rendered, static_cast<uint32_t>(i), bitmap, why) && ocr::Recognize(bitmap, result, why) &&
                        !result.lines.empty())
                    {
                        double pageW = 0, pageH = 0;
                        rendered->PageSize(static_cast<uint32_t>(i), pageW, pageH);
                        lines = LinesFromOcr(result, bitmap.width ? pageW / bitmap.width : 1.0);
                        ++out.ocrPages;
                        ocrPage = true;
                    }
                    else
                    {
                        if (!why.empty())
                            log::Warn(L"pdf text: OCR of page {} failed: {}", i + 1, why);
                        if (VisibleChars(lines) == 0)
                            ++out.pagesWithout;
                    }
                }
                if (pictures)
                    KeepPictures(pages[i], i, drawn, lines, ocrPage, seen, out);
                out.pages.push_back(std::move(lines));
                if (progress)
                    progress(static_cast<double>(i + 1) / static_cast<double>(pages.size()));
            }
        }
        catch (const std::exception& e)
        {
            error = QpdfError(e);
            return false;
        }
        return true;
    }

    QPDFObjectHandle PdfTextLayer::Font()
    {
        if (m_font.isInitialized())
            return m_font;
        using H = QPDFObjectHandle;
        auto bbox = H::newArray({ H::newInteger(0), H::newInteger(0), H::newInteger(500), H::newInteger(1000) });
        auto descriptor = m_pdf.makeIndirectObject(H::newDictionary({
            { "/Type", H::newName("/FontDescriptor") },
            { "/FontName", H::newName("/SDGlyphless") },
            { "/Flags", H::newInteger(4) },
            { "/FontBBox", bbox },
            { "/ItalicAngle", H::newInteger(0) },
            { "/Ascent", H::newInteger(1000) },
            { "/Descent", H::newInteger(0) },
            { "/CapHeight", H::newInteger(1000) },
            { "/StemV", H::newInteger(80) },
        }));
        auto system = H::newDictionary({
            { "/Registry", H::newString("Adobe") },
            { "/Ordering", H::newString("Identity") },
            { "/Supplement", H::newInteger(0) },
        });
        auto cid = m_pdf.makeIndirectObject(H::newDictionary({
            { "/Type", H::newName("/Font") },
            { "/Subtype", H::newName("/CIDFontType2") },
            { "/BaseFont", H::newName("/SDGlyphless") },
            { "/CIDSystemInfo", system },
            { "/FontDescriptor", descriptor },
            { "/DW", H::newInteger(500) },
            { "/CIDToGIDMap", H::newName("/Identity") },
        }));
        // QPDFWriter deflates streams that have no filter.
        auto toUnicode = m_pdf.newStream(ocr::IdentityToUnicodeCMap());
        m_font = m_pdf.makeIndirectObject(H::newDictionary({
            { "/Type", H::newName("/Font") },
            { "/Subtype", H::newName("/Type0") },
            { "/BaseFont", H::newName("/SDGlyphless") },
            { "/Encoding", H::newName("/Identity-H") },
            { "/DescendantFonts", H::newArray({ cid }) },
            { "/ToUnicode", toUnicode },
        }));
        return m_font;
    }

    bool PdfTextLayer::AddToPage(QPDFPageObjectHelper& page, const ocr::Result& result)
    {
        auto box = page.getCropBox(false, false);
        double crop[4] = { 0, 0, 612, 792 };
        if (box.isArray() && box.getArrayNItems() == 4)
            for (int i = 0; i < 4; ++i)
                if (box.getArrayItem(i).isNumber())
                    crop[i] = box.getArrayItem(i).getNumericValue();
        const auto words = ocr::PlaceOnPdfPage(result, crop, RotationOf(page));
        const std::string ops = ocr::TextLayerOps(words, "SDOcr");
        if (ops.empty())
            return false;
        // q ... Q around the original content, so whatever state it leaves does not move our text.
        page.addPageContents(m_pdf.newStream("q\n"), true);
        page.addPageContents(m_pdf.newStream("Q\n" + ops), false);
        auto resources = page.getAttribute("/Resources", true);
        if (!resources.isDictionary())
        {
            resources = QPDFObjectHandle::newDictionary();
            page.getObjectHandle().replaceKey("/Resources", resources);
        }
        auto fonts = resources.getKey("/Font");
        if (!fonts.isDictionary())
        {
            fonts = QPDFObjectHandle::newDictionary();
            resources.replaceKey("/Font", fonts);
        }
        fonts.replaceKey("/SDOcr", Font());
        return true;
    }
}
