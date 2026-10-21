#pragma once
#include <string>
#include <vector>

// The document model every document converter goes through: readers (DOCX, Markdown, HTML,
// TXT, RTF, and later PDF) fill it, writers (DOCX, Markdown, HTML, TXT, EPUB, PDF) read it. It holds
// what survives a trip between formats: headings, paragraphs, bold/italic/strike/code, links, lists,
// quotes, code blocks, tables and images. Text is UTF-8.
namespace sd::docs
{
    struct Span
    {
        std::string text;          // for an image: its alt text
        bool bold = false;
        bool italic = false;
        bool strike = false;
        bool code = false;         // inline code (monospace)
        std::string link;          // hyperlink target, empty if none
        int image = -1;            // index into Document::images: this span is a picture
        bool lineBreak = false;    // a hard line break (text is empty)

        bool SameStyle(const Span& o) const
        {
            return bold == o.bold && italic == o.italic && strike == o.strike && code == o.code && link == o.link;
        }
    };

    enum class BlockKind
    {
        Paragraph,
        Heading,    // level 1..6
        ListItem,   // level = nesting depth (0 = top), ordered = numbered
        Quote,      // a paragraph inside a block quote
        Code,       // a preformatted block: spans hold the text verbatim, lines split by '\n'
        Table,
        Rule,       // a horizontal rule / thematic break
    };

    struct TableCell
    {
        std::vector<Span> spans;
    };

    struct Block
    {
        BlockKind kind = BlockKind::Paragraph;
        int level = 0;
        bool ordered = false;
        int start = 1;                       // first number of an ordered list (on its first item)
        std::vector<Span> spans;
        std::vector<std::vector<TableCell>> rows;   // Table only
        bool headerRow = false;              // Table: the first row is a header
    };

    struct Image
    {
        std::string name;      // file name inside packages: "image1.png"
        std::string mime;      // "image/png"
        std::string data;      // the encoded bytes
        int width = 0;         // pixels, 0 if unknown
        int height = 0;
    };

    struct Document
    {
        std::string title;     // may be empty (writers fall back to the first heading or the file name)
        std::string language;  // BCP 47, may be empty
        std::vector<Block> blocks;
        std::vector<Image> images;
    };

    // The concatenated text of spans (images contribute their alt text, line breaks '\n').
    std::string PlainText(const std::vector<Span>& spans);
    // Joins neighbouring spans of the same style and drops empty ones.
    void NormalizeSpans(std::vector<Span>& spans);
    // The title to use: Document::title, else the first heading, else `fallback`.
    std::string TitleOf(const Document& doc, const std::string& fallback);
    // Image type from its first bytes: "image/png", "image/jpeg", "image/gif", "image/bmp",
    // "image/webp", "image/svg+xml", or empty. `extension` gets ".png" etc.
    std::string SniffImage(const std::string& data, std::string* extension = nullptr);
    // Width and height of a PNG, JPEG or GIF from its header (no decoding); false if unknown.
    bool ImageDimensions(const std::string& data, int& width, int& height);
    // Adds an image to the document and returns its index; names it "image<N><ext>".
    int AddImage(Document& doc, std::string data);
}
