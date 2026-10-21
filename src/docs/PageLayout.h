#pragma once
#include "docs/DocModel.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Page layout for documents and spreadsheets printed to PDF: line breaking, tables, images and
// pagination, in points (1/72 inch), y growing down from the top of the page. Pure: text is measured
// through ITextMeasure (DirectWrite in the app, fixed metrics in the tests), and the result is a list
// of positioned items per page that PdfPrint.cpp draws.
namespace sd::docs
{
    struct FontStyle
    {
        float size = 11;
        bool bold = false;
        bool italic = false;
        bool mono = false;
        bool operator==(const FontStyle&) const = default;
    };

    struct FontMetrics
    {
        float ascent = 0;    // points above the baseline
        float descent = 0;   // points below it
        float lineGap = 0;
    };

    class ITextMeasure
    {
    public:
        virtual ~ITextMeasure() = default;
        virtual float Width(std::wstring_view text, const FontStyle& font) = 0;   // advance width in points
        virtual FontMetrics Metrics(const FontStyle& font) = 0;
    };

    enum class ItemKind
    {
        Text,    // x = left, y = baseline
        Rect,    // x, y = top left; w, h; fill and/or stroke
        Line,    // from (x, y) to (x + w, y + h)
        Image,   // x, y = top left; w, h; image = index into Document::images
    };

    inline constexpr uint32_t kNoColor = 0xFFFFFFFF;

    struct PageItem
    {
        ItemKind kind = ItemKind::Text;
        float x = 0, y = 0, w = 0, h = 0;
        std::wstring text;
        FontStyle font;
        uint32_t color = 0x000000;      // 0xRRGGBB: text or stroke colour
        uint32_t fill = kNoColor;       // Rect fill
        float lineWidth = 0.5f;
        bool underline = false;
        int image = -1;
    };

    struct Page
    {
        std::vector<PageItem> items;
    };

    struct PageSetup
    {
        float width = 595.28f;    // A4 portrait
        float height = 841.89f;
        float margin = 56.7f;     // 2 cm all round
        float footer = 20;        // room under the text for the page number
        bool pageNumbers = true;
    };

    // ---- Line breaking ------------------------------------------------------------------------------

    struct Fragment
    {
        std::wstring text;
        FontStyle font;
        uint32_t color = 0x000000;
        bool underline = false;
        bool lineBreak = false;     // a hard break (text ignored)
        int image = -1;             // an atomic picture of imageW x imageH points
        float imageW = 0, imageH = 0;
    };

    struct Line
    {
        std::vector<PageItem> items;   // x from the line's start, y = 0 at the baseline (images: top = -h)
        float width = 0;
        float ascent = 0, descent = 0, gap = 0;
        float Height() const { return ascent + descent + gap; }
    };

    // Greedy line breaking: breaks at spaces (and between CJK characters), splits a word only when
    // it is wider than the whole line, drops spaces at line starts and ends. `keepSpaces` keeps
    // every space (code blocks: indentation matters).
    std::vector<Line> BreakLines(const std::vector<Fragment>& fragments, float maxWidth, ITextMeasure& measure, bool keepSpaces = false);

    // ---- Documents ----------------------------------------------------------------------------------

    struct LayoutOptions
    {
        PageSetup page;
        float bodySize = 11;
        bool repeatTableHeader = true;
    };

    // Lays out a whole document: headings, paragraphs, lists, quotes, code, rules, tables (header
    // row repeated on each page), images (scaled to fit) and page numbers.
    std::vector<Page> LayoutDocument(const Document& doc, const LayoutOptions& options, ITextMeasure& measure);

    // Column widths for a table that must fit `available` points: each column wants its natural
    // (one-line) width; when they don't fit, every column keeps at least its longest word (or an
    // equal share, whichever is smaller) and the rest is shared in proportion to natural width.
    std::vector<float> FitColumns(const std::vector<float>& natural, const std::vector<float>& minimum, float available);
}
