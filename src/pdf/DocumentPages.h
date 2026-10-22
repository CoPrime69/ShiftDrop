#pragma once
#include "Engine.h"
#include "officebridge/OfficeBridge.h"

#include <stop_token>
#include <string>

// Edit for any document: a Word file, a spreadsheet, a deck, Markdown, HTML, text... is laid out as
// PDF pages the way the Convert wheel makes its PDF, and those pages open in the PDF page editor
// (Organize, Redact, Annotate, Crop, Light, Fill and sign). Saving writes "name (edited).pdf" next
// to the document; the document itself is never touched.
namespace sd::pdf
{
    // How Edit gets a file's pages.
    enum class PageSource
    {
        None,          // not a document Edit opens (pictures have editors of their own)
        Pdf,           // its own pages
        Office,        // Microsoft Office or LibreOffice, as it prints (Convert's Exact PDF)
        Document,      // our own layout of the document model (Convert's PDF of a document)
        Spreadsheet,   // our own tables (Convert's PDF of a spreadsheet; long sheets ask first)
    };
    // Office files take the office app when one is installed, so the pages look as they do there;
    // without one, the formats we read ourselves take our own layout, and the rest have no Edit.
    PageSource PageSourceOf(const std::wstring& path, const office::Apps& apps);

    // Lays `source` out as a new PDF at `pdf`. Any worker thread; an STA one (JobLane::Office) for
    // PageSource::Office. The errors are Convert's own, word for word.
    office::Outcome LayOutPages(const std::wstring& source, const office::Apps& apps, const std::wstring& pdf, std::stop_token stop,
                                const ProgressFn& progress);
}
