#pragma once
#include "Engine.h"
#include "docs/DocModel.h"

#include <stop_token>
#include <string>

// Documents: DOCX, Markdown, HTML, TXT, RTF, RTFD, PDF and Pages in; DOCX, Markdown, HTML,
// TXT, EPUB and PDF out, all through one document model (DocModel.h). One FeatureModule, one
// action, "docs.convert".
namespace sd::docs
{
    FeatureModule Module();

    enum class DocInput
    {
        None,
        Docx,
        Markdown,
        Text,
        Html,
        Rtf,
        Rtfd,    // a folder: TXT.rtf plus its pictures
        Pdf,     // reflowed text and pictures (the PDF module's reader); only to DOCX, EPUB and HTML
        Pages,   // Apple Pages: through the preview PDF older .pages files carry
    };
    DocInput InputOf(const std::wstring& path);

    // Output formats of docs.convert.
    struct DocTarget
    {
        const char* id;           // "docx"
        const wchar_t* extension; // L".docx"
    };
    const DocTarget* FindDocTarget(std::string_view id);
    // Does converting `input` to `target` make sense (a real change, and a path we support)?
    bool TargetFits(DocInput input, std::string_view target);

    // Reads any supported input into the model. RTF goes through RichEdit on the calling thread;
    // PDF and Pages through the PDF module's text reader (PdfSource.h).
    bool ReadDocument(const std::wstring& path, DocInput input, Document& doc, std::stop_token stop, std::wstring& error);

    // The document as PDF pages, laid out as Convert's PDF does it: our own layout printed through
    // Microsoft Print to PDF (PdfPrint.h), or a Pages file's preview PDF as it is. Convert writes
    // these bytes as "name (converted).pdf"; Edit opens them in the PDF page editor. Any worker.
    bool DocumentPdf(const std::wstring& source, std::string& pdf, std::stop_token stop, const ProgressFn& progress, std::wstring& error);
}
