#include "Ask.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "docs/Bytes.h"
#include "docs/Zip.h"
#include "sheets/SheetsModule.h"
#include "sheets/Workbook.h"

#include <objbase.h>
#include <wrl/client.h>

#include <cstring>

using namespace sd;
using namespace sd::sheets;
using namespace sd::test;

namespace
{
    // Answers the questions of src/Ask.h for one test.
    struct ScopedAnswers
    {
        explicit ScopedAnswers(ask::ChooseFn choose) { ask::SetAnswers(std::move(choose), {}); }
        ~ScopedAnswers() { ask::SetAnswers({}, {}); }
    };

    Cell DateCell(double serial) { return Cell{ CellType::Date, serial, {} }; }

    // The workbook every round trip uses: text that needs escaping, numbers, a date, a date-time,
    // booleans, an error, gaps, and a second sheet.
    Workbook SampleBook()
    {
        Workbook book;
        Sheet& a = book.sheets.emplace_back();
        a.name = "Data & more";
        a.Set(0, 0, Cell::Str("Name"));
        a.Set(0, 1, Cell::Str("Value"));
        a.Set(0, 2, Cell::Str("When"));
        a.Set(1, 0, Cell::Str("a & <b> \"q\""));
        a.Set(1, 1, Cell::Num(3.25));
        a.Set(1, 2, DateCell(45000));
        a.Set(2, 0, Cell{ CellType::Bool, 1, {} });
        a.Set(2, 1, Cell::Num(-1.5e-7));
        a.Set(2, 2, DateCell(45000.75));
        a.Set(3, 0, Cell::Str("  two  spaces"));
        a.Set(3, 1, Cell::Str("line\nbreak"));
        a.Set(3, 2, Cell::Str("Ünïcödé 漢字"));
        a.Set(4, 3, Cell{ CellType::Error, 0, "#N/A" });
        Sheet& b = book.sheets.emplace_back();
        b.name = "Second";
        b.Set(0, 0, Cell::Num(1));
        b.Set(2, 3, Cell::Str("x"));
        return book;
    }

    void ExpectSameCells(const Workbook& expected, const Workbook& actual)
    {
        ASSERT_EQ(actual.sheets.size(), expected.sheets.size());
        for (size_t s = 0; s < expected.sheets.size(); ++s)
        {
            const auto& e = expected.sheets[s];
            const auto& a = actual.sheets[s];
            EXPECT_EQ(a.name, e.name);
            ASSERT_EQ(a.rows.size(), e.rows.size()) << e.name;
            for (size_t r = 0; r < e.rows.size(); ++r)
            {
                ASSERT_EQ(a.rows[r].size(), e.rows[r].size()) << e.name << " row " << r;
                for (size_t c = 0; c < e.rows[r].size(); ++c)
                {
                    const auto& ec = e.rows[r][c];
                    const auto& ac = a.rows[r][c];
                    EXPECT_EQ(ac.type, ec.type) << e.name << " " << ColumnName(c) << r + 1;
                    EXPECT_NEAR(ac.number, ec.number, 1e-9) << e.name << " " << ColumnName(c) << r + 1;
                    EXPECT_EQ(ac.text, ec.text) << e.name << " " << ColumnName(c) << r + 1;
                }
            }
        }
    }

    std::string Rec(uint16_t type, const std::string& data)
    {
        std::string r;
        r += static_cast<char>(type & 0xFF);
        r += static_cast<char>(type >> 8);
        r += static_cast<char>(data.size() & 0xFF);
        r += static_cast<char>(data.size() >> 8);
        return r + data;
    }
    std::string U16(uint16_t v) { return { static_cast<char>(v & 0xFF), static_cast<char>(v >> 8) }; }
    std::string U32(uint32_t v) { return U16(static_cast<uint16_t>(v)) + U16(static_cast<uint16_t>(v >> 16)); }
    std::string F64(double v)
    {
        std::string s(8, '\0');
        std::memcpy(s.data(), &v, 8);
        return s;
    }
    std::string Cell3(uint16_t row, uint16_t col, uint16_t xf) { return U16(row) + U16(col) + U16(xf); }

    // A small BIFF8 workbook stream: one sheet with every record kind the reader knows.
    std::string Biff8Stream()
    {
        const auto bof = [](uint16_t kind) { return Rec(0x0809, U16(0x0600) + U16(kind) + std::string(12, '\0')); };
        std::string globals = bof(0x0005);
        for (int i = 0; i < 16; ++i)
            globals += Rec(0x00E0, U16(0) + U16(0) + std::string(16, '\0'));     // XF 0..15: General
        globals += Rec(0x00E0, U16(0) + U16(14) + std::string(16, '\0'));        // XF 16: a date
        globals += Rec(0x0022, U16(0));                                          // DATEMODE: 1900
        // SST: "Name", "Café" (compressed, Latin-1), "Hello world" split across a CONTINUE.
        const std::string sst = U32(3) + U32(3) + U16(4) + '\0' + "Name" + U16(4) + '\0' + "Caf\xE9" + U16(11) + '\0' + "Hello ";
        const std::string cont = std::string(1, '\x01') + std::string("w\0o\0r\0l\0d\0", 10);   // the rest, now UTF-16
        const std::string sheetName = "Sheet A";
        const size_t boundSize = 4 + 8 + sheetName.size();
        const size_t sheetOffset = globals.size() + boundSize + (4 + sst.size()) + (4 + cont.size()) + 4;
        globals += Rec(0x0085, U32(static_cast<uint32_t>(sheetOffset)) + '\0' + '\0' + static_cast<char>(sheetName.size()) + '\0' + sheetName);
        globals += Rec(0x00FC, sst);
        globals += Rec(0x003C, cont);
        globals += Rec(0x000A, "");

        std::string sheet = bof(0x0010);
        sheet += Rec(0x00FD, Cell3(0, 0, 0) + U32(0));                     // A1 "Name"
        sheet += Rec(0x00FD, Cell3(0, 1, 0) + U32(1));                     // B1 "Café"
        sheet += Rec(0x0203, Cell3(1, 0, 0) + F64(3.5));                   // A2 3.5
        sheet += Rec(0x027E, Cell3(1, 1, 0) + U32((42u << 2) | 2));        // B2 42 (integer RK)
        sheet += Rec(0x00BD, U16(2) + U16(0) + U16(0) + U32((125u << 2) | 3) + U16(16) + U32((45000u << 2) | 2) + U16(1));   // A3 1.25, B3 a date
        sheet += Rec(0x0205, Cell3(3, 0, 0) + '\x01' + '\0');               // A4 TRUE
        sheet += Rec(0x0205, Cell3(3, 1, 0) + '\x2A' + '\x01');             // B4 #N/A
        sheet += Rec(0x0006, Cell3(4, 0, 0) + std::string(6, '\0') + U16(0xFFFF) + std::string(8, '\0'));   // A5: string formula...
        sheet += Rec(0x0207, U16(4) + '\0' + "calc");                       // ...whose result is "calc"
        sheet += Rec(0x0204, Cell3(4, 1, 0) + U16(1) + '\x01' + std::string("\xA9\x03", 2));   // B5 "Ω" (UTF-16)
        sheet += Rec(0x00FD, Cell3(5, 0, 0) + U32(2));                     // A6 "Hello world"
        sheet += Rec(0x000A, "");
        return globals + sheet;
    }
}

TEST(Sheets, NumbersAndDatesAsText)
{
    EXPECT_EQ(FormatNumber(3), "3");
    EXPECT_EQ(FormatNumber(0.1), "0.1");
    EXPECT_EQ(FormatNumber(-2.5), "-2.5");
    EXPECT_EQ(FormatNumber(1e20), "1e+20");
    EXPECT_EQ(SerialToIso(45000), "2023-03-15");
    EXPECT_EQ(SerialToIso(45000.5), "2023-03-15 12:00:00");
    EXPECT_EQ(SerialToIso(0.75), "18:00:00");
    EXPECT_EQ(SerialToIso(1), "1900-01-01");
    EXPECT_EQ(SerialToIso(59), "1900-02-28");   // before Excel's fictitious 1900-02-29
    EXPECT_EQ(SerialToIso(61), "1900-03-01");
    double serial = 0;
    ASSERT_TRUE(IsoToSerial("2023-03-15", serial));
    EXPECT_EQ(serial, 45000);
    ASSERT_TRUE(IsoToSerial("2023-03-15T18:00:00", serial));
    EXPECT_NEAR(serial, 45000.75, 1e-9);
    ASSERT_TRUE(IsoToSerial("1900-01-01", serial));
    EXPECT_EQ(serial, 1);
    EXPECT_FALSE(IsoToSerial("15/03/2023", serial));

    EXPECT_EQ(ColumnName(0), "A");
    EXPECT_EQ(ColumnName(25), "Z");
    EXPECT_EQ(ColumnName(26), "AA");
    EXPECT_EQ(ColumnName(701), "ZZ");
    size_t row = 0, col = 0;
    ASSERT_TRUE(ParseCellRef("AB12", row, col));
    EXPECT_EQ(row, 11u);
    EXPECT_EQ(col, 27u);
    EXPECT_FALSE(ParseCellRef("12", row, col));

    double v = 0;
    EXPECT_TRUE(LooksNumeric("42", v));
    EXPECT_TRUE(LooksNumeric("-3.5", v));
    EXPECT_EQ(v, -3.5);
    EXPECT_TRUE(LooksNumeric("1e6", v));
    EXPECT_TRUE(LooksNumeric("0.5", v));
    EXPECT_FALSE(LooksNumeric("007", v));            // keeps its zeros as text
    EXPECT_FALSE(LooksNumeric("+1 555 0100", v));
    EXPECT_FALSE(LooksNumeric("1234567890123456", v));   // more digits than a double holds
    EXPECT_FALSE(LooksNumeric("", v));
    EXPECT_FALSE(LooksNumeric("12abc", v));
}

TEST(Sheets, CsvQuotingBothWays)
{
    const std::string csv = "name,quote,notes\r\n\"Smith, Ann\",\"She said \"\"hi\"\"\",\"two\r\nlines\"\r\n,,\r\nlast,1,\r\n";
    const auto sheet = ReadCsv(csv);
    ASSERT_EQ(sheet.rows.size(), 4u);
    EXPECT_TRUE(sheet.rows[2].empty());   // ",,": a blank row in the middle stays
    EXPECT_EQ(sheet.rows[3][1].text, "1");
    EXPECT_EQ(sheet.rows[1][0].text, "Smith, Ann");
    EXPECT_EQ(sheet.rows[1][1].text, "She said \"hi\"");
    EXPECT_EQ(sheet.rows[1][2].text, "two\nlines");
    EXPECT_TRUE(sheet.untyped);

    Sheet out;
    out.Set(0, 0, Cell::Str("plain"));
    out.Set(0, 1, Cell::Str("a,b"));
    out.Set(0, 2, Cell::Str("say \"x\""));
    out.Set(1, 0, Cell::Str("multi\nline"));
    out.Set(1, 1, Cell::Num(2.5));
    out.Set(2, 2, Cell{ CellType::Bool, 0, {} });
    EXPECT_EQ(WriteCsv(out, ','), "\xEF\xBB\xBF" "plain,\"a,b\",\"say \"\"x\"\"\"\r\n\"multi\nline\",2.5,\r\n,,FALSE\r\n");
    // Round trip through text.
    const auto back = ReadCsv(docs::DecodeText(WriteCsv(out, ',')));
    EXPECT_EQ(back.rows[0][2].text, "say \"x\"");
    EXPECT_EQ(back.rows[1][0].text, "multi\nline");

    // Tabs are quoted in TSV, commas are not.
    Sheet tsv;
    tsv.Set(0, 0, Cell::Str("a,b"));
    tsv.Set(0, 1, Cell::Str("c\td"));
    EXPECT_EQ(WriteCsv(tsv, '\t'), "\xEF\xBB\xBF" "a,b\t\"c\td\"\r\n");
}

TEST(Sheets, CsvDelimitersAndEncodings)
{
    // Semicolons (European Excel) are detected; a comma inside is then just text.
    const auto semi = ReadCsv("a;b;c\n1,5;2;3\n");
    ASSERT_EQ(semi.rows.size(), 2u);
    ASSERT_EQ(semi.rows[0].size(), 3u);
    EXPECT_EQ(semi.rows[1][0].text, "1,5");
    const auto tabs = ReadCsv("a\tb\n1\t2");
    ASSERT_EQ(tabs.rows.size(), 2u);
    EXPECT_EQ(tabs.rows[1][1].text, "2");

    // UTF-8 with a BOM, UTF-16 LE with a BOM and Windows-1252 all decode to the same text.
    const std::string expected = "café,naïve";
    EXPECT_EQ(ReadCsv(docs::DecodeText("\xEF\xBB\xBF" "café,naïve")).rows[0][1].text, "naïve");
    const std::wstring wide = L"\xFEFF" L"café,naïve";
    const auto utf16 = docs::DecodeText(std::string(reinterpret_cast<const char*>(wide.data()), wide.size() * 2));
    EXPECT_EQ(utf16, expected);
    EXPECT_EQ(docs::DecodeText("caf\xE9,na\xEFve"), expected);
}

TEST(Sheets, XlsxRoundTrip)
{
    const auto book = SampleBook();
    std::string bytes;
    ASSERT_TRUE(WriteXlsx(book, bytes));
    docs::ZipReader zip;
    ASSERT_TRUE(zip.Open(bytes));
    EXPECT_TRUE(zip.Has("[Content_Types].xml"));
    EXPECT_TRUE(zip.Has("xl/workbook.xml"));
    EXPECT_TRUE(zip.Has("xl/worksheets/sheet2.xml"));

    Workbook back;
    std::string error;
    ASSERT_TRUE(ReadXlsx(bytes, back, error)) << error;
    ExpectSameCells(book, back);
}

TEST(Sheets, XlsxFromCsvStoresNumbers)
{
    auto sheet = ReadCsv("id,code,amount\n1,007,12.50\n");
    sheet.name = "data";
    Workbook book;
    book.sheets.push_back(sheet);
    std::string bytes;
    ASSERT_TRUE(WriteXlsx(book, bytes));
    Workbook back;
    std::string error;
    ASSERT_TRUE(ReadXlsx(bytes, back, error)) << error;
    const auto& r = back.sheets[0].rows[1];
    EXPECT_EQ(r[0].type, CellType::Number);
    EXPECT_EQ(r[1].type, CellType::Text);   // leading zeros keep it text
    EXPECT_EQ(r[1].text, "007");
    EXPECT_EQ(r[2].type, CellType::Number);
    EXPECT_EQ(r[2].number, 12.5);
}

TEST(Sheets, OdsRoundTrip)
{
    const auto book = SampleBook();
    std::string bytes;
    ASSERT_TRUE(WriteOds(book, bytes));
    // ODF: "mimetype" first and stored, like EPUB.
    ASSERT_GT(bytes.size(), 60u);
    EXPECT_EQ(bytes.substr(30, 8), "mimetype");
    EXPECT_EQ(bytes.substr(38, 46), "application/vnd.oasis.opendocument.spreadsheet");

    Workbook back;
    std::string error;
    ASSERT_TRUE(ReadOds(bytes, back, error)) << error;
    // ODF has no error value without a formula: "#N/A" comes back as its text.
    auto expected = book;
    expected.sheets[0].rows[4][3].type = CellType::Text;
    ExpectSameCells(expected, back);
}

TEST(Sheets, OdsRepeatedCellsAreExpanded)
{
    docs::ZipWriter zip;
    zip.Add("mimetype", "application/vnd.oasis.opendocument.spreadsheet", true);
    zip.Add("content.xml",
            R"(<?xml version="1.0"?><office:document-content xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0">)"
            R"(<office:body><office:spreadsheet><table:table table:name="T">)"
            R"(<table:table-row><table:table-cell office:value-type="float" office:value="7" table:number-columns-repeated="3"><text:p>7</text:p></table:table-cell><table:table-cell table:number-columns-repeated="16000"/></table:table-row>)"
            R"(<table:table-row table:number-rows-repeated="2"><table:table-cell office:value-type="string"><text:p>a<text:s text:c="2"/>b</text:p></table:table-cell></table:table-row>)"
            R"(<table:table-row table:number-rows-repeated="1048000"><table:table-cell/></table:table-row>)"
            R"(</table:table></office:spreadsheet></office:body></office:document-content>)");
    std::string bytes;
    ASSERT_TRUE(zip.Finish(bytes));
    Workbook book;
    std::string error;
    ASSERT_TRUE(ReadOds(bytes, book, error)) << error;
    const auto& rows = book.sheets.at(0).rows;
    ASSERT_EQ(rows.size(), 3u);   // the trailing empty rows are not materialised
    ASSERT_EQ(rows[0].size(), 3u);
    EXPECT_EQ(rows[0][2].number, 7);
    EXPECT_EQ(rows[2][0].text, "a  b");
}

TEST(Sheets, Biff8Records)
{
    Workbook book;
    std::string error;
    ASSERT_TRUE(ParseBiff8(Biff8Stream(), book, error)) << error;
    ASSERT_EQ(book.sheets.size(), 1u);
    const auto& s = book.sheets[0];
    EXPECT_EQ(s.name, "Sheet A");
    ASSERT_EQ(s.rows.size(), 6u);
    EXPECT_EQ(s.rows[0][0].text, "Name");
    EXPECT_EQ(s.rows[0][1].text, "Café");
    EXPECT_EQ(s.rows[1][0].number, 3.5);
    EXPECT_EQ(s.rows[1][1].number, 42);
    EXPECT_EQ(s.rows[2][0].number, 1.25);
    EXPECT_EQ(s.rows[2][1].type, CellType::Date);
    EXPECT_EQ(CellText(s.rows[2][1]), "2023-03-15");
    EXPECT_EQ(s.rows[3][0].type, CellType::Bool);
    EXPECT_EQ(CellText(s.rows[3][0]), "TRUE");
    EXPECT_EQ(s.rows[3][1].type, CellType::Error);
    EXPECT_EQ(s.rows[3][1].text, "#N/A");
    EXPECT_EQ(s.rows[4][0].text, "calc");
    EXPECT_EQ(s.rows[4][1].text, "Ω");
    EXPECT_EQ(s.rows[5][0].text, "Hello world");

    Workbook none;
    EXPECT_FALSE(ParseBiff8("not a workbook at all", none, error));
}

TEST(Sheets, XlsThroughStructuredStorage)
{
    using Microsoft::WRL::ComPtr;
    // An in-memory compound file holding a "Workbook" stream, like a real .xls.
    ComPtr<ILockBytes> lockBytes;
    ASSERT_HRESULT_SUCCEEDED(CreateILockBytesOnHGlobal(nullptr, TRUE, &lockBytes));
    {
        ComPtr<IStorage> storage;
        ASSERT_HRESULT_SUCCEEDED(StgCreateDocfileOnILockBytes(lockBytes.Get(), STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &storage));
        ComPtr<IStream> stream;
        ASSERT_HRESULT_SUCCEEDED(storage->CreateStream(L"Workbook", STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stream));
        const auto biff = Biff8Stream();
        ULONG written = 0;
        ASSERT_HRESULT_SUCCEEDED(stream->Write(biff.data(), static_cast<ULONG>(biff.size()), &written));
        stream.Reset();
        ASSERT_HRESULT_SUCCEEDED(storage->Commit(STGC_DEFAULT));
    }
    STATSTG stat{};
    ASSERT_HRESULT_SUCCEEDED(lockBytes->Stat(&stat, STATFLAG_NONAME));
    std::string file(static_cast<size_t>(stat.cbSize.QuadPart), '\0');
    ULONG read = 0;
    ULARGE_INTEGER zero{};
    ASSERT_HRESULT_SUCCEEDED(lockBytes->ReadAt(zero, file.data(), static_cast<ULONG>(file.size()), &read));
    ASSERT_EQ(read, file.size());

    Workbook book;
    std::string error;
    ASSERT_TRUE(ReadXls(file, book, error)) << error;
    EXPECT_EQ(book.sheets.at(0).rows.at(5).at(0).text, "Hello world");

    // A zip is not an .xls.
    Workbook other;
    EXPECT_FALSE(ReadXls("PK\x03\x04 nothing", other, error));
}

TEST(Sheets, SafeNames)
{
    Workbook book;
    for (const char* n : { "Q1/Q2", "Q1/Q2", "", "A name that is much longer than thirty-one characters", "[x]" })
        book.sheets.emplace_back().name = n;
    const auto names = SafeSheetNames(book);
    ASSERT_EQ(names.size(), 5u);
    EXPECT_EQ(names[0], "Q1_Q2");
    EXPECT_NE(names[1], names[0]);
    EXPECT_FALSE(names[2].empty());
    EXPECT_LE(names[3].size(), 31u);
    EXPECT_EQ(names[4].find_first_of("[]"), std::string::npos);

    EXPECT_EQ(SafeFileName("Q1/Q2"), L"Q1_Q2");
    EXPECT_EQ(SafeFileName("con"), L"_con");
    EXPECT_EQ(SafeFileName("dots..."), L"dots");
    EXPECT_EQ(SafeFileName(""), L"Sheet");
}

TEST(Sheets, WorkbookAsDocumentForPdf)
{
    const auto doc = WorkbookToDocument(SampleBook(), 3);
    // Two sheets: a heading and a table each, then one note at the very end (the last page) for the
    // cut-off first sheet.
    ASSERT_EQ(doc.blocks.size(), 5u);
    EXPECT_EQ(doc.blocks[0].kind, docs::BlockKind::Heading);
    EXPECT_EQ(doc.blocks[1].kind, docs::BlockKind::Table);
    EXPECT_TRUE(doc.blocks[1].headerRow);
    EXPECT_EQ(doc.blocks[1].rows.size(), 3u);
    EXPECT_EQ(doc.blocks[1].rows[0].size(), 4u);   // padded to the widest row
    EXPECT_EQ(docs::PlainText(doc.blocks[1].rows[1][2].spans), "2023-03-15");
    EXPECT_EQ(doc.blocks[2].kind, docs::BlockKind::Heading);
    EXPECT_EQ(doc.blocks[4].kind, docs::BlockKind::Paragraph);
    EXPECT_EQ(docs::PlainText(doc.blocks[4].spans), "2 rows were left out: only the first 3 rows of \xE2\x80\x9C" "Data & more\xE2\x80\x9D are printed.");

    // Every row: no note.
    const auto all = WorkbookToDocument(SampleBook(), SIZE_MAX);
    ASSERT_EQ(all.blocks.size(), 4u);
    EXPECT_EQ(all.blocks[1].rows.size(), 5u);
}

TEST(Sheets, RowsLeftOutOfAPdf)
{
    Workbook book;
    Sheet& a = book.sheets.emplace_back();
    a.name = "A";
    a.Set(kPdfRowLimit + 9, 0, Cell::Num(1));   // 20,010 rows
    Sheet& b = book.sheets.emplace_back();
    b.name = "B";
    b.Set(kPdfRowLimit - 1, 0, Cell::Num(1));   // exactly 20,000: all printed
    EXPECT_EQ(RowsLeftOut(book), 10u);
    b.Set(kPdfRowLimit + 4, 0, Cell::Num(1));
    EXPECT_EQ(RowsLeftOut(book), 15u);
    const auto doc = WorkbookToDocument(book);
    EXPECT_EQ(docs::PlainText(doc.blocks.back().spans), "15 rows were left out: only the first 20,000 rows of each sheet are printed.");
    EXPECT_EQ(RowsLeftOut(SampleBook()), 0u);
}

TEST(Sheets, ALongSheetAsksBeforeItIsPrinted)
{
    TempDir dir;
    Recipes recipes(dir);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    std::string csv = "n\n";
    for (int i = 0; i < 20000; ++i)
        csv += std::to_string(i) + "\n";
    const auto source = dir / L"long.csv";   // 20,001 rows with the header
    WriteText(source, csv);

    std::vector<ask::Question> asked;
    ScopedAnswers answers([&](const ask::Question& q) {
        asked.push_back(q);
        return ask::kCancel;
    });
    // Straight to the engine: the question comes before any printing, so no PDF printer is needed
    // (PipelineRunner::Run would first check that there is one).
    const auto& recipe = recipes.Get("sheets-to-pdf");
    const auto r = runner.EngineFor(recipe)->Run(recipe, 0, source, {}, {});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.skipped) << "Cancel stops it, and it is not a failure";
    EXPECT_NE(r.error.find(L"nothing was printed"), std::wstring::npos) << Narrow(r.error);
    EXPECT_EQ(CountFiles(dir, L".pdf"), 0u);
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
    ASSERT_EQ(asked.size(), 1u);
    EXPECT_EQ(asked[0].title, L"Spreadsheet to PDF");
    EXPECT_EQ(asked[0].heading, L"long.csv has 20,001 rows");
    ASSERT_EQ(asked[0].choices.size(), 2u);
    EXPECT_EQ(asked[0].choices[0].label, L"Print all rows");
    EXPECT_EQ(asked[0].choices[1].label, L"Print the first 20,000 rows");
    EXPECT_EQ(asked[0].choices[1].note, L"A note on the last page says 1 row was left out");
    EXPECT_EQ(asked[0].unattended, asked[0].choices[1].id) << "with no one to ask, the first 20,000 as before";

    // Other formats never ask: a CSV to XLSX keeps every row.
    asked.clear();
    const auto xlsx = runner.Run(recipes.Get("sheets-to-xlsx"), 0, source, {});
    ASSERT_TRUE(xlsx.ok) << Narrow(xlsx.error);
    EXPECT_TRUE(asked.empty());
}

TEST(Sheets, EngineConversions)
{
    TempDir dir;
    Recipes recipes(dir);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);

    std::string xlsx;
    ASSERT_TRUE(WriteXlsx(SampleBook(), xlsx));
    const auto source = dir / L"book.xlsx";
    WriteText(source, xlsx);
    EXPECT_TRUE(runner.Applies(recipes.Get("sheets-to-csv"), source));
    EXPECT_TRUE(runner.Applies(recipes.Get("sheets-to-ods"), source));
    EXPECT_FALSE(runner.Applies(recipes.Get("sheets-to-csv"), dir / L"a.csv"));
    EXPECT_TRUE(runner.Applies(recipes.Get("sheets-to-xlsx"), dir / L"a.csv"));
    EXPECT_FALSE(runner.Applies(recipes.Get("sheets-to-xlsx"), dir / L"a.docx"));

    // Two sheets to CSV: a folder with one file per sheet.
    const auto csv = runner.Run(recipes.Get("sheets-to-csv"), 0, source, {});
    ASSERT_TRUE(csv.ok) << Narrow(csv.error);
    ASSERT_TRUE(std::filesystem::is_directory(csv.output));
    const auto first = ReadAll((std::filesystem::path(csv.output) / L"Data & more.csv").wstring());
    EXPECT_TRUE(first.starts_with("\xEF\xBB\xBF" "Name,Value,When,\r\n\"a & <b> \"\"q\"\"\",3.25,2023-03-15,\r\n")) << first;
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(csv.output) / L"Second.csv"));

    // One sheet to CSV: a file; and CSV back to XLSX.
    const auto single = dir / L"one.csv";
    WriteText(single, "a,b\n1,2\n");
    const auto toXlsx = runner.Run(recipes.Get("sheets-to-xlsx"), 0, single, {});
    ASSERT_TRUE(toXlsx.ok) << Narrow(toXlsx.error);
    Workbook back;
    std::string error;
    ASSERT_TRUE(ReadXlsx(ReadAll(toXlsx.output), back, error)) << error;
    EXPECT_EQ(back.sheets.at(0).name, "one");
    EXPECT_EQ(back.sheets[0].rows.at(1).at(1).type, CellType::Number);

    const auto tsv = runner.Run(recipes.Get("sheets-to-tsv"), 0, single, {});
    ASSERT_TRUE(tsv.ok) << Narrow(tsv.error);
    EXPECT_EQ(ReadAll(tsv.output), "\xEF\xBB\xBF" "a\tb\r\n1\t2\r\n");

    const auto broken = dir / L"broken.xlsx";
    WriteText(broken, "not a zip");
    EXPECT_FALSE(runner.Run(recipes.Get("sheets-to-csv"), 0, broken, {}).ok);
}
