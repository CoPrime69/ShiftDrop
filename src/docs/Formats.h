#pragma once
#include "docs/DocModel.h"

#include <string>
#include <string_view>

// Readers and writers between the document model (DocModel.h) and each file format. All of them
// are plain functions on bytes in memory, so the tests can round-trip without touching disk.
namespace sd::docs
{
    // ---- Readers ----------------------------------------------------------------------------------
    // `baseDir` resolves relative image paths (Markdown, HTML); empty skips local images. Remote
    // images (http:, https:) are never fetched: ShiftDrop is offline, so they keep their alt text.
    void ReadMarkdown(std::string_view utf8, const std::wstring& baseDir, Document& doc);
    void ReadHtml(std::string_view utf8, const std::wstring& baseDir, Document& doc);
    void ReadPlainText(std::string_view utf8, Document& doc);
    bool ReadDocx(std::string bytes, Document& doc, std::string& error);

    // An image reference from Markdown or HTML: a data: URI or a local path relative to `baseDir`.
    // Returns its index in doc.images, or -1 (remote, missing, not an image).
    int LoadImageRef(std::string_view src, const std::wstring& baseDir, Document& doc);
    // Base64 (data: URIs); skips whitespace, stops at the first invalid character.
    std::string Base64Decode(std::string_view s);
    std::string Base64Encode(std::string_view s);
    // Percent-decoding of URL paths ("my%20file.png").
    std::string PercentDecode(std::string_view s);

    // ---- Writers ----------------------------------------------------------------------------------
    // `imageDir` is where the images sit relative to the output ("images/"); writers emit links to
    // imageDir + Image::name and the caller writes the files.
    std::string WriteMarkdown(const Document& doc, const std::string& imageDir);

    struct HtmlOptions
    {
        std::string imageDir = "images/";
        bool xhtml = false;      // XML syntax for EPUB: <br/>, the XHTML namespace, an XML declaration
        bool standalone = true;  // a whole document with <head>, else only the body's content
        std::string title;       // <title>; empty: TitleOf(doc)
        std::string cssHref;     // a linked stylesheet instead of the built-in <style>
        std::string language;    // <html lang>
    };
    std::string WriteHtml(const Document& doc, const HtmlOptions& options);
    // The body markup of the given blocks only (EPUB chapters), with id="h<N>" on headings,
    // numbered from `firstHeadingId`.
    std::string WriteHtmlBlocks(const Document& doc, size_t first, size_t last, const HtmlOptions& options, int firstHeadingId);

    // The stylesheet standalone HTML embeds (EPUB ships it as style.css).
    const char* DefaultCss();

    std::string WritePlainText(const Document& doc);   // UTF-8, CRLF line ends
    bool WriteDocx(const Document& doc, std::string& out);
    // EPUB 3 (with an EPUB 2 toc.ncx for older readers). Chapters split at level-1 headings when
    // there are several. `fallbackTitle` is used when the document has no title or heading.
    bool WriteEpub(const Document& doc, const std::string& fallbackTitle, std::string& out);
}
