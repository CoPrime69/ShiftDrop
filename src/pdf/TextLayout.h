#pragma once
#include "ocr/Ocr.h"

#include <string>
#include <vector>

// From positioned pieces of text (a PDF's text layer, or OCR) to plain text and Markdown. Pure
// functions, no PDF or Windows types, so the heuristics are unit-tested on their own.
namespace sd::pdf
{
    // A run of text from one show-text operator, in the page's reading frame: x to the right, y up,
    // both in points; `y` is the baseline.
    struct TextSpan
    {
        double x = 0, y = 0;
        double endX = 0;   // where the next glyph would go
        double size = 0;   // font size as it appears on the page
        std::wstring text;
    };

    struct TextLine
    {
        double x = 0, y = 0, endX = 0, size = 0;
        std::wstring text;
    };

    using PageLines = std::vector<TextLine>;

    // Spans in content order -> lines. A span joins the line before it when it sits on the same
    // baseline and does not jump back left; a space goes in where the gap is wider than a sliver
    // of the font size.
    PageLines JoinSpans(const std::vector<TextSpan>& spans);

    // OCR lines -> lines in points: `scale` points per bitmap pixel, y flipped so it grows upward.
    PageLines LinesFromOcr(const ocr::Result& ocr, double scale);

    // Plain text: the lines as they are, a blank line between paragraphs and between pages. CRLF.
    std::wstring ToText(const std::vector<PageLines>& pages);

    // Markdown: paragraphs re-flowed into one line each (hyphenated line ends rejoined), headings
    // from font size (#, ##, ### for text clearly larger than the body), bullets as "- ". LF.
    std::wstring ToMarkdown(const std::vector<PageLines>& pages);

    // Non-space characters across the pages: decides whether a page "has text" or needs OCR.
    size_t VisibleChars(const PageLines& lines);
}
