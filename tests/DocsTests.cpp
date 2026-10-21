#include "PdfTestUtil.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "docs/Bytes.h"
#include "docs/DocsModule.h"
#include "docs/Formats.h"
#include "docs/PageLayout.h"
#include "docs/Paper.h"
#include "docs/PdfPrint.h"
#include "docs/Rtf.h"
#include "docs/Xml.h"
#include "docs/Zip.h"

#include <pugixml.hpp>

#include <cmath>
#include <cstring>

using namespace sd;
using namespace sd::docs;
using namespace sd::test;

namespace
{
    // A compact, comparable picture of a document: one line per block.
    std::string Inline(const Document& doc, const std::vector<Span>& spans)
    {
        std::string out;
        for (const auto& s : spans)
        {
            if (s.lineBreak)
            {
                out += "\\n";
                continue;
            }
            std::string t = s.image >= 0 ? std::format("![{}]({}x{})", s.text, doc.images[static_cast<size_t>(s.image)].width,
                                                       doc.images[static_cast<size_t>(s.image)].height)
                                         : s.text;
            if (s.code)
                t = "`" + t + "`";
            if (s.strike)
                t = "~~" + t + "~~";
            if (s.italic)
                t = "_" + t + "_";
            if (s.bold)
                t = "**" + t + "**";
            if (!s.link.empty())
                t = "[" + t + "](" + s.link + ")";
            out += t;
        }
        return out;
    }

    std::string Describe(const Document& doc)
    {
        std::string out;
        for (const auto& b : doc.blocks)
        {
            switch (b.kind)
            {
            case BlockKind::Heading: out += std::format("H{} ", b.level); break;
            case BlockKind::Paragraph: out += "P "; break;
            case BlockKind::ListItem: out += std::format("L{}{} ", b.level, b.ordered ? std::format("#{}", b.start) : std::string("*")); break;
            case BlockKind::Quote: out += "Q "; break;
            case BlockKind::Code: out += "C "; break;
            case BlockKind::Rule: out += "R"; break;
            case BlockKind::Table:
                out += b.headerRow ? "T+ " : "T ";
                for (const auto& row : b.rows)
                {
                    for (const auto& cell : row)
                        out += Inline(doc, cell.spans) + "|";
                    out += "/";
                }
                break;
            }
            out += b.kind == BlockKind::Code ? b.spans.empty() ? "" : b.spans[0].text : Inline(doc, b.spans);
            out += "\n";
        }
        return out;
    }

    // Everything the model holds, built by hand.
    Document Sample(const std::string& png)
    {
        Document doc;
        doc.title = "Sample";
        const auto para = [&](BlockKind kind, std::vector<Span> spans, int level = 0, bool ordered = false, int start = 1) {
            Block b;
            b.kind = kind;
            b.spans = std::move(spans);
            b.level = level;
            b.ordered = ordered;
            b.start = start;
            doc.blocks.push_back(std::move(b));
        };
        const auto text = [](std::string t) { return Span{ std::move(t) }; };
        Span bold = text("bold");
        bold.bold = true;
        Span italic = text("italic");
        italic.italic = true;
        Span strike = text("gone");
        strike.strike = true;
        Span code = text("x = 1");
        code.code = true;
        Span link = text("a link");
        link.link = "https://example.com/a?b=c&d=e";

        para(BlockKind::Heading, { text("Title & more") }, 1);
        para(BlockKind::Paragraph, { text("Plain "), bold, text(", "), italic, text(", "), strike, text(" and "), code, text(".") });
        para(BlockKind::Paragraph, { text("See "), link, text(" <now>.") });
        para(BlockKind::Heading, { text("Lists") }, 2);
        para(BlockKind::ListItem, { text("one") }, 0, false);
        para(BlockKind::ListItem, { text("nested") }, 1, false);
        para(BlockKind::ListItem, { text("two") }, 0, false);
        para(BlockKind::Paragraph, { text("Between lists.") });
        para(BlockKind::ListItem, { text("third") }, 0, true, 3);
        para(BlockKind::ListItem, { text("fourth") }, 0, true, 4);
        para(BlockKind::Quote, { text("A quote.") });
        Block codeBlock;
        codeBlock.kind = BlockKind::Code;
        codeBlock.spans = { text("int main()\n{\n    return 0;\n}") };
        doc.blocks.push_back(codeBlock);

        Block table;
        table.kind = BlockKind::Table;
        table.headerRow = true;
        table.rows = { { TableCell{ { text("Name") } }, TableCell{ { text("Qty") } } },
                       { TableCell{ { text("Apples") } }, TableCell{ { text("3") } } },
                       { TableCell{ { bold } }, TableCell{} } };
        doc.blocks.push_back(table);

        const int img = AddImage(doc, png);
        Span picture;
        picture.text = "A picture";
        picture.image = img;
        para(BlockKind::Paragraph, { picture });
        Block rule;
        rule.kind = BlockKind::Rule;
        doc.blocks.push_back(rule);
        para(BlockKind::Paragraph, { text("Ünïcödé — 漢字 😀") });
        return doc;
    }

    // Fixed metrics for layout tests: every character is 0.5 em wide, ascent 0.8, descent 0.2.
    class FakeMeasure final : public ITextMeasure
    {
    public:
        float Width(std::wstring_view text, const FontStyle& font) override { return static_cast<float>(text.size()) * font.size * 0.5f; }
        FontMetrics Metrics(const FontStyle& font) override { return { font.size * 0.8f, font.size * 0.2f, 0 }; }
    };

    uint16_t U16At(const std::string& s, size_t at) { return static_cast<uint16_t>(static_cast<uint8_t>(s[at]) | static_cast<uint8_t>(s[at + 1]) << 8); }
}

TEST(Docs, XmlNamesMatchByTheirLocalPart)
{
    pugi::xml_document xml;
    ASSERT_TRUE(xml.load_string(R"(<w:p xmlns:w="x"><w:r w:val="on" empty=""/><r2/></w:p>)"));
    const auto p = xml.first_child();
    EXPECT_EQ(Local(p.name()), "p");
    EXPECT_EQ(Local("plain"), "plain");
    const auto r = Child(p, "r");
    ASSERT_TRUE(r);
    EXPECT_FALSE(Child(p, "missing"));
    EXPECT_EQ(Attr(r, "val"), "on");
    EXPECT_EQ(Attr(r, "missing"), "");
    ASSERT_NE(AttrOrNull(r, "empty"), nullptr);   // present but empty, unlike absent
    EXPECT_STREQ(AttrOrNull(r, "empty"), "");
    EXPECT_EQ(AttrOrNull(r, "missing"), nullptr);
}

TEST(Docs, HelpersDecodeAndSniff)
{
    EXPECT_EQ(Base64Decode("SGVs bG8=\n"), "Hello");
    EXPECT_EQ(Base64Encode("Hello"), "SGVsbG8=");
    EXPECT_EQ(Base64Decode(Base64Encode(std::string("\0\xFF\x10", 3))), std::string("\0\xFF\x10", 3));
    EXPECT_EQ(PercentDecode("my%20file%C3%A9.png"), "my fileé.png");
    EXPECT_EQ(DecodeEntity("amp"), "&");
    EXPECT_EQ(DecodeEntity("#233"), "é");
    EXPECT_EQ(DecodeEntity("#x1F600"), "😀");
    EXPECT_EQ(DecodeEntity("bogus"), "");
    EXPECT_EQ(DecodeEntity("lrm"), "\xE2\x80\x8E");
    EXPECT_EQ(DecodeEntities("a &amp; b&#233;&rlm; &bogus; & c"), "a & b\xC3\xA9\xE2\x80\x8F &bogus; & c");
    EXPECT_EQ(XmlEscape("a<b>&\"c'\x01"), "a&lt;b&gt;&amp;&quot;c&apos;");
    EXPECT_EQ(NormalizeNewlines("a\r\nb\rc\n"), "a\nb\nc\n");

    TempDir dir;
    const auto png = PngBytes(dir, 4, 3);
    std::string ext;
    EXPECT_EQ(SniffImage(png, &ext), "image/png");
    EXPECT_EQ(ext, ".png");
    int w = 0, h = 0;
    ASSERT_TRUE(ImageDimensions(png, w, h));
    EXPECT_EQ(w, 4);
    EXPECT_EQ(h, 3);
    EXPECT_EQ(SniffImage("GIF89a......"), "image/gif");
    EXPECT_EQ(SniffImage("plain text"), "");
}

TEST(Docs, ZipRoundTrip)
{
    ZipWriter w;
    ASSERT_TRUE(w.Add("mimetype", "application/epub+zip", true));
    ASSERT_TRUE(w.Add("Folder/Item.xml", std::string(5000, 'x')));
    std::string bytes;
    ASSERT_TRUE(w.Finish(bytes));
    ZipReader r;
    ASSERT_TRUE(r.Open(bytes));
    EXPECT_EQ(r.Names().size(), 2u);
    EXPECT_EQ(r.Read("folder/item.xml").value_or(""), std::string(5000, 'x'));   // any case
    EXPECT_FALSE(r.Read("missing").has_value());
    ZipReader bad;
    EXPECT_FALSE(bad.Open("not a zip"));
}

TEST(Docs, MarkdownReadsEveryBlock)
{
    TempDir dir;
    const auto png = PngBytes(dir, 4, 3);
    const std::string md =
        "# Title\n\nSome **bold**, *italic*, ~~gone~~ and `code` with a [link](https://example.com).\n\n"
        "- one\n  - nested\n- two\n\n3. third\n4. fourth\n\n- [x] done\n- [ ] todo\n\n> quoted\n\n"
        "```\nfn main() {}\n```\n\n| A | B |\n|---|---|\n| 1 | **2** |\n\n---\n\n"
        "Line one  \nline two\n\n![pic](data:image/png;base64," + Base64Encode(png) + ")\n\n![remote](https://example.com/x.png)\n";
    Document doc;
    ReadMarkdown(md, dir.path().wstring(), doc);
    EXPECT_EQ(Describe(doc),
              "H1 Title\n"
              "P Some **bold**, _italic_, ~~gone~~ and `code` with a [link](https://example.com).\n"
              "L0* one\nL1* nested\nL0* two\n"
              "L0#3 third\nL0#4 fourth\n"
              "L0* \xE2\x98\x91 done\nL0* \xE2\x98\x90 todo\n"
              "Q quoted\n"
              "C fn main() {}\n"
              "T+ A|B|/1|**2**|/\n"
              "R\n"
              "P Line one\\nline two\n"
              "P ![pic](4x3)\n"
              "P remote\n");
    ASSERT_EQ(doc.images.size(), 1u);   // the remote picture is never fetched
}

TEST(Docs, MarkdownToHtmlAndBack)
{
    TempDir dir;
    const auto doc = Sample(PngBytes(dir, 4, 3));
    HtmlOptions o;
    const auto html = WriteHtml(doc, o);
    EXPECT_NE(html.find("<!DOCTYPE html>"), std::string::npos);
    EXPECT_NE(html.find(">Title &amp; more</h1>"), std::string::npos);
    EXPECT_NE(html.find("<img src=\"images/image1.png\" alt=\"A picture\""), std::string::npos);
    EXPECT_NE(html.find("<ol start=\"3\">"), std::string::npos);

    // HTML -> model: the picture comes back from the images folder next to the page.
    std::filesystem::create_directories(dir.path() / L"images");
    WriteText(dir / L"images\\image1.png", doc.images[0].data);
    Document back;
    ReadHtml(html, dir.path().wstring(), back);
    EXPECT_EQ(Describe(back), Describe(doc));

    // Model -> Markdown -> model.
    const auto md = WriteMarkdown(doc, "images/");
    Document fromMd;
    ReadMarkdown(md, dir.path().wstring(), fromMd);
    EXPECT_EQ(Describe(fromMd), Describe(doc)) << md;
}

TEST(Docs, MarkdownEscapesWhatWouldBeMarkup)
{
    Document doc;
    Block b;
    b.spans = { Span{ "1. not a list, *not italic*, [not a link], # not a heading, a_b" } };
    doc.blocks.push_back(b);
    Block h;
    h.kind = BlockKind::Heading;
    h.level = 2;
    h.spans = { Span{ "Real heading" } };
    doc.blocks.push_back(h);
    Document back;
    ReadMarkdown(WriteMarkdown(doc, ""), {}, back);
    EXPECT_EQ(Describe(back), Describe(doc));
}

TEST(Docs, HtmlReaderToleratesRealPages)
{
    const std::string html =
        "<html><head><title>T</title><style>p{color:red}</style><script>var x = '<p>no</p>';</script></head><body>"
        "<h2 id=x>Head<br>ing</h2>"
        "<p>One&nbsp;two &amp; <span style=\"font-weight: 700\">bold</span> <span style='font-style:italic'>it</span>"
        " <b><i>both</i></b> <s>old</s> <code>c</code> <a href=\"https://a.example/\">link</a>"
        "<p>unclosed paragraph"
        "<ul><li>a<ul><li>b</li></ul></li><li>c</ul>"
        "<table><thead><tr><th>H1</th><th>H2</th></tr></thead><tbody><tr><td>1</td><td>2</td></tr></tbody></table>"
        "<pre>  keep\n    spaces</pre><blockquote><p>q</p></blockquote><hr><img src=\"http://remote/x.png\" alt=\"far\">"
        "</body></html>";
    Document doc;
    ReadHtml(html, {}, doc);
    EXPECT_EQ(doc.title, "T");
    const auto d = Describe(doc);
    EXPECT_EQ(d.find("var x"), std::string::npos);   // no script or style text
    EXPECT_EQ(d.find("no</p>"), std::string::npos);
    EXPECT_EQ(d.find("color:red"), std::string::npos);
    EXPECT_NE(d.find("H2 Head\\ning\n"), std::string::npos) << d;
    EXPECT_NE(d.find("P One\xC2\xA0two & **bold** _it_ **_both_** ~~old~~ `c` [link](https://a.example/)\n"), std::string::npos) << d;
    EXPECT_NE(d.find("P unclosed paragraph\n"), std::string::npos) << d;
    EXPECT_NE(d.find("L0* a\nL1* b\nL0* c\n"), std::string::npos) << d;
    EXPECT_NE(d.find("T+ H1|H2|/1|2|/\n"), std::string::npos) << d;
    EXPECT_NE(d.find("C   keep\n    spaces\n"), std::string::npos) << d;
    EXPECT_NE(d.find("Q q\nR\nP far\n"), std::string::npos) << d;
}

TEST(Docs, DocxRoundTrip)
{
    TempDir dir;
    const auto doc = Sample(PngBytes(dir, 4, 3));
    std::string bytes;
    ASSERT_TRUE(WriteDocx(doc, bytes));
    ZipReader zip;
    ASSERT_TRUE(zip.Open(bytes));
    for (const char* part : { "[Content_Types].xml", "_rels/.rels", "word/document.xml", "word/styles.xml", "word/numbering.xml",
                              "word/_rels/document.xml.rels", "word/media/image1.png", "docProps/core.xml" })
        EXPECT_TRUE(zip.Has(part)) << part;
    // Every XML part is well formed.
    for (const auto& name : zip.Names())
        if (name.ends_with(".xml") || name.ends_with(".rels"))
        {
            pugi::xml_document x;
            const auto xml = zip.Read(name).value_or("");
            EXPECT_TRUE(x.load_buffer(xml.data(), xml.size())) << name;
        }

    Document back;
    std::string error;
    ASSERT_TRUE(ReadDocx(bytes, back, error)) << error;
    EXPECT_EQ(back.title, "Sample");
    EXPECT_EQ(Describe(back), Describe(doc));
    ASSERT_EQ(back.images.size(), 1u);
    EXPECT_EQ(back.images[0].data, doc.images[0].data);

    Document none;
    EXPECT_FALSE(ReadDocx("PK not really", none, error));
}

TEST(Docs, DocxFromAnotherWriter)
{
    // A minimal document as other writers produce it: heading style by name, a numbered list
    // through numbering.xml, a hyperlink field, a tab and a soft break.
    ZipWriter w;
    w.Add("[Content_Types].xml", R"(<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"/>)");
    w.Add("_rels/.rels", R"(<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="r1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>)");
    w.Add("word/_rels/document.xml.rels", R"(<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rL" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink" Target="https://x.example/" TargetMode="External"/></Relationships>)");
    w.Add("word/styles.xml", R"(<?xml version="1.0"?><w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:style w:type="paragraph" w:styleId="Kop1"><w:name w:val="heading 1"/></w:style></w:styles>)");
    w.Add("word/numbering.xml", R"(<?xml version="1.0"?><w:numbering xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:abstractNum w:abstractNumId="0"><w:lvl w:ilvl="0"><w:start w:val="5"/><w:numFmt w:val="decimal"/></w:lvl></w:abstractNum><w:num w:numId="9"><w:abstractNumId w:val="0"/></w:num></w:numbering>)");
    w.Add("word/document.xml", R"(<?xml version="1.0"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><w:body>)"
                               R"(<w:p><w:pPr><w:pStyle w:val="Kop1"/></w:pPr><w:r><w:t>Hoofdstuk</w:t></w:r></w:p>)"
                               R"(<w:p><w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="9"/></w:numPr></w:pPr><w:r><w:t>first</w:t></w:r></w:p>)"
                               R"(<w:p><w:r><w:t xml:space="preserve">Go </w:t></w:r><w:hyperlink r:id="rL"><w:r><w:t>there</w:t></w:r></w:hyperlink>)"
                               R"(<w:r><w:tab/><w:t>tab</w:t><w:br/><w:t>next</w:t></w:r>)"
                               R"(<w:r><w:fldChar w:fldCharType="begin"/></w:r><w:r><w:instrText> HYPERLINK "https://y.example/" </w:instrText></w:r><w:r><w:fldChar w:fldCharType="separate"/></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>field</w:t></w:r><w:r><w:fldChar w:fldCharType="end"/></w:r></w:p>)"
                               R"(</w:body></w:document>)");
    std::string bytes;
    ASSERT_TRUE(w.Finish(bytes));
    Document doc;
    std::string error;
    ASSERT_TRUE(ReadDocx(bytes, doc, error)) << error;
    EXPECT_EQ(Describe(doc), "H1 Hoofdstuk\nL0#5 first\nP Go [there](https://x.example/)\ttab\\nnext[**field**](https://y.example/)\n");
}

TEST(Docs, EpubContainerIsValid)
{
    TempDir dir;
    auto doc = Sample(PngBytes(dir, 4, 3));
    Block h;
    h.kind = BlockKind::Heading;
    h.level = 1;
    h.spans = { Span{ "Second chapter" } };
    doc.blocks.push_back(h);
    Block p;
    p.spans = { Span{ "More." } };
    doc.blocks.push_back(p);

    std::string bytes;
    ASSERT_TRUE(WriteEpub(doc, "fallback", bytes));
    // OCF: the first entry is "mimetype", stored (method 0), with no extra field, so its content
    // sits at byte 38 for readers that sniff it.
    ASSERT_GT(bytes.size(), 60u);
    EXPECT_EQ(bytes.substr(0, 4), std::string("PK\x03\x04", 4));
    EXPECT_EQ(U16At(bytes, 8), 0);    // compression method: stored
    EXPECT_EQ(U16At(bytes, 26), 8);   // name length
    EXPECT_EQ(U16At(bytes, 28), 0);   // extra field length
    EXPECT_EQ(bytes.substr(30, 8), "mimetype");
    EXPECT_EQ(bytes.substr(38, 20), "application/epub+zip");

    ZipReader zip;
    ASSERT_TRUE(zip.Open(bytes));
    pugi::xml_document container;
    const auto containerXml = zip.Read("META-INF/container.xml").value_or("");
    ASSERT_TRUE(container.load_buffer(containerXml.data(), containerXml.size()));
    const std::string opfPath = container.select_node("//*[local-name()='rootfile']").node().attribute("full-path").value();
    EXPECT_EQ(opfPath, "OEBPS/content.opf");

    pugi::xml_document opf;
    const auto opfXml = zip.Read(opfPath).value_or("");
    ASSERT_TRUE(opf.load_buffer(opfXml.data(), opfXml.size()));
    const auto package = opf.child("package");
    EXPECT_STREQ(package.attribute("version").value(), "3.0");
    EXPECT_FALSE(opf.select_node("//*[local-name()='meta'][@property='dcterms:modified']").node().empty());
    EXPECT_STREQ(opf.select_node("//*[local-name()='title']").node().child_value(), "Sample");
    // Every manifest item exists and every XHTML file is well-formed XML.
    size_t chapters = 0;
    bool nav = false;
    for (const auto& item : opf.select_nodes("//*[local-name()='item']"))
    {
        const std::string href = item.node().attribute("href").value();
        const auto path = "OEBPS/" + href;
        ASSERT_TRUE(zip.Has(path)) << path;
        nav |= std::string(item.node().attribute("properties").value()) == "nav";
        if (href.ends_with(".xhtml"))
        {
            pugi::xml_document x;
            const auto xml = zip.Read(path).value_or("");
            EXPECT_TRUE(x.load_buffer(xml.data(), xml.size())) << path;
            chapters += href.starts_with("text/");
        }
    }
    EXPECT_TRUE(nav);
    EXPECT_EQ(chapters, 2u);   // split at the two level-1 headings
    EXPECT_TRUE(zip.Has("OEBPS/images/image1.png"));
    EXPECT_EQ(opf.select_nodes("//*[local-name()='itemref']").size(), 2u);
}

TEST(Docs, PlainTextBothWays)
{
    TempDir dir;
    const auto text = WritePlainText(Sample(PngBytes(dir, 4, 3)));
    EXPECT_NE(text.find("Title & more\r\n"), std::string::npos);
    EXPECT_NE(text.find("See a link (https://example.com/a?b=c&d=e) <now>.\r\n"), std::string::npos) << text;
    EXPECT_NE(text.find("\xE2\x80\xA2 one\r\n  \xE2\x80\xA2 nested\r\n"), std::string::npos) << text;
    EXPECT_NE(text.find("3. third\r\n4. fourth\r\n"), std::string::npos) << text;
    EXPECT_NE(text.find("Name\tQty\r\nApples\t3\r\n"), std::string::npos) << text;

    Document doc;
    ReadPlainText("First line\nstill first\n\n\nSecond", doc);
    EXPECT_EQ(Describe(doc), "P First line\\nstill first\nP Second\n");
}

TEST(Docs, RtfPrepareSwapsPicturesAndLinks)
{
    TempDir dir;
    const auto png = PngBytes(dir, 4, 3);
    std::string hex;
    for (const unsigned char c : png)
        hex += std::format("{:02x}", c);
    const std::string rtf = "{\\rtf1 Hello {\\field{\\*\\fldinst{HYPERLINK \"https://x.example/\"}}{\\fldrslt{there}}} "
                            "{\\pict\\pngblip\\picwgoal1500\\pichgoal750 " + hex + "} {\\*\\shppict{\\pict\\pngblip " + hex + "}}{\\nonshppict{\\pict\\wmetafile8 00}}}";
    auto prepared = RtfPrepare(rtf, {});
    ASSERT_EQ(prepared.links.size(), 1u);
    EXPECT_EQ(prepared.links[0], "https://x.example/");
    ASSERT_EQ(prepared.images.size(), 2u);   // the \nonshppict fallback is dropped
    EXPECT_EQ(prepared.images[0], png);
    EXPECT_EQ(prepared.imageWidths[0], 100);   // 1500 twips = 100 px at 96 dpi
    EXPECT_EQ(prepared.imageHeights[0], 50);
    EXPECT_NE(prepared.rtf.find("@@SDLNK0@@"), std::string::npos);
    EXPECT_NE(prepared.rtf.find("@@SDIMG0@@"), std::string::npos);
    EXPECT_EQ(prepared.rtf.find("pngblip"), std::string::npos);
}

TEST(Docs, RtfHeuristics)
{
    const auto run = [](const wchar_t* text, float size, bool bold = false, bool mono = false) {
        RtfRun r;
        r.text = text;
        r.sizePt = size;
        r.bold = bold;
        r.mono = mono;
        return r;
    };
    std::vector<RtfParagraph> ps(9);
    ps[0].runs = { run(L"Big title", 20, true) };
    ps[1].runs = { run(L"Some body text that is long enough to set the body size. ", 12), run(L"@@SDLNK0@@here@@SDEND@@", 12) };
    ps[2].runs = { run(L"Section", 16, true) };
    ps[3].runs = { run(L"item one", 12) };
    ps[3].numbering = 1;
    ps[3].indentTwips = 720;
    ps[4].runs = { run(L"deeper", 12) };
    ps[4].numbering = 1;
    ps[4].indentTwips = 1080;
    ps[5].runs = { run(L"first", 12) };
    ps[5].numbering = 2;
    ps[5].numberStart = 1;
    ps[5].indentTwips = 720;
    ps[6].runs = { run(L"second", 12) };
    ps[6].numbering = 2;
    ps[6].indentTwips = 720;
    ps[7].runs = { run(L"int x;", 10, false, true) };
    ps[8].runs = { run(L"return x;", 10, false, true) };
    RtfPrepared prepared;
    prepared.links = { "https://x.example/" };
    Document doc;
    RtfBuildDocument(ps, prepared, doc);
    EXPECT_EQ(Describe(doc),
              "H1 Big title\n"
              "P Some body text that is long enough to set the body size. [here](https://x.example/)\n"
              "H2 Section\n"
              "L0* item one\nL1* deeper\n"
              "L0#1 first\nL0#2 second\n"
              "C int x;\nreturn x;\n");
}

TEST(Docs, RtfThroughRichEdit)
{
    const std::string rtf =
        "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Calibri;}{\\f1 Consolas;}}\\fs24 "
        // As WordPad writes it: switches on and off rather than groups.
        "\\b\\fs40 Heading\\b0\\fs24\\par "
        "Plain \\b bold\\b0  \\i italic\\i0  \\strike gone\\strike0  caf\\'e9 text, long enough to be the body.\\par "
        "{\\f1 mono line\\par}"
        "}";
    Document doc;
    std::wstring error;
    ASSERT_TRUE(ReadRtf(rtf, {}, doc, error)) << Narrow(error);
    EXPECT_EQ(Describe(doc), "H1 Heading\nP Plain **bold** _italic_ ~~gone~~ café text, long enough to be the body.\nC mono line\n");
}

TEST(Docs, LineBreaking)
{
    FakeMeasure m;
    FontStyle f;
    f.size = 10;   // 5 points per character
    std::vector<Fragment> frags = { Fragment{ L"aaaa bbbb cccc", f } };
    auto lines = BreakLines(frags, 50, m);   // 10 characters per line
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].items.at(0).text, L"aaaa bbbb");
    EXPECT_FLOAT_EQ(lines[0].width, 45);
    EXPECT_EQ(lines[1].items.at(0).text, L"cccc");
    EXPECT_FLOAT_EQ(lines[1].ascent, 8);
    EXPECT_FLOAT_EQ(lines[1].descent, 2);

    // A word wider than the line is split; a hard break ends the line.
    Fragment br;
    br.lineBreak = true;
    lines = BreakLines({ Fragment{ L"abcdefghijklmnop", f }, br, Fragment{ L"x", f } }, 50, m);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0].items.at(0).text, L"abcdefghij");
    EXPECT_EQ(lines[1].items.at(0).text, L"klmnop");
    EXPECT_EQ(lines[2].items.at(0).text, L"x");

    // Styled runs sit side by side on one line.
    FontStyle b = f;
    b.bold = true;
    lines = BreakLines({ Fragment{ L"ab ", f }, Fragment{ L"cd", b } }, 100, m);
    ASSERT_EQ(lines.size(), 1u);
    ASSERT_EQ(lines[0].items.size(), 2u);
    EXPECT_FLOAT_EQ(lines[0].items[1].x, 15);
    EXPECT_TRUE(lines[0].items[1].font.bold);

    // Code keeps its indentation.
    lines = BreakLines({ Fragment{ L"    x", f } }, 100, m, true);
    EXPECT_EQ(lines.at(0).items.at(0).text, L"    x");
}

TEST(Docs, ColumnFitting)
{
    // Room for everything: natural widths.
    auto w = FitColumns({ 100, 50 }, { 20, 10 }, 400);
    EXPECT_FLOAT_EQ(w[0], 100);
    EXPECT_FLOAT_EQ(w[1], 50);
    // Too wide: every column keeps its minimum, the rest goes by natural width, total = available.
    w = FitColumns({ 600, 200 }, { 50, 50 }, 400);
    EXPECT_NEAR(w[0] + w[1], 400, 0.01);
    EXPECT_GE(w[1], 50);
    EXPECT_GT(w[0], w[1]);
    // Minimums that don't fit: equal shares at most.
    w = FitColumns({ 900, 900, 900 }, { 300, 300, 300 }, 300);
    EXPECT_NEAR(w[0] + w[1] + w[2], 300, 0.01);
    EXPECT_NEAR(w[0], 100, 0.01);
}

TEST(Docs, PaginationNumbersAndRepeatedHeaders)
{
    FakeMeasure m;
    Document doc;
    Block h;
    h.kind = BlockKind::Heading;
    h.level = 1;
    h.spans = { Span{ "Report" } };
    doc.blocks.push_back(h);
    for (int i = 0; i < 40; ++i)
    {
        Block p;
        p.spans = { Span{ std::format("Paragraph {} with enough words to wrap over more than one line of the page.", i) } };
        doc.blocks.push_back(p);
    }
    Block t;
    t.kind = BlockKind::Table;
    t.headerRow = true;
    t.rows.push_back({ TableCell{ { Span{ "Head A" } } }, TableCell{ { Span{ "Head B" } } } });
    for (int i = 0; i < 80; ++i)
        t.rows.push_back({ TableCell{ { Span{ std::format("a{}", i) } } }, TableCell{ { Span{ std::format("b{}", i) } } } });
    doc.blocks.push_back(t);

    LayoutOptions o;
    const auto pages = LayoutDocument(doc, o, m);
    ASSERT_GE(pages.size(), 3u);
    for (size_t i = 0; i < pages.size(); ++i)
    {
        // Every page has its number at the bottom, and nothing leaves the paper.
        bool number = false;
        for (const auto& item : pages[i].items)
        {
            if (item.kind == ItemKind::Text && item.text == std::to_wstring(i + 1) && item.y > o.page.height - o.page.margin)
                number = true;
            EXPECT_GE(item.x, 0);
            EXPECT_LE(item.x + item.w, o.page.width + 0.01f);
            EXPECT_LE(item.y, o.page.height);
        }
        EXPECT_TRUE(number) << "page " << i + 1;
    }
    // The table continues on a later page that starts with its header row again.
    size_t headers = 0;
    for (const auto& page : pages)
        for (const auto& item : page.items)
            headers += item.text == L"Head A";
    EXPECT_GE(headers, 2u);
    // The body text sits inside the margins.
    for (const auto& item : pages[0].items)
        if (item.kind == ItemKind::Text && item.text.starts_with(L"Paragraph"))
            EXPECT_GE(item.x, o.page.margin - 0.01f);
}

TEST(Docs, HeadingIsKeptWithTheNextParagraph)
{
    FakeMeasure m;
    LayoutOptions o;
    Document doc;
    // Fill the first page up to just above the bottom, then a heading and its paragraph.
    for (int i = 0; i < 200; ++i)
    {
        Block p;
        p.spans = { Span{ "x" } };
        doc.blocks.push_back(p);
        const auto pages = LayoutDocument(doc, o, m);
        if (pages.size() > 1)
        {
            doc.blocks.pop_back();
            doc.blocks.pop_back();
            break;
        }
    }
    Block h;
    h.kind = BlockKind::Heading;
    h.level = 2;
    h.spans = { Span{ "Heading" } };
    doc.blocks.push_back(h);
    Block p;
    p.spans = { Span{ "After" } };
    doc.blocks.push_back(p);
    const auto pages = LayoutDocument(doc, o, m);
    ASSERT_EQ(pages.size(), 2u);
    const auto onPage = [&](size_t page, const wchar_t* text) {
        for (const auto& item : pages[page].items)
            if (item.text == text)
                return true;
        return false;
    };
    EXPECT_TRUE(onPage(1, L"Heading"));
    EXPECT_TRUE(onPage(1, L"After"));
}

TEST(Docs, PaperFollowsTheRegion)
{
    const auto paper = DefaultPaper();
    EXPECT_TRUE((paper.letter && paper.widthPt == 612.f) || (!paper.letter && std::abs(paper.widthPt - 595.28f) < 0.01f));
    EXPECT_FLOAT_EQ(LetterPaper().heightPt, 792.f);
    EXPECT_NEAR(A4Paper().heightPt, 841.89f, 0.01f);
}

TEST(Docs, PrintsToPdfWhenThePrinterIsThere)
{
    std::wstring why;
    if (!FindPdfPrinter(nullptr, &why))
        GTEST_SKIP() << "no Microsoft Print to PDF here: " << Narrow(why);
    TempDir dir;
    const auto doc = Sample(PngBytes(dir, 4, 3));
    std::string pdf;
    std::wstring error;
    ASSERT_TRUE(DocumentToPdf(doc, L"Sample", false, 11, {}, {}, pdf, error)) << Narrow(error);
    EXPECT_TRUE(pdf.starts_with("%PDF"));
    EXPECT_NE(pdf.find("%%EOF"), std::string::npos);
}

TEST(Docs, EngineConversions)
{
    TempDir dir;
    Recipes recipes(dir);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);

    std::string docx;
    ASSERT_TRUE(WriteDocx(Sample(PngBytes(dir, 4, 3)), docx));
    const auto source = dir / L"report.docx";
    WriteText(source, docx);
    EXPECT_FALSE(runner.Applies(recipes.Get("docs-to-docx"), source));
    for (const char* id : { "docs-to-md", "docs-to-html", "docs-to-epub", "docs-to-txt", "docs-to-pdf" })
        EXPECT_TRUE(runner.Applies(recipes.Get(id), source)) << id;

    // With a picture, Markdown becomes a folder holding the page and its images.
    const auto md = runner.Run(recipes.Get("docs-to-md"), 0, source, {});
    ASSERT_TRUE(md.ok) << Narrow(md.error);
    ASSERT_TRUE(std::filesystem::is_directory(md.output));
    const auto page = ReadAll((std::filesystem::path(md.output) / L"report.md").wstring());
    EXPECT_TRUE(page.starts_with("# Title & more\n")) << page;
    EXPECT_NE(page.find("![A picture](images/image1.png)"), std::string::npos) << page;
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(md.output) / L"images" / L"image1.png"));

    const auto epub = runner.Run(recipes.Get("docs-to-epub"), 0, source, {});
    ASSERT_TRUE(epub.ok) << Narrow(epub.error);
    EXPECT_EQ(std::filesystem::path(epub.output).extension(), L".epub");

    // Markdown without pictures to DOCX is a single file, and back.
    const auto notes = dir / L"notes.md";
    WriteText(notes, "# Notes\n\n- a\n- b\n");
    EXPECT_FALSE(runner.Applies(recipes.Get("docs-to-md"), notes));
    const auto toDocx = runner.Run(recipes.Get("docs-to-docx"), 0, notes, {});
    ASSERT_TRUE(toDocx.ok) << Narrow(toDocx.error);
    Document back;
    std::string error;
    ASSERT_TRUE(ReadDocx(ReadAll(toDocx.output), back, error)) << error;
    EXPECT_EQ(Describe(back), "H1 Notes\nL0* a\nL0* b\n");
    const auto html = runner.Run(recipes.Get("docs-to-html"), 0, notes, {});
    ASSERT_TRUE(html.ok) << Narrow(html.error);
    EXPECT_FALSE(std::filesystem::is_directory(html.output));

    // TXT gets no Markdown slice (it would change nothing); PDF only DOCX, EPUB and HTML.
    EXPECT_FALSE(runner.Applies(recipes.Get("docs-to-md"), dir / L"a.txt"));
    EXPECT_TRUE(runner.Applies(recipes.Get("docs-to-docx"), dir / L"a.txt"));
    EXPECT_TRUE(runner.Applies(recipes.Get("docs-to-docx"), dir / L"a.pdf"));
    EXPECT_FALSE(runner.Applies(recipes.Get("docs-to-md"), dir / L"a.pdf"));
    EXPECT_FALSE(runner.Applies(recipes.Get("docs-to-pdf"), dir / L"a.pdf"));
}

TEST(Docs, PdfInputReflowsToDocx)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto path = dir / L"paper.pdf";
    WriteTextPdf(path, { "BT /F1 24 Tf 72 700 Td (Findings) Tj ET\n"
                         "BT /F1 12 Tf 72 660 Td (The first para-) Tj 0 -14 Td (graph.) Tj ET\n"
                         "BT /F1 12 Tf 72 610 Td (\\225 A point) Tj ET" });
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    const auto result = runner.Run(recipes.Get("docs-to-docx"), 0, path, {});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    Document doc;
    std::string error;
    ASSERT_TRUE(ReadDocx(ReadAll(result.output), doc, error)) << error;
    EXPECT_EQ(Describe(doc), "H1 Findings\nP The first paragraph.\nL0* A point\n");
}

TEST(Docs, PdfPicturesComeAlong)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto path = dir / L"illustrated.pdf";
    WritePicturePdf(path,
                    { "BT /F1 24 Tf 72 700 Td (Findings) Tj ET\n"
                      "BT /F1 12 Tf 72 660 Td (Before the chart.) Tj ET\n"
                      "q 300 0 0 225 72 400 cm /Im0 Do Q\n"
                      "BT /F1 12 Tf 72 360 Td (After the chart.) Tj ET" },
                    { { "/Type /XObject /Subtype /Image /Width 40 /Height 30 /ColorSpace /DeviceRGB /BitsPerComponent 8", RgbRamp(40, 30), true } });
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);

    // DOCX: an inline picture in word/media where it was on the page, drawn 300 pt wide there, so
    // 400 pixels at 96 dpi, in the picture's own shape.
    const auto docx = runner.Run(recipes.Get("docs-to-docx"), 0, path, {});
    ASSERT_TRUE(docx.ok) << Narrow(docx.error);
    const auto bytes = ReadAll(docx.output);
    ZipReader zip;
    ASSERT_TRUE(zip.Open(bytes));
    EXPECT_TRUE(zip.Has("word/media/image1.png"));
    Document doc;
    std::string error;
    ASSERT_TRUE(ReadDocx(bytes, doc, error)) << error;
    EXPECT_EQ(Describe(doc), "H1 Findings\nP Before the chart.\nP ![](400x300)\nP After the chart.\n");

    // EPUB: the picture is in the package and its manifest.
    const auto epub = runner.Run(recipes.Get("docs-to-epub"), 0, path, {});
    ASSERT_TRUE(epub.ok) << Narrow(epub.error);
    ZipReader book;
    ASSERT_TRUE(book.Open(ReadAll(epub.output)));
    EXPECT_TRUE(book.Has("OEBPS/images/image1.png"));
    const auto opf = book.Read("OEBPS/content.opf").value_or("");
    EXPECT_NE(opf.find("href=\"images/image1.png\" media-type=\"image/png\""), std::string::npos) << opf;

    // HTML: like any HTML output with pictures, a folder holding the page and an images folder.
    const auto html = runner.Run(recipes.Get("docs-to-html"), 0, path, {});
    ASSERT_TRUE(html.ok) << Narrow(html.error);
    ASSERT_TRUE(std::filesystem::is_directory(html.output));
    const auto page = ReadAll((std::filesystem::path(html.output) / L"illustrated.html").wstring());
    EXPECT_NE(page.find("<img src=\"images/image1.png\""), std::string::npos) << page;
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(html.output) / L"images" / L"image1.png"));
}
