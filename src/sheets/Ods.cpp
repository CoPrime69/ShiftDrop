#include "sheets/Workbook.h"
#include "docs/Bytes.h"
#include "docs/Zip.h"

#include <pugixml.hpp>

#include <cmath>
#include <format>

// ODS (OpenDocument spreadsheet, ISO/IEC 26300): content.xml inside a zip whose first entry is an
// uncompressed "mimetype". Rows and cells can carry repeat counts; LibreOffice pads every sheet
// with a repeated empty row and column up to the sheet's limits, so empty repeats are only
// expanded when real content follows them.
namespace sd::sheets
{
    namespace
    {
        using docs::XmlEscape;

        std::string_view Local(const char* name)
        {
            const std::string_view n(name);
            const auto colon = n.find(':');
            return colon == std::string_view::npos ? n : n.substr(colon + 1);
        }

        std::string Attr(pugi::xml_node node, std::string_view local)
        {
            for (auto a = node.first_attribute(); a; a = a.next_attribute())
                if (Local(a.name()) == local)
                    return a.value();
            return {};
        }

        // The text of a <text:p>, with <text:s> (spaces), <text:tab>, <text:line-break> and spans.
        void ParagraphText(pugi::xml_node node, std::string& out)
        {
            for (auto c = node.first_child(); c; c = c.next_sibling())
            {
                if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata)
                {
                    out += c.value();
                    continue;
                }
                const auto n = Local(c.name());
                if (n == "s")
                {
                    const int count = std::max(1, std::atoi(Attr(c, "c").c_str()));
                    out.append(static_cast<size_t>(std::min(count, 1000)), ' ');
                }
                else if (n == "tab")
                    out.push_back('\t');
                else if (n == "line-break")
                    out.push_back('\n');
                else if (n != "annotation" && n != "note")
                    ParagraphText(c, out);
            }
        }

        // "PT13H45M00S" -> fraction of a day.
        double ParseDuration(std::string_view s)
        {
            double h = 0, m = 0, sec = 0, cur = 0, scale = 0;
            bool frac = false;
            for (const char c : s)
            {
                if (c >= '0' && c <= '9')
                {
                    if (frac)
                    {
                        scale /= 10;
                        cur += (c - '0') * scale;
                    }
                    else
                        cur = cur * 10 + (c - '0');
                }
                else if (c == '.' || c == ',')
                {
                    frac = true;
                    scale = 1;
                }
                else
                {
                    if (c == 'H') h = cur;
                    else if (c == 'M') m = cur;
                    else if (c == 'S') sec = cur;
                    cur = 0;
                    frac = false;
                }
            }
            return (h * 3600 + m * 60 + sec) / 86400.0;
        }

        Cell ReadCell(pugi::xml_node c)
        {
            const auto type = Attr(c, "value-type");
            if (type == "float" || type == "percentage" || type == "currency")
                return Cell::Num(std::strtod(Attr(c, "value").c_str(), nullptr));
            if (type == "date")
            {
                double serial = 0;
                if (IsoToSerial(Attr(c, "date-value"), serial))
                    return Cell{ CellType::Date, serial, {} };
            }
            if (type == "time")
                return Cell{ CellType::Date, ParseDuration(Attr(c, "time-value")), {} };
            if (type == "boolean")
                return Cell{ CellType::Bool, Attr(c, "boolean-value") == "true" ? 1.0 : 0.0, {} };
            std::string text;
            bool first = true;
            for (auto p = c.first_child(); p; p = p.next_sibling())
                if (Local(p.name()) == "p")
                {
                    if (!first)
                        text.push_back('\n');
                    ParagraphText(p, text);
                    first = false;
                }
            if (text.empty())
            {
                if (const auto sv = Attr(c, "string-value"); !sv.empty())
                    text = sv;
            }
            if (text.empty())
                return {};
            // A formula error shows as its text ("#DIV/0!") and has no value type.
            if (type.empty() && text.size() > 1 && text[0] == '#' && (text.back() == '!' || text.back() == '?' || text == "#N/A"))
                return Cell{ CellType::Error, 0, text };
            return Cell::Str(std::move(text));
        }

        void ReadRows(pugi::xml_node parent, Sheet& sheet, size_t& row, size_t& pendingEmpty)
        {
            for (auto r = parent.first_child(); r; r = r.next_sibling())
            {
                const auto n = Local(r.name());
                if (n == "table-row-group" || n == "table-header-rows" || n == "table-rows")
                {
                    ReadRows(r, sheet, row, pendingEmpty);
                    continue;
                }
                if (n != "table-row")
                    continue;
                const size_t repeat = static_cast<size_t>(std::max(1, std::atoi(Attr(r, "number-rows-repeated").c_str())));
                std::vector<Cell> cells;
                size_t pendingCells = 0;
                for (auto c = r.first_child(); c; c = c.next_sibling())
                {
                    const auto cn = Local(c.name());
                    if (cn != "table-cell" && cn != "covered-table-cell")
                        continue;
                    const size_t cr = static_cast<size_t>(std::max(1, std::atoi(Attr(c, "number-columns-repeated").c_str())));
                    Cell cell = cn == "table-cell" ? ReadCell(c) : Cell{};
                    if (cell.type == CellType::Empty)
                    {
                        pendingCells += cr;
                        continue;
                    }
                    cells.resize(std::min<size_t>(cells.size() + pendingCells, 16384));
                    pendingCells = 0;
                    for (size_t k = 0; k < std::min<size_t>(cr, 1024) && cells.size() < 16384; ++k)
                        cells.push_back(cell);
                }
                if (cells.empty())
                {
                    pendingEmpty += repeat;
                    continue;
                }
                row += pendingEmpty;
                pendingEmpty = 0;
                for (size_t k = 0; k < std::min<size_t>(repeat, 1024) && row < 1048576; ++k)
                {
                    for (size_t col = 0; col < cells.size(); ++col)
                        sheet.Set(row, col, cells[col]);
                    ++row;
                }
            }
        }
    }

    bool ReadOds(std::string bytes, Workbook& book, std::string& error)
    {
        docs::ZipReader zip;
        if (!zip.Open(std::move(bytes)))
        {
            error = "not an OpenDocument spreadsheet (not a zip package)";
            return false;
        }
        const auto xml = zip.Read("content.xml");
        pugi::xml_document d;
        if (!xml || !d.load_buffer(xml->data(), xml->size(), pugi::parse_default | pugi::parse_ws_pcdata))
        {
            error = "not an OpenDocument spreadsheet (no content.xml)";
            return false;
        }
        pugi::xml_node spreadsheet;
        for (auto b = d.document_element().first_child(); b; b = b.next_sibling())
            if (Local(b.name()) == "body")
                for (auto s = b.first_child(); s; s = s.next_sibling())
                    if (Local(s.name()) == "spreadsheet")
                        spreadsheet = s;
        for (auto t = spreadsheet.first_child(); t; t = t.next_sibling())
        {
            if (Local(t.name()) != "table")
                continue;
            Sheet sheet;
            sheet.name = Attr(t, "name");
            size_t row = 0, pending = 0;
            ReadRows(t, sheet, row, pending);
            sheet.Trim();
            book.sheets.push_back(std::move(sheet));
        }
        if (book.sheets.empty())
        {
            error = "the spreadsheet has no sheets";
            return false;
        }
        return true;
    }

    bool WriteOds(const Workbook& book, std::string& out)
    {
        const auto names = SafeSheetNames(book);
        std::string tables;
        const size_t count = std::max<size_t>(1, book.sheets.size());
        for (size_t i = 0; i < count; ++i)
        {
            tables += "<table:table table:name=\"" + XmlEscape(i < names.size() ? names[i] : "Sheet1") + "\">";
            const Sheet* sheet = i < book.sheets.size() ? &book.sheets[i] : nullptr;
            const size_t cols = sheet ? std::max<size_t>(1, sheet->Columns()) : 1;
            tables += std::format("<table:table-column table:number-columns-repeated=\"{}\"/>", cols);
            if (!sheet || sheet->rows.empty())
                tables += "<table:table-row><table:table-cell/></table:table-row>";
            else
                for (const auto& r : sheet->rows)
                {
                    tables += "<table:table-row>";
                    if (r.empty())
                        tables += "<table:table-cell/>";
                    for (const auto& c : r)
                    {
                        double number = 0;
                        switch (c.type)
                        {
                        case CellType::Empty:
                            tables += "<table:table-cell/>";
                            break;
                        case CellType::Number:
                            tables += std::format("<table:table-cell office:value-type=\"float\" office:value=\"{0}\"><text:p>{0}</text:p></table:table-cell>",
                                                  FormatNumber(c.number));
                            break;
                        case CellType::Date:
                        {
                            const auto iso = SerialToIso(c.number);
                            auto value = iso;
                            if (value.size() > 10 && value[10] == ' ')
                                value[10] = 'T';
                            if (c.number < 1)
                            {
                                const auto secs = static_cast<long long>(std::llround(c.number * 86400));
                                tables += std::format("<table:table-cell table:style-name=\"ceT\" office:value-type=\"time\" office:time-value=\"PT{:02}H{:02}M{:02}S\"><text:p>{}</text:p></table:table-cell>",
                                                      secs / 3600, secs / 60 % 60, secs % 60, iso);
                            }
                            else
                                tables += std::format("<table:table-cell table:style-name=\"{}\" office:value-type=\"date\" office:date-value=\"{}\"><text:p>{}</text:p></table:table-cell>",
                                                      value.size() > 10 ? "ceDT" : "ceD", value, iso);
                            break;
                        }
                        case CellType::Bool:
                            tables += std::format("<table:table-cell office:value-type=\"boolean\" office:boolean-value=\"{}\"><text:p>{}</text:p></table:table-cell>",
                                                  c.number != 0 ? "true" : "false", c.number != 0 ? "TRUE" : "FALSE");
                            break;
                        case CellType::Error:
                        case CellType::Text:
                            if (c.type == CellType::Text && sheet->untyped && LooksNumeric(c.text, number))
                            {
                                tables += std::format("<table:table-cell office:value-type=\"float\" office:value=\"{0}\"><text:p>{1}</text:p></table:table-cell>",
                                                      FormatNumber(number), XmlEscape(c.text));
                                break;
                            }
                            tables += "<table:table-cell office:value-type=\"string\">";
                            {
                                // One <text:p> per line; runs of spaces as <text:s>.
                                size_t start = 0;
                                const auto& t = c.text;
                                while (start <= t.size())
                                {
                                    const auto nl = t.find('\n', start);
                                    const auto line = t.substr(start, (nl == std::string::npos ? t.size() : nl) - start);
                                    std::string p;
                                    for (size_t k = 0; k < line.size(); ++k)
                                    {
                                        if (line[k] == ' ' && (k == 0 || line[k - 1] == ' ' || k + 1 == line.size()))
                                            p += "<text:s/>";
                                        else if (line[k] == '\t')
                                            p += "<text:tab/>";
                                        else
                                            p += XmlEscape(std::string_view(&line[k], 1));
                                    }
                                    tables += "<text:p>" + p + "</text:p>";
                                    if (nl == std::string::npos)
                                        break;
                                    start = nl + 1;
                                }
                            }
                            tables += "</table:table-cell>";
                            break;
                        }
                    }
                    tables += "</table:table-row>";
                }
            tables += "</table:table>";
        }
        const std::string ns =
            R"(xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" xmlns:number="urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0" xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" office:version="1.2")";
        const std::string content = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content " + ns + R"(>
<office:automatic-styles>
<number:date-style style:name="ND"><number:year number:style="long"/><number:text>-</number:text><number:month number:style="long"/><number:text>-</number:text><number:day number:style="long"/></number:date-style>
<number:date-style style:name="NDT"><number:year number:style="long"/><number:text>-</number:text><number:month number:style="long"/><number:text>-</number:text><number:day number:style="long"/><number:text> </number:text><number:hours number:style="long"/><number:text>:</number:text><number:minutes number:style="long"/><number:text>:</number:text><number:seconds number:style="long"/></number:date-style>
<number:time-style style:name="NT"><number:hours number:style="long"/><number:text>:</number:text><number:minutes number:style="long"/><number:text>:</number:text><number:seconds number:style="long"/></number:time-style>
<style:style style:name="ceD" style:family="table-cell" style:data-style-name="ND"/>
<style:style style:name="ceDT" style:family="table-cell" style:data-style-name="NDT"/>
<style:style style:name="ceT" style:family="table-cell" style:data-style-name="NT"/>
</office:automatic-styles>
<office:body><office:spreadsheet>)" + tables + "</office:spreadsheet></office:body></office:document-content>\n";
        const std::string styles = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles " + ns + "><office:styles/></office:document-styles>\n";
        const std::string meta = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" office:version=\"1.2\"><office:meta><meta:generator>ShiftDrop</meta:generator></office:meta></office:document-meta>\n";
        const char* manifest = R"(<?xml version="1.0" encoding="UTF-8"?>
<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0" manifest:version="1.2">
<manifest:file-entry manifest:full-path="/" manifest:version="1.2" manifest:media-type="application/vnd.oasis.opendocument.spreadsheet"/>
<manifest:file-entry manifest:full-path="content.xml" manifest:media-type="text/xml"/>
<manifest:file-entry manifest:full-path="styles.xml" manifest:media-type="text/xml"/>
<manifest:file-entry manifest:full-path="meta.xml" manifest:media-type="text/xml"/>
</manifest:manifest>
)";
        docs::ZipWriter zip;
        return zip.Add("mimetype", "application/vnd.oasis.opendocument.spreadsheet", true) && zip.Add("META-INF/manifest.xml", manifest) &&
               zip.Add("content.xml", content) && zip.Add("styles.xml", styles) && zip.Add("meta.xml", meta) && zip.Finish(out);
    }
}
