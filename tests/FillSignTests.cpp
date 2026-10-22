// Fill and sign: typing in a text box, its handles, the signatures kept on this PC, and marks
// written into a PDF page as real content (text in a subset font, images, shapes).
#include "FileIo.h"
#include "PdfTestUtil.h"
#include "TestUtil.h"
#include "Utf.h"
#include "canvas/Raster.h"
#include "pdf/FontSubset.h"
#include "pdf/PageEdits.h"
#include "pdf/PageMarks.h"
#include "pdf/PdfDoc.h"
#include "pdf/PdfText.h"
#include "pdf/PdfWriter.h"
#include "photo/Annotations.h"
#include "photo/Compose.h"
#include "photo/PhotoTools.h"
#include "photo/Signatures.h"
#include "photo/TextBox.h"

#include <qpdf/QPDFPageDocumentHelper.hh>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <map>

using namespace sd;
using namespace sd::test;
using canvas::PointD;
using canvas::Raster;
using canvas::RectD;

namespace
{
    // ---- A small TrueType font made here, so the subsetting doesn't depend on installed fonts ------

    void Put16(std::string& s, uint16_t v)
    {
        s.push_back(char(v >> 8));
        s.push_back(char(v & 0xFF));
    }
    void Put32(std::string& s, uint32_t v)
    {
        Put16(s, uint16_t(v >> 16));
        Put16(s, uint16_t(v & 0xFFFF));
    }
    uint16_t Get16(std::string_view s, size_t at) { return uint16_t((uint8_t(s[at]) << 8) | uint8_t(s[at + 1])); }
    uint32_t Get32(std::string_view s, size_t at) { return (uint32_t(Get16(s, at)) << 16) | Get16(s, at + 2); }

    // A simple glyph: one closed contour through `points` (on-curve), coordinates as words.
    std::string SimpleGlyph(const std::vector<std::pair<int16_t, int16_t>>& points)
    {
        std::string g;
        int16_t x0 = 32767, y0 = 32767, x1 = -32768, y1 = -32768;
        for (const auto& [x, y] : points)
        {
            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        }
        Put16(g, 1);
        for (const int16_t v : { x0, y0, x1, y1 })
            Put16(g, uint16_t(v));
        Put16(g, uint16_t(points.size() - 1));   // end point of the contour
        Put16(g, 0);                              // no instructions
        for (size_t i = 0; i < points.size(); ++i)
            g.push_back(0x01);                    // on curve, long x and y
        int16_t px = 0, py = 0;
        for (const auto& p : points)
        {
            Put16(g, uint16_t(int16_t(p.first - px)));
            px = p.first;
        }
        for (const auto& p : points)
        {
            Put16(g, uint16_t(int16_t(p.second - py)));
            py = p.second;
        }
        while (g.size() % 2)
            g.push_back('\0');
        return g;
    }

    struct FontOptions
    {
        uint16_t fsType = 0;
        bool cff = false;
        bool variable = false;   // an 'fvar' with a weight axis whose default is 400
    };

    // Glyphs: 0 .notdef (empty), 1 a triangle, 2 a square, 3 a composite of 1 and 2, 4 a spare.
    std::string TestFont(const FontOptions& o = {})
    {
        std::vector<std::string> glyphs = { "", SimpleGlyph({ { 0, 0 }, { 300, 700 }, { 600, 0 } }), SimpleGlyph({ { 50, 0 }, { 50, 500 }, { 550, 500 }, { 550, 0 } }),
                                            "", SimpleGlyph({ { 0, 0 }, { 100, 100 }, { 200, 0 } }) };
        std::string composite;
        Put16(composite, uint16_t(int16_t(-1)));
        for (const int16_t v : { 0, 0, 1200, 700 })
            Put16(composite, uint16_t(v));
        Put16(composite, 0x0021);   // ARG_1_AND_2_ARE_WORDS | MORE_COMPONENTS
        Put16(composite, 1);
        Put16(composite, 0);
        Put16(composite, 0);
        Put16(composite, 0x0001);
        Put16(composite, 2);
        Put16(composite, 600);
        Put16(composite, 0);
        glyphs[3] = composite;
        std::string glyf, loca;
        for (const auto& g : glyphs)
        {
            Put16(loca, uint16_t(glyf.size() / 2));
            glyf += g;
        }
        Put16(loca, uint16_t(glyf.size() / 2));
        const uint16_t n = uint16_t(glyphs.size());

        std::string head;
        Put32(head, 0x00010000);
        Put32(head, 0x00010000);
        Put32(head, 0);            // checkSumAdjustment
        Put32(head, 0x5F0F3CF5);
        Put16(head, 0);
        Put16(head, 1000);         // unitsPerEm
        head.append(16, '\0');     // created, modified
        for (const int16_t v : { 0, -200, 1200, 800 })
            Put16(head, uint16_t(v));
        Put16(head, 0);
        Put16(head, 8);
        Put16(head, 2);
        Put16(head, 0);            // short loca
        Put16(head, 0);
        std::string hhea;
        Put32(hhea, 0x00010000);
        Put16(hhea, 800);
        Put16(hhea, uint16_t(int16_t(-200)));
        hhea.append(26, '\0');
        Put16(hhea, n);            // numberOfHMetrics
        std::string maxp;
        Put32(maxp, 0x00005000);
        Put16(maxp, n);
        std::string hmtx;
        for (uint16_t i = 0; i < n; ++i)
        {
            Put16(hmtx, uint16_t(500 + 100 * i));
            Put16(hmtx, 0);
        }
        std::string os2(96, '\0');
        os2[0] = 0, os2[1] = 4;
        os2[8] = char(o.fsType >> 8), os2[9] = char(o.fsType & 0xFF);
        os2[88] = char(700 >> 8), os2[89] = char(700 & 0xFF);
        std::string post;
        Put32(post, 0x00030000);
        post.append(28, '\0');
        std::string name;
        const std::string ps = "TestSans-Regular";
        Put16(name, 0);
        Put16(name, 1);
        Put16(name, 6 + 12);
        for (const size_t v : { size_t(3), size_t(1), size_t(0x409), size_t(6), ps.size() * 2, size_t(0) })
            Put16(name, uint16_t(v));
        for (const char c : ps)
            Put16(name, uint16_t(c));
        std::map<std::string, std::string> tables = { { "OS/2", os2 }, { "glyf", glyf }, { "head", head }, { "hhea", hhea },
                                                      { "hmtx", hmtx }, { "loca", loca }, { "maxp", maxp }, { "name", name }, { "post", post } };
        if (o.variable)
        {
            std::string fvar;
            for (const uint16_t v : { 1, 0, 16, 2, 1, 20, 0, 0 })
                Put16(fvar, v);
            Put32(fvar, 0x77676874);   // 'wght'
            Put32(fvar, 100u << 16);
            Put32(fvar, 400u << 16);
            Put32(fvar, 900u << 16);
            Put16(fvar, 0);
            Put16(fvar, 256);
            tables["fvar"] = fvar;
        }
        std::string out;
        Put32(out, o.cff ? 0x4F54544F : 0x00010000);
        Put16(out, uint16_t(tables.size()));
        out.append(6, '\0');
        std::string body;
        const size_t start = 12 + tables.size() * 16;
        for (const auto& [tag, data] : tables)
        {
            out += tag;
            Put32(out, 0);
            Put32(out, uint32_t(start + body.size()));
            Put32(out, uint32_t(data.size()));
            body += data;
            while (body.size() % 4)
                body.push_back('\0');
        }
        return out + body;
    }

    // The tables of a TrueType file, as SubsetTrueType wrote it.
    std::map<std::string, std::string_view> Tables(std::string_view font)
    {
        std::map<std::string, std::string_view> out;
        const uint16_t n = Get16(font, 4);
        for (uint16_t i = 0; i < n; ++i)
        {
            const size_t rec = 12 + size_t(i) * 16;
            out[std::string(font.substr(rec, 4))] = font.substr(Get32(font, rec + 8), Get32(font, rec + 12));
        }
        return out;
    }

    // A text box laid out by hand: "Hello world" wrapped after "Hello " (10 pixels a character), then
    // a line break and "ok".
    photo::TextLayoutInfo HandLayout()
    {
        photo::TextLayoutInfo l;
        l.lines = { { 0, 6, 0, 20, 0, 60 }, { 6, 11, 20, 20, 0, 50 }, { 12, 14, 40, 20, 0, 20 } };
        // The wrap point (6) counts as the second line's start; position 12 starts "ok".
        l.caretX = { 0, 10, 20, 30, 40, 50, 0, 10, 20, 30, 40, 50, 0, 10, 20 };
        l.width = 60;
        l.height = 60;
        return l;
    }

    // A TrueType font this PC has: Arial on Windows, a look-alike elsewhere (Wine).
    std::wstring InstalledFont()
    {
        const auto& families = photo::SystemFontFamilies();
        for (const wchar_t* name : { L"Arial", L"Segoe UI", L"Liberation Sans", L"DejaVu Sans" })
            if (std::find(families.begin(), families.end(), name) != families.end())
                return name;
        return families.empty() ? L"Arial" : families.front();
    }

    Raster Filled(UINT w, UINT h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
    {
        Raster out;
        out.Allocate(w, h);
        for (size_t i = 0; i < out.px.size(); i += 4)
        {
            out.px[i] = b;
            out.px[i + 1] = g;
            out.px[i + 2] = r;
            out.px[i + 3] = a;
        }
        return out;
    }

    // A one-page PDF of `w` x `h` points with "Original" written on it, turned by `rotate`.
    std::wstring SourcePdf(const TempDir& dir, const std::wstring& name, double w, double h, int rotate = 0)
    {
        const auto path = dir / name;
        pdf::PdfWriter out(path);
        const int catalog = out.NewObject(), pages = out.NewObject(), page = out.NewObject(), content = out.NewObject(), font = out.NewObject();
        out.Object(catalog, std::format("<< /Type /Catalog /Pages {} 0 R >>", pages));
        out.Object(pages, std::format("<< /Type /Pages /Kids [{} 0 R] /Count 1 >>", page));
        out.Object(page, std::format("<< /Type /Page /Parent {} 0 R /MediaBox [0 0 {} {}] /Rotate {} /Resources << /Font << /F1 {} 0 R >> >> /Contents {} 0 R >>",
                                     pages, pdf::Num(w), pdf::Num(h), rotate, font, content));
        out.Object(font, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
        out.Stream(content, "", "BT /F1 12 Tf 10 20 Td (Original) Tj ET\n");
        out.Finish(catalog, 0);
        return path;
    }

    std::string Contents(QPDFPageObjectHelper& page)
    {
        std::string all;
        for (auto& s : page.getPageContents())
        {
            auto data = s.getStreamData(qpdf_dl_generalized);
            all.append(reinterpret_cast<const char*>(data->getBuffer()), data->getSize());
        }
        return all;
    }

    std::string StreamText(QPDFObjectHandle stream)
    {
        auto data = stream.getStreamData(qpdf_dl_generalized);
        return std::string(reinterpret_cast<const char*>(data->getBuffer()), data->getSize());
    }

    std::wstring AllText(const pdf::PdfText& text)
    {
        std::wstring all;
        for (const auto& page : text.pages)
            for (const auto& line : page)
                all += line.text + L"\n";
        return all;
    }
}

// ---- Typing -----------------------------------------------------------------------------------------

TEST(FillSign, CaretMovesThroughWrappedAndBrokenLines)
{
    using namespace sd::photo;
    const std::wstring text = L"Hello world\nok";
    const auto l = HandLayout();
    EXPECT_EQ(l.LineOf(6), 1u);    // where the line wrapped: the next line's start
    EXPECT_EQ(l.LineOf(11), 1u);   // before the line break
    EXPECT_EQ(l.LineOf(12), 2u);
    EXPECT_EQ(PositionAt(l, { 22, 5 }), 2u);
    EXPECT_EQ(PositionAt(l, { 999, 25 }), 11u);
    EXPECT_EQ(PositionAt(l, { 999, 5 }), 5u);   // the end of a wrapped line is before its space
    EXPECT_EQ(PositionAt(l, { 3, 200 }), 12u);  // below the text: the last line

    TextCursor c;
    double goal = -1;
    c.Collapse(3);
    MoveCaret(c, text, l, CaretMove::Down, false, goal);
    EXPECT_EQ(c.caret, 9u);
    MoveCaret(c, text, l, CaretMove::Down, false, goal);
    EXPECT_EQ(c.caret, 14u);   // "ok" is shorter: its end
    MoveCaret(c, text, l, CaretMove::Up, false, goal);
    EXPECT_EQ(c.caret, 9u);    // back to the column it started in
    MoveCaret(c, text, l, CaretMove::Home, false, goal);
    EXPECT_EQ(c.caret, 6u);
    MoveCaret(c, text, l, CaretMove::End, true, goal);
    EXPECT_EQ(c.caret, 11u);
    EXPECT_EQ(SelectedText(text, c), L"world");
    MoveCaret(c, text, l, CaretMove::Left, false, goal);
    EXPECT_EQ(c.caret, 6u);    // a selection collapses to its side
    MoveCaret(c, text, l, CaretMove::WordLeft, false, goal);
    EXPECT_EQ(c.caret, 0u);
    MoveCaret(c, text, l, CaretMove::WordRight, false, goal);
    EXPECT_EQ(c.caret, 6u);
    MoveCaret(c, text, l, CaretMove::TextEnd, true, goal);
    EXPECT_EQ(c.Start(), 6u);
    EXPECT_EQ(c.End(), 14u);
    // The highlight: the rest of line 2 with its break, then "ok".
    const auto rects = SelectionRects(l, c.Start(), c.End(), 4);
    ASSERT_EQ(rects.size(), 2u);
    EXPECT_DOUBLE_EQ(rects[0].x, 0);
    EXPECT_DOUBLE_EQ(rects[0].w, 54);
    EXPECT_DOUBLE_EQ(rects[1].y, 40);
}

TEST(FillSign, EditsReplaceSelectionsAndKeepSurrogatePairs)
{
    using namespace sd::photo;
    std::wstring text = L"Sign here";
    TextCursor c;
    SelectWord(c, text, 6);
    EXPECT_EQ(SelectedText(text, c), L"here");
    EXPECT_TRUE(InsertText(text, c, L"below", 100));
    EXPECT_EQ(text, L"Sign below");
    EXPECT_EQ(c.caret, 10u);
    EXPECT_TRUE(DeleteBackward(text, c, true));   // Ctrl+Backspace: the word
    EXPECT_EQ(text, L"Sign ");
    c.Collapse(0);
    EXPECT_TRUE(DeleteForward(text, c, true));    // Ctrl+Delete: the word and its space
    EXPECT_EQ(text, L"");
    EXPECT_FALSE(DeleteBackward(text, c, false));

    // An emoji is two UTF-16 units: one Backspace takes both, and a limit never splits it.
    text = L"ok\U0001F58A";
    c.Collapse(text.size());
    EXPECT_TRUE(DeleteBackward(text, c, false));
    EXPECT_EQ(text, L"ok");
    EXPECT_TRUE(InsertText(text, c, L"!\U0001F58A", 4));
    EXPECT_EQ(text, L"ok!");
    EXPECT_FALSE(InsertText(text, c, L"x", 3));
    EXPECT_EQ(CleanPasted(L"a\r\nb\rc\td\x01"), L"a\nb\nc d");
}

TEST(FillSign, DirectWriteWrapsANarrowBoxIntoMoreLines)
{
    using namespace sd::photo;
    Annotations set;
    Annotation t;
    t.kind = Mark::Text;
    t.text = L"Name of the applicant as written in the passport";
    t.fontSize = 20;
    t.font = InstalledFont();
    t.a = { 10, 10 };
    t.wrap = true;
    t.b = { 610, 10 };
    const uint32_t id = set.Add(t);
    TextLayoutInfo wide, narrow;
    PointD origin;
    canvas::SizeD measured;
    ASSERT_TRUE(LayOutMarkText(*set.Find(id), set, wide, origin, &measured));
    set.Find(id)->b.x = 130;
    ASSERT_TRUE(LayOutMarkText(*set.Find(id), set, narrow, origin, &measured));
    EXPECT_GE(wide.lines.size(), 1u);
    EXPECT_GT(narrow.lines.size(), wide.lines.size());
    EXPECT_DOUBLE_EQ(measured.w, 120);   // the box's width, not the text's
    EXPECT_GT(narrow.height, wide.height);
    EXPECT_EQ(narrow.caretX.size(), t.text.size() + 1);
    // Every character is on exactly one line, in order.
    for (size_t i = 1; i < narrow.lines.size(); ++i)
        EXPECT_GE(narrow.lines[i].start, narrow.lines[i - 1].end);
    // Without Auto-wrap it is one line however narrow the box was.
    set.Find(id)->wrap = false;
    ASSERT_TRUE(LayOutMarkText(*set.Find(id), set, narrow, origin, &measured));
    EXPECT_EQ(narrow.lines.size(), 1u);
}

// ---- Handles ----------------------------------------------------------------------------------------

TEST(FillSign, TextBoxHandlesWrapResizeAndScaleTheType)
{
    using namespace sd::photo;
    Annotations set;
    Annotation t;
    t.kind = Mark::Text;
    t.text = L"Jane Doe";
    t.fontSize = 20;
    t.a = { 100, 100 };
    const uint32_t id = set.Add(t);
    set.SetMeasured(id, { 80, 26 });
    auto* m = set.Find(id);
    const RectD box = set.TextBox(*m);
    EXPECT_DOUBLE_EQ(box.w, 80);
    // Corners and the two sides; no top or bottom until it wraps.
    EXPECT_EQ(set.Grips(*m).size(), 6u);
    EXPECT_EQ(set.GripAt(*m, { 180, 113 }, 3), Part::Right);
    EXPECT_EQ(set.GripAt(*m, { 180, 126 }, 3), Part::B);
    EXPECT_EQ(set.GripAt(*m, { 140, 113 }, 3), Part::Body);

    // A corner scales the type, the opposite corner staying put.
    Annotation before = *m;
    set.Drag(*m, before, Part::B, { 180, 126 }, { 260, 152 });
    EXPECT_NEAR(m->fontSize, 40, 0.5);
    EXPECT_DOUBLE_EQ(m->a.x, 100);
    EXPECT_DOUBLE_EQ(m->a.y, 100);
    EXPECT_FALSE(m->wrap);
    // The left side sets a width to wrap at (Auto-wrap on), the right edge staying put.
    *m = before;
    set.Drag(*m, before, Part::Left, { 100, 113 }, { 130, 113 });
    EXPECT_TRUE(m->wrap);
    EXPECT_DOUBLE_EQ(m->a.x, 130);
    EXPECT_DOUBLE_EQ(m->b.x, 180);
    EXPECT_EQ(set.Grips(*m).size(), 8u);
    // A wrapping box's corner resizes the box; the type stays.
    before = *m;
    set.SetMeasured(id, { 50, 52 });
    before = *m;
    const RectD wrapped = set.TextBox(*m);
    set.Drag(*m, before, Part::B, { wrapped.Right(), wrapped.Bottom() }, { wrapped.Right() + 100, wrapped.Bottom() + 30 });
    EXPECT_DOUBLE_EQ(m->fontSize, 20);
    EXPECT_DOUBLE_EQ(m->b.x - m->a.x, 150);
    EXPECT_DOUBLE_EQ(m->b.y - m->a.y, wrapped.h + 30);
    // Never narrower than a character or so.
    before = *m;
    set.Drag(*m, before, Part::Right, { m->b.x, 120 }, { m->a.x - 500, 120 });
    EXPECT_GE(m->b.x - m->a.x, MinTextWidth(*m) - 1e-9);
}

TEST(FillSign, PicturesKeepTheirShapeUnlessShiftIsHeld)
{
    using namespace sd::photo;
    Annotations set;
    Annotation s;
    s.kind = Mark::Image;
    s.signature = true;
    s.image = L"C:\\sig.png";
    s.a = { 100, 100 };
    s.b = { 400, 200 };   // 3:1
    const uint32_t id = set.Add(s);
    auto* m = set.Find(id);
    EXPECT_EQ(set.Grips(*m).size(), 8u);
    const Annotation before = *m;
    set.Drag(*m, before, Part::B, { 400, 200 }, { 700, 260 });
    EXPECT_NEAR((m->b.x - m->a.x) / (m->b.y - m->a.y), 3.0, 1e-6);
    EXPECT_DOUBLE_EQ(m->a.x, 100);
    EXPECT_DOUBLE_EQ(m->a.y, 100);
    *m = before;
    set.Drag(*m, before, Part::B, { 400, 200 }, { 500, 300 }, true);   // Shift: free
    EXPECT_DOUBLE_EQ(m->b.x, 500);
    EXPECT_DOUBLE_EQ(m->b.y, 300);
    *m = before;
    set.Drag(*m, before, Part::Right, { 400, 150 }, { 250, 150 }, true);
    EXPECT_DOUBLE_EQ(m->b.x, 250);
    EXPECT_DOUBLE_EQ(m->b.y, 200);

    // Ticks, crosses, the type and the signature flag survive the trip through JSON, ids included.
    Annotation tick;
    tick.kind = Mark::Check;
    tick.a = { 10, 10 };
    tick.b = { 30, 30 };
    const uint32_t tickId = set.Add(tick);
    Annotation text;
    text.kind = Mark::Text;
    text.text = L"21/10/2026";
    text.font = L"Georgia";
    text.weight = 700;
    text.italic = text.underline = text.wrap = true;
    text.align = Align::Right;
    text.a = { 5, 5 };
    text.b = { 200, 40 };
    const uint32_t textId = set.Add(text);
    set.Remove(id);
    const auto back = Annotations::FromJson(set.ToJson());
    ASSERT_EQ(back.Items().size(), 2u);
    ASSERT_NE(back.Find(tickId), nullptr);
    EXPECT_EQ(back.Find(tickId)->kind, Mark::Check);
    const auto* t = back.Find(textId);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->font, L"Georgia");
    EXPECT_EQ(t->weight, 700);
    EXPECT_TRUE(t->italic && t->underline && t->wrap);
    EXPECT_EQ(t->align, Align::Right);
    EXPECT_DOUBLE_EQ(t->b.x, 200);
    // A new mark after the round trip doesn't take an id that is in use.
    auto copy = back;
    Annotation cross;
    cross.kind = Mark::Cross;
    const uint32_t crossId = copy.Add(cross);
    EXPECT_NE(crossId, tickId);
    EXPECT_NE(crossId, textId);
    double width = 0;
    EXPECT_EQ(CheckStrokes(*copy.Find(crossId), width).size(), 2u);
    EXPECT_EQ(CheckStrokes(*copy.Find(tickId), width).size(), 1u);
}

// ---- The tool -----------------------------------------------------------------------------------------

TEST(FillSign, ToolTypesStylesAndPlacesSignatures)
{
    using namespace sd::photo;
    TempDir dir;
    AnnotateTool tool({ L"C:\\page.png" }, AnnotateOptions{ true, true });
    tool.UseSignatures(SignatureStore((dir.path() / L"signatures").wstring()));
    auto page = std::make_shared<Raster>(Filled(1700, 2200, 255, 255, 255));
    page->dpiX = page->dpiY = 200;
    tool.SetPicture(0, page);
    EXPECT_EQ(tool.Title(), L"Fill and sign");
    EXPECT_FALSE(tool.CanApply(nullptr));
    canvas::Pointer p;
    p.tolerance = 4;
    // It opens on the text box: a click and typing.
    p.at = { 300, 400 };
    tool.PointerDown(p);
    tool.PointerUp(p);
    ASSERT_TRUE(tool.TextInput());
    for (wchar_t c : std::wstring(L"Jane Dow"))
        tool.Char(c);
    const uint32_t id = tool.Editing();
    auto* m = tool.Marks().Find(id);
    ASSERT_NE(m, nullptr);
    EXPECT_NEAR(m->fontSize, 32 * 200 / 72.0, 1e-6);   // 32 pt on a 200 dpi page
    EXPECT_EQ(kDefaultTextSize, 32);
    EXPECT_EQ(m->color, 0x111111u);
    tool.Key(VK_BACK, false, false);
    tool.Char(L'e');
    EXPECT_EQ(m->text, L"Jane Doe");
    // Ctrl+A, Ctrl+C, then paste at the end: the text twice (the tool's own clipboard without a window).
    tool.Key('A', false, true);
    tool.Key('C', false, true);
    tool.Key(VK_END, false, true);
    tool.Char(L' ');
    tool.Key('V', false, true);
    EXPECT_EQ(m->text, L"Jane Doe Jane Doe");
    // Shift+Home selects to the line's start; typing replaces it.
    tool.Key(VK_HOME, true, false);
    for (wchar_t c : std::wstring(L"J. Doe"))
        tool.Char(c);
    EXPECT_EQ(m->text, L"J. Doe");
    // Bold, a bigger size, and the font size buttons step through the usual sizes.
    EXPECT_TRUE(tool.Key('B', false, true).changed);
    EXPECT_GE(m->weight, 700);
    tool.Key(VK_OEM_6, false, true);   // Ctrl+]
    EXPECT_NEAR(m->fontSize / (200 / 72.0), 36, 1e-6);
    tool.Key(VK_OEM_4, false, true);
    tool.Key(VK_OEM_4, false, true);   // Ctrl+[ twice
    EXPECT_NEAR(m->fontSize / (200 / 72.0), 28, 1e-6);
    canvas::ControlEvent wrap{ "wrap" };
    wrap.on = true;
    EXPECT_TRUE(tool.OnControl(wrap).changed);
    EXPECT_TRUE(m->wrap);
    EXPECT_GT(m->b.x, m->a.x);
    // Esc ends the typing; the box stays selected and Delete removes it.
    tool.Key(VK_ESCAPE, false, false);
    EXPECT_FALSE(tool.TextInput());
    EXPECT_EQ(tool.Selected(), id);
    EXPECT_TRUE(tool.Key(VK_DELETE, false, false).changed);
    EXPECT_TRUE(tool.Marks().Items().empty());

    // The date and a tick, each by a click.
    tool.SetMode(AnnotateTool::Mode::Date);
    p.at = { 900, 400 };
    tool.PointerDown(p);
    tool.PointerUp(p);
    ASSERT_EQ(tool.Marks().Items().size(), 1u);
    EXPECT_EQ(tool.Marks().Items()[0].text, AnnotateTool::TodayText());
    EXPECT_FALSE(AnnotateTool::TodayText().empty());
    tool.SetMode(AnnotateTool::Mode::Check);
    p.at = { 200, 800 };
    tool.PointerDown(p);
    EXPECT_TRUE(tool.PointerUp(p).changed);
    ASSERT_EQ(tool.Marks().Items().size(), 2u);
    const auto& tick = tool.Marks().Items()[1];
    EXPECT_EQ(tick.kind, Mark::Check);
    EXPECT_NEAR(tick.b.x - tick.a.x, tick.b.y - tick.a.y, 1e-9);

    // A signature waits for a click, then lands there at about 2.4 inches wide, its shape kept.
    auto ink = std::make_shared<Raster>(Filled(300, 100, 20, 30, 140));
    tool.BeginPlacing(L"C:\\signature.png", Picture::Of(ink), true);
    EXPECT_TRUE(tool.Escape());   // Esc drops it
    EXPECT_FALSE(tool.Escape());  // nothing left to drop: the window would close
    tool.BeginPlacing(L"C:\\signature.png", Picture::Of(ink), true);
    p.at = { 800, 1600 };
    EXPECT_TRUE(tool.PointerDown(p).changed);
    tool.PointerUp(p);
    ASSERT_EQ(tool.Marks().Items().size(), 3u);
    const auto& sig = tool.Marks().Items()[2];
    EXPECT_EQ(sig.kind, Mark::Image);
    EXPECT_TRUE(sig.signature);
    EXPECT_NEAR(sig.b.x - sig.a.x, 2.4 * 200, 1e-6);
    EXPECT_NEAR((sig.b.x - sig.a.x) / (sig.b.y - sig.a.y), 3.0, 1e-6);
    EXPECT_NEAR((sig.a.x + sig.b.x) / 2, 800, 1e-6);
    EXPECT_TRUE(tool.CanApply(nullptr));

    // Undo states keep the marks' ids, so the selection carries on.
    const auto state = tool.State();
    tool.Key(VK_DELETE, false, false);
    tool.Restore(state);
    EXPECT_EQ(tool.Marks().Items().size(), 3u);
    std::vector<canvas::Control> panel;
    tool.BuildPanel(panel);
    EXPECT_TRUE(std::any_of(panel.begin(), panel.end(), [](const canvas::Control& c) { return c.id == "drawSignature"; }));
}

// ---- Signatures kept on this PC ----------------------------------------------------------------------

TEST(FillSign, SignaturesAreKeptListedAndRemoved)
{
    using namespace sd::photo;
    TempDir dir;
    const SignatureStore store((dir.path() / L"signatures").wstring());
    EXPECT_TRUE(store.List().empty());
    std::wstring error;
    const auto first = store.Add(Filled(40, 20, 0, 0, 0), error);
    ASSERT_FALSE(first.empty()) << Narrow(error);
    const auto second = store.Add(Filled(30, 10, 0, 0, 200), error);
    ASSERT_FALSE(second.empty()) << Narrow(error);
    const auto draft = store.Add(Filled(30, 10, 0, 0, 0), error, true);
    ASSERT_FALSE(draft.empty());
    auto list = store.List();
    ASSERT_EQ(list.size(), 2u);   // a draft isn't offered again
    EXPECT_EQ(list[0], second);   // newest first
    EXPECT_EQ(list[1], first);
    Raster back;
    ASSERT_HRESULT_SUCCEEDED(canvas::LoadRaster(second, back, error));
    EXPECT_EQ(back.width, 30u);
    EXPECT_EQ(back.At(0, 0)[0], 200);

    // Only its own files: not a draft, not a file elsewhere.
    const auto elsewhere = dir / L"signature-elsewhere.png";
    std::filesystem::copy_file(first, elsewhere);
    EXPECT_FALSE(store.Remove(elsewhere));
    EXPECT_TRUE(std::filesystem::exists(elsewhere));
    EXPECT_FALSE(store.Remove(draft));
    EXPECT_TRUE(store.Remove(first));
    EXPECT_FALSE(std::filesystem::exists(first));
    EXPECT_EQ(store.List().size(), 1u);
    EXPECT_TRUE(store.Add(Raster{}, error).empty());
    EXPECT_FALSE(error.empty());

    // Only the newest dozen stay.
    for (size_t i = 0; i < SignatureStore::kMax + 2; ++i)
        ASSERT_FALSE(store.Add(Filled(8, 8, 0, 0, uint8_t(i)), error).empty()) << Narrow(error);
    EXPECT_EQ(store.List().size(), SignatureStore::kMax);
}

TEST(FillSign, PaperBecomesSeeThroughAndTheInkIsCropped)
{
    using namespace sd::photo;
    // Greyish paper (a photo of a white page) with a blue stroke across the middle.
    Raster r = Filled(200, 100, 228, 226, 222);
    for (UINT y = 45; y < 55; ++y)
        for (UINT x = 50; x < 150; ++x)
        {
            uint8_t* p = r.At(x, y);
            p[0] = 150, p[1] = 40, p[2] = 30;   // BGR: dark blue
        }
    PaperToTransparent(r);
    EXPECT_EQ(r.At(10, 10)[3], 0);
    EXPECT_EQ(r.At(100, 50)[3], 255);
    EXPECT_GT(r.At(100, 50)[0], r.At(100, 50)[2]);   // still blue
    ASSERT_TRUE(TrimToInk(r, 4));
    EXPECT_EQ(r.width, 108u);
    EXPECT_EQ(r.height, 18u);
    Raster empty = Filled(10, 10, 0, 0, 0, 0);
    EXPECT_FALSE(TrimToInk(empty, 2));
    EXPECT_EQ(empty.width, 10u);
}

// ---- Into the PDF --------------------------------------------------------------------------------------

TEST(FillSign, FontsAreCheckedAndSubset)
{
    using namespace sd::pdf;
    const std::string font = TestFont();
    EXPECT_EQ(CheckFont(font, 0, 400, false), FontUse::Embed);
    EXPECT_EQ(CheckFont(TestFont({ 0x0002 }), 0, 400, false), FontUse::Restricted);
    EXPECT_EQ(CheckFont(TestFont({ 0x0200 }), 0, 400, false), FontUse::Restricted);
    EXPECT_EQ(CheckFont(TestFont({ 0x0008 }), 0, 400, false), FontUse::Embed);   // editable embedding is fine
    EXPECT_EQ(CheckFont(TestFont({ 0, true }), 0, 400, false), FontUse::NoOutlines);
    EXPECT_EQ(CheckFont(TestFont({ 0, false, true }), 0, 400, false), FontUse::Embed);
    EXPECT_EQ(CheckFont(TestFont({ 0, false, true }), 0, 700, false), FontUse::Instance);
    EXPECT_EQ(CheckFont("not a font", 0, 400, false), FontUse::Broken);

    // The composite glyph brings the glyphs it is made of; the spare glyph stays out.
    FontSubset subset;
    std::wstring error;
    ASSERT_TRUE(SubsetTrueType(font, 0, { 3, 3 }, subset, error)) << Narrow(error);
    EXPECT_EQ(subset.glyphs, (std::vector<uint16_t>{ 0, 1, 2, 3 }));
    EXPECT_EQ(subset.advances, (std::vector<uint16_t>{ 500, 600, 700, 800 }));
    EXPECT_EQ(subset.postScriptName, "TestSans-Regular");
    EXPECT_EQ(subset.capHeight, 700);
    ASSERT_TRUE(SubsetTrueType(font, 0, { 4, 2 }, subset, error)) << Narrow(error);
    EXPECT_EQ(subset.glyphs, (std::vector<uint16_t>{ 0, 2, 4 }));
    EXPECT_EQ(subset.remap.at(4), 2);
    auto tables = Tables(subset.data);
    EXPECT_EQ(Get16(tables["maxp"], 4), 3);
    EXPECT_EQ(Get16(tables["hhea"], 34), 3);
    EXPECT_EQ(Get16(tables["head"], 50), 1);   // long offsets
    EXPECT_EQ(tables["loca"].size(), 16u);
    EXPECT_EQ(tables["hmtx"].size(), 12u);
    // The whole file sums to the magic number, as a TrueType file must.
    uint32_t sum = 0;
    for (size_t i = 0; i < subset.data.size(); i += 4)
        sum += Get32(subset.data + std::string(3, '\0'), i);
    EXPECT_EQ(sum, 0xB1B0AFBAu);

    // Renumbered composites point at the new numbers of their parts.
    ASSERT_TRUE(SubsetTrueType(font, 0, { 3 }, subset, error));
    ASSERT_TRUE(SubsetTrueType(font, 0, { 2, 3 }, subset, error));
    tables = Tables(subset.data);
    const auto loca = tables["loca"];
    const auto glyf = tables["glyf"];
    const uint32_t at = Get32(loca, subset.remap.at(3) * 4);
    EXPECT_EQ(int16_t(Get16(glyf, at)), -1);
    EXPECT_EQ(Get16(glyf, at + 12), subset.remap.at(1));
    EXPECT_EQ(Get16(glyf, at + 20), subset.remap.at(2));
}

TEST(FillSign, PageMatrixFollowsTheCropBoxAndRotation)
{
    using namespace sd::pdf;
    const double crop[4] = { 10, 20, 210, 120 };   // 200 x 100 points
    double m[6];
    const auto map = [&](PointD p) { return PointD{ m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5] }; };
    PictureToPage(crop, 0, 400, 200, m);
    EXPECT_NEAR(map({ 0, 0 }).x, 10, 1e-9);    // the picture's top-left is the page's top-left
    EXPECT_NEAR(map({ 0, 0 }).y, 120, 1e-9);
    EXPECT_NEAR(map({ 400, 200 }).x, 210, 1e-9);
    EXPECT_NEAR(map({ 400, 200 }).y, 20, 1e-9);
    // Turned a quarter clockwise: the page shows 100 x 200, its left edge on top.
    PictureToPage(crop, 90, 200, 400, m);
    EXPECT_NEAR(map({ 0, 0 }).x, 10, 1e-9);
    EXPECT_NEAR(map({ 0, 0 }).y, 20, 1e-9);
    EXPECT_NEAR(map({ 200, 0 }).y, 120, 1e-9);   // across the top: up the left edge
    EXPECT_NEAR(map({ 0, 400 }).x, 210, 1e-9);
    PictureToPage(crop, 180, 400, 200, m);
    EXPECT_NEAR(map({ 0, 0 }).x, 210, 1e-9);
    EXPECT_NEAR(map({ 0, 0 }).y, 20, 1e-9);
    PictureToPage(crop, 270, 200, 400, m);
    EXPECT_NEAR(map({ 0, 0 }).x, 210, 1e-9);
    EXPECT_NEAR(map({ 0, 0 }).y, 120, 1e-9);
    EXPECT_NEAR(map({ 200, 400 }).x, 10, 1e-9);
}

TEST(FillSign, MarksGoOntoThePageAsTextImagesAndShapes)
{
    using namespace sd::pdf;
    TempDir dir;
    const auto source = SourcePdf(dir, L"form.pdf", 200, 100);
    PageOverlay overlay;
    overlay.width = 400;   // 2 pixels a point
    overlay.height = 200;
    MarkFont font;
    font.file = TestFont();
    font.key = "test";
    font.name = "Test";
    overlay.fonts.push_back(font);
    MarkItem text;
    text.kind = MarkItem::Kind::Glyphs;
    text.glyphs.size = 40;
    text.glyphs.origin = { 20, 100 };
    text.glyphs.glyphs = { 1, 2, 1 };
    text.glyphs.advances = { 24, 30, 24 };   // 600 and 700 units at 40 px, then a gap
    text.glyphs.text = { L"J", L"o", L"e" };
    text.glyphs.rgb = 0x1F3A93;
    overlay.items.push_back(text);
    MarkItem box;
    box.kind = MarkItem::Kind::Path;
    box.path.stroke = true;
    box.path.width = 3;
    box.path.rgb = 0xE81123;
    box.path.alpha = 0.5f;
    box.path.Rect({ 10, 10, 100, 50 });
    overlay.items.push_back(box);
    MarkItem sig;
    sig.kind = MarkItem::Kind::Image;
    auto ink = std::make_shared<Raster>(Filled(4, 2, 20, 30, 140, 255));
    const_cast<Raster&>(*ink).At(0, 0)[3] = 0;   // see-through where there is no ink
    sig.image.raster = ink;
    sig.image.at = { 300, 120, 80, 40 };
    overlay.items.push_back(sig);

    const auto out = dir / L"signed.pdf";
    std::wstring error;
    MarkedPageInfo info;
    ASSERT_TRUE(WriteMarkedPage(source, 0, overlay, out, error, &info)) << Narrow(error);
    EXPECT_EQ(info.textRuns, 1u);
    EXPECT_EQ(info.fonts, 1u);
    EXPECT_EQ(info.images, 1u);
    EXPECT_EQ(info.paths, 1u);

    auto pdf = OpenPdf(out, error);
    ASSERT_TRUE(pdf) << Narrow(error);
    auto pages = QPDFPageDocumentHelper(*pdf).getAllPages();
    ASSERT_EQ(pages.size(), 1u);
    auto mediaBox = pages[0].getMediaBox();
    EXPECT_EQ(mediaBox.getArrayItem(2).getNumericValue(), 200);
    const std::string content = Contents(pages[0]);
    EXPECT_NE(content.find("(Original) Tj"), std::string::npos);    // the page's own content, as it was
    EXPECT_NE(content.find("0.5 0 0 -0.5 0 100 cm"), std::string::npos);
    EXPECT_NE(content.find("40 0 0 -40 20 100 Tm"), std::string::npos);
    EXPECT_NE(content.find("<0001>"), std::string::npos);
    EXPECT_NE(content.find("80 0 0 -40 300 160 cm /SDFillIm1 Do"), std::string::npos);
    EXPECT_NE(content.find("/SDFillGs1 gs"), std::string::npos);

    auto resources = pages[0].getAttribute("/Resources", false);
    EXPECT_TRUE(resources.getKey("/Font").hasKey("/F1"));
    auto type0 = resources.getKey("/Font").getKey("/SDFillF1");
    ASSERT_TRUE(type0.isDictionary());
    EXPECT_EQ(type0.getKey("/Subtype").getName(), "/Type0");
    EXPECT_EQ(type0.getKey("/Encoding").getName(), "/Identity-H");
    const std::string baseFont = type0.getKey("/BaseFont").getName();
    EXPECT_EQ(baseFont.size(), 1 + 7 + std::string("TestSans-Regular").size());
    EXPECT_EQ(baseFont.substr(7), "+TestSans-Regular");
    auto cid = type0.getKey("/DescendantFonts").getArrayItem(0);
    EXPECT_EQ(cid.getKey("/Subtype").getName(), "/CIDFontType2");
    EXPECT_EQ(cid.getKey("/CIDToGIDMap").getName(), "/Identity");
    auto widths = cid.getKey("/W").getArrayItem(1);
    ASSERT_EQ(widths.getArrayNItems(), 3);   // .notdef and the two glyphs used
    EXPECT_EQ(widths.getArrayItem(1).getNumericValue(), 600);
    // The embedded subset is a TrueType file with just those glyphs.
    const std::string embedded = StreamText(cid.getKey("/FontDescriptor").getKey("/FontFile2"));
    EXPECT_EQ(Get16(Tables(embedded)["maxp"], 4), 3);
    EXPECT_NE(StreamText(type0.getKey("/ToUnicode")).find("<0001> <004A>"), std::string::npos);
    auto image = resources.getKey("/XObject").getKey("/SDFillIm1");
    EXPECT_EQ(image.getDict().getKey("/Width").getIntValue(), 4);
    EXPECT_TRUE(image.getDict().getKey("/SMask").isStream());
    EXPECT_NEAR(resources.getKey("/ExtGState").getKey("/SDFillGs1").getKey("/CA").getNumericValue(), 0.5, 1e-6);

    // Read back as text: the page's own words and the typed ones.
    PdfText read;
    ASSERT_TRUE(ReadPdfText(out, false, {}, {}, read, error)) << Narrow(error);
    const std::wstring all = AllText(read);
    EXPECT_NE(all.find(L"Original"), std::wstring::npos) << Narrow(all);
    EXPECT_NE(all.find(L"Jo"), std::wstring::npos) << Narrow(all);

    // A turned page keeps its turn, and the marks are drawn the right way up over it.
    const auto turned = SourcePdf(dir, L"turned.pdf", 200, 100, 90);
    overlay.width = 200;
    overlay.height = 400;
    const auto out2 = dir / L"turned-signed.pdf";
    ASSERT_TRUE(WriteMarkedPage(turned, 0, overlay, out2, error)) << Narrow(error);
    auto pdf2 = OpenPdf(out2, error);
    auto turnedPages = QPDFPageDocumentHelper(*pdf2).getAllPages();
    EXPECT_EQ(RotationOf(turnedPages[0]), 90);
    EXPECT_NE(Contents(turnedPages[0]).find("0 0.5 0.5 0 0 0 cm"), std::string::npos);

    // A page that isn't there, or a font that can't be read, is an error, not a page without its text.
    EXPECT_FALSE(WriteMarkedPage(source, 3, overlay, dir / L"none.pdf", error));
    overlay.fonts[0].file = "broken";
    EXPECT_FALSE(WriteMarkedPage(source, 0, overlay, dir / L"broken.pdf", error));
}

TEST(FillSign, TypedTextBecomesSearchableTextInThePdf)
{
    using namespace sd::photo;
    TempDir dir;
    const auto source = SourcePdf(dir, L"form.pdf", 306, 396);
    auto page = std::make_shared<Raster>(Filled(612, 792, 255, 255, 255));
    Annotations marks;
    Annotation name;
    name.kind = Mark::Text;
    name.text = L"Jane Doe";
    name.font = InstalledFont();
    name.fontSize = 30;
    name.weight = 400;
    name.color = 0x111111;
    name.a = { 100, 100 };
    marks.Add(name);
    Annotation tick;
    tick.kind = Mark::Check;
    tick.a = { 300, 300 };
    tick.b = { 330, 330 };
    tick.color = 0x111111;
    marks.Add(tick);
    sd::pdf::PageOverlay overlay;
    size_t pictures = 0;
    std::wstring error;
    ASSERT_HRESULT_SUCCEEDED(MarksToPdf(Picture::Of(page), marks, {}, overlay, &pictures, error)) << Narrow(error);
    if (pictures > 0)
        GTEST_SKIP() << "the installed font can't be embedded here";
    ASSERT_FALSE(overlay.fonts.empty());
    // The glyphs as DirectWrite placed them, each with the characters it shows.
    std::wstring shown;
    for (const auto& item : overlay.items)
        if (item.kind == sd::pdf::MarkItem::Kind::Glyphs)
            for (const auto& t : item.glyphs.text)
                shown += t;
    EXPECT_EQ(shown, L"Jane Doe");
    const auto out = dir / L"filled.pdf";
    ASSERT_TRUE(sd::pdf::WriteMarkedPage(source, 0, overlay, out, error)) << Narrow(error);
    sd::pdf::PdfText read;
    ASSERT_TRUE(sd::pdf::ReadPdfText(out, false, {}, {}, read, error)) << Narrow(error);
    const std::wstring all = AllText(read);
    EXPECT_NE(all.find(L"Jane Doe"), std::wstring::npos) << Narrow(all);
    EXPECT_NE(all.find(L"Original"), std::wstring::npos) << Narrow(all);
    // The subset font is complete for a PDF reader: embedded program, widths, descriptor, ToUnicode.
    int fonts = 0;
    const auto problems = sd::test::FontProblems(out, "SDFillF", fonts);
    EXPECT_GE(fonts, 1);
    EXPECT_TRUE(problems.empty()) << problems.front();
}

TEST(FillSign, SignedPagesKeepTheirPageAndOpenAgainWithTheirMarks)
{
    using namespace sd::photo;
    using sd::pdf::PageTool;
    TempDir dir;
    const auto source = SourcePdf(dir, L"form.pdf", 306, 396);
    const std::wstring picture = dir / L"page 1-1.png";
    std::wstring error;
    const auto& png = canvas::EditedFormatFor(L".png");
    ASSERT_HRESULT_SUCCEEDED(canvas::SaveRasterToFile(Filled(612, 792, 255, 255, 255), png, canvas::EditedQuality(png), picture, error)) << Narrow(error);
    Annotations marks;
    Annotation name;
    name.kind = Mark::Text;
    name.text = L"Jane Doe";
    name.font = InstalledFont();
    name.fontSize = 30;
    name.a = { 100, 100 };
    marks.Add(name);
    const auto textOf = [&](const std::wstring& pdf) {
        sd::pdf::PdfText read;
        std::wstring why;
        EXPECT_TRUE(sd::pdf::ReadPdfText(pdf, false, {}, {}, read, why)) << Narrow(why);
        return AllText(read);
    };
    const auto count = [](const std::wstring& all, const std::wstring& word) {
        size_t n = 0;
        for (size_t at = all.find(word); at != std::wstring::npos; at = all.find(word, at + 1))
            ++n;
        return n;
    };

    // The marks go over the page, and the page without them and the marks stay beside it.
    bool changed = false;
    const std::wstring first = dir / L"edit 1-1";
    ASSERT_TRUE(sd::pdf::SavePageEdit(PageTool::Sign, marks.ToJson(), picture, source, 0, 0.5, first, changed, error)) << Narrow(error);
    ASSERT_TRUE(changed);
    EXPECT_EQ(count(textOf(first + L".pdf"), L"Original"), 1u);
    EXPECT_EQ(count(textOf(first + L".pdf"), L"Jane Doe"), 1u);
    EXPECT_TRUE(std::filesystem::exists(first + L".png"));
    EXPECT_EQ(sd::pdf::PictureOf(first + L".pdf", PageTool::Sign), first + L".base.png");
    EXPECT_EQ(sd::pdf::PictureOf(first + L".pdf", PageTool::Redact), first + L".png");

    // Edited again, it opens with its marks; unchanged, nothing is written.
    const std::wstring base = sd::pdf::PictureOf(first + L".pdf", PageTool::Sign);
    auto tool = sd::pdf::MakePageTool(PageTool::Sign, { base });
    auto* annotate = dynamic_cast<AnnotateTool*>(tool.get());
    ASSERT_NE(annotate, nullptr);
    ASSERT_EQ(annotate->Marks().Items().size(), 1u);
    EXPECT_EQ(annotate->Marks().Items()[0].text, L"Jane Doe");
    const std::wstring second = dir / L"edit 2-1";
    ASSERT_TRUE(sd::pdf::SavePageEdit(PageTool::Sign, marks.ToJson(), base, source, 0, 0.5, second, changed, error)) << Narrow(error);
    EXPECT_FALSE(changed);
    EXPECT_FALSE(std::filesystem::exists(second + L".pdf"));
    // A mark added: the text is there once, not once per edit.
    Annotation tick;
    tick.kind = Mark::Check;
    tick.a = { 300, 300 };
    tick.b = { 330, 330 };
    marks.Add(tick);
    ASSERT_TRUE(sd::pdf::SavePageEdit(PageTool::Sign, marks.ToJson(), base, source, 0, 0.5, second, changed, error)) << Narrow(error);
    EXPECT_TRUE(changed);
    EXPECT_EQ(count(textOf(second + L".pdf"), L"Jane Doe"), 1u);
    EXPECT_EQ(count(textOf(second + L".pdf"), L"Original"), 1u);

    // A page redacted before is signed over the redacted page, never over the original.
    const std::wstring redacted = dir / L"edit 3-1";
    std::filesystem::copy_file(picture, redacted + L".png");
    ASSERT_TRUE(sd::pdf::WritePicturePage(redacted + L".png", 0.5, redacted + L".pdf", error)) << Narrow(error);
    const std::wstring fourth = dir / L"edit 4-1";
    ASSERT_TRUE(sd::pdf::SavePageEdit(PageTool::Sign, marks.ToJson(), redacted + L".png", source, 0, 0.5, fourth, changed, error)) << Narrow(error);
    EXPECT_TRUE(changed);
    EXPECT_EQ(count(textOf(fourth + L".pdf"), L"Original"), 0u);
    EXPECT_EQ(count(textOf(fourth + L".pdf"), L"Jane Doe"), 1u);
}
