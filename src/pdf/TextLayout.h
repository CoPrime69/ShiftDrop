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

    // Where the gutter of a two-column page is, from the gap in the lines' x positions: a vertical
    // strip in the middle part of the page that nearly no line crosses, with a column of text on
    // each side, the two side by side over a good part of the page and both mostly filled to their
    // width. False for a single-column page. `left` and `right` are the strip's edges in points.
    bool FindGutter(const PageLines& lines, double& left, double& right);

    // The page's lines in reading order, as runs that never share a paragraph. On a two-column page
    // the lines crossing the gutter (a title, a full-width heading or caption, a page number) cut
    // the page into bands, read top to bottom: in each band the left column top to bottom, then
    // the right. A column can split into columns again (three-column pages). Any other page is one
    // run in content order, as the PDF draws it.
    std::vector<PageLines> ReadingOrder(const PageLines& lines);

    // Plain text: the lines in reading order, a blank line between paragraphs and between pages. CRLF.
    std::wstring ToText(const std::vector<PageLines>& pages);

    // Markdown: paragraphs re-flowed into one line each (hyphenated line ends rejoined), headings
    // from font size (#, ##, ### for text clearly larger than the body), bullets as "- ". Reading
    // order as ToText. LF.
    std::wstring ToMarkdown(const std::vector<PageLines>& pages);

    // A picture on a page, in the same frame as its text (points, y up). `id` is the caller's.
    struct PagePicture
    {
        double left = 0, bottom = 0, right = 0, top = 0;
        int id = -1;
    };

    // ToMarkdown with pictures: the Markdown in parts, each either text or one picture, with every
    // picture between the paragraphs it sat between on its page (by its top edge; on a two-column
    // page in the column it sits in, or between the bands when it is as wide as both).
    // `pictures[i]` are page i's; missing pages have none.
    struct MarkdownPart
    {
        std::wstring markdown;   // empty for a picture
        int picture = -1;        // PagePicture::id, or -1 for text
    };
    std::vector<MarkdownPart> ToMarkdownParts(const std::vector<PageLines>& pages, const std::vector<std::vector<PagePicture>>& pictures);

    // Non-space characters across the pages: decides whether a page "has text" or needs OCR.
    size_t VisibleChars(const PageLines& lines);
}
