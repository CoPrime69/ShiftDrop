#pragma once
#include "Engine.h"
#include "ocr/Ocr.h"
#include "pdf/PageRenderer.h"
#include "pdf/TextLayout.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <stop_token>
#include <string>
#include <vector>

// Text out of PDFs, and text into scanned ones.
namespace sd::pdf
{
    // A page with fewer visible characters than this in its text layer is treated as a picture of
    // text (a scan) and read with OCR instead. A few stray characters (a page number stamped on a
    // scan) don't count as a text layer.
    inline constexpr size_t kMinTextChars = 8;

    struct PdfText
    {
        std::vector<PageLines> pages;
        size_t ocrPages = 0;        // pages whose text came from OCR
        size_t pagesWithout = 0;    // pages that needed OCR but got none (unavailable or nothing found)
        std::wstring ocrProblem;    // why OCR could not run, if it couldn't
    };

    // Every page's text: the text layer where there is one, OCR (Windows.Data.Pdf render + Windows
    // OCR) where there isn't and `useOcr` is set.
    bool ReadPdfText(const std::wstring& path, bool useOcr, std::stop_token stop, const ProgressFn& progress, PdfText& out,
                     std::wstring& error);

    // Renders one page for OCR at 300 dpi (at most 4000 px on the long side, which Windows OCR reads
    // well and quickly).
    bool RenderForOcr(const IRenderedPdf& doc, uint32_t page, Bitmap& out, std::wstring& error);

    // Writes invisible OCR text onto pages of an open document (searchable PDF). One shared font.
    class PdfTextLayer
    {
    public:
        explicit PdfTextLayer(QPDF& pdf) : m_pdf(pdf) {}
        // False when the result has no words to place.
        bool AddToPage(QPDFPageObjectHelper& page, const ocr::Result& result);

    private:
        QPDFObjectHandle Font();
        QPDF& m_pdf;
        QPDFObjectHandle m_font;
    };
}
