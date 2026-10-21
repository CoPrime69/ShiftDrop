#pragma once
#include "docs/DocModel.h"

#include <stop_token>
#include <string>

// PDF as a document source (on top of the PDF engine in src/pdf): the PDF module reads each page's text
// (its text layer, or Windows OCR for scans) and rebuilds paragraphs, headings and bullets as
// Markdown, which becomes the document model. Reflowed text only: page layout and pictures are not
// kept. Kept in its own file because it needs QPDF.
namespace sd::docs
{
    bool ReadPdfDocument(const std::wstring& path, Document& doc, std::stop_token stop, std::wstring& error);

    // Apple Pages: older files carry QuickLook/Preview.pdf (a full PDF of the document). Returns its
    // bytes, or false with `error` when the file has none (newer Pages saves only a JPEG preview).
    bool PagesPreviewPdf(const std::wstring& path, std::string& pdf, std::wstring& error);
}
