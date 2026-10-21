#include "sheets/Workbook.h"
#include "docs/Bytes.h"
#include "docs/Zip.h"

#include <pugixml.hpp>

#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <set>

// XLSX (SpreadsheetML, ECMA-376) without Excel: a zip of XML parts. Values only, both ways.
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

        pugi::xml_node Child(pugi::xml_node node, std::string_view local)
        {
            for (auto c = node.first_child(); c; c = c.next_sibling())
                if (c.type() == pugi::node_element && Local(c.name()) == local)
                    return c;
            return {};
        }

        std::string Attr(pugi::xml_node node, std::string_view local)
        {
            for (auto a = node.first_attribute(); a; a = a.next_attribute())
                if (Local(a.name()) == local)
                    return a.value();
            return {};
        }

        // Text of an <si> or <is>: its <t>, or the <t> of each rich-text run (<r>), skipping
        // phonetic guides (<rPh>).
        std::string StringItem(pugi::xml_node si)
        {
            std::string out;
            for (auto c = si.first_child(); c; c = c.next_sibling())
            {
                const auto n = Local(c.name());
                if (n == "t")
                    out += c.child_value();
                else if (n == "r")
                    out += Child(c, "t").child_value();
            }
            return out;
        }

        // Is this number format a date or time? Built-in ids 14-22 and 45-47, or a format code with
        // date letters outside quotes, brackets and escapes.
        bool IsDateFormat(int id, const std::string& code)
        {
            if ((id >= 14 && id <= 22) || (id >= 45 && id <= 47) || (id >= 27 && id <= 36) || (id >= 50 && id <= 58))
                return true;
            if (code.empty())
                return false;
            bool quoted = false, bracket = false;
            for (size_t i = 0; i < code.size(); ++i)
            {
                const char c = code[i];
                if (c == '"')
                    quoted = !quoted;
                else if (quoted)
                    continue;
                else if (c == '[')
                    bracket = true;
                else if (c == ']')
                    bracket = false;
                else if (bracket)
                    continue;
                else if (c == '\\' || c == '_' || c == '*')
                    ++i;
                else if (c == 'd' || c == 'D' || c == 'm' || c == 'M' || c == 'y' || c == 'Y' || c == 'h' || c == 'H' || c == 's' || c == 'S')
                    return docs::AsciiLower(code) != "general";
            }
            return false;
        }

        std::string ResolveTarget(const std::string& target)
        {
            if (!target.empty() && target.front() == '/')
                return target.substr(1);
            return "xl/" + target;
        }

        bool LoadXml(const docs::ZipReader& zip, const std::string& part, pugi::xml_document& d)
        {
            const auto xml = zip.Read(part);
            return xml && d.load_buffer(xml->data(), xml->size(), pugi::parse_default | pugi::parse_ws_pcdata);
        }
    }

    bool ReadXlsx(std::string bytes, Workbook& book, std::string& error)
    {
        docs::ZipReader zip;
        if (!zip.Open(std::move(bytes)))
        {
            error = "not an Excel workbook (not a zip package)";
            return false;
        }
        pugi::xml_document wb;
        if (!LoadXml(zip, "xl/workbook.xml", wb))
        {
            error = "not an Excel workbook (no xl/workbook.xml)";
            return false;
        }
        const bool date1904 = [&] {
            const auto v = docs::AsciiLower(Attr(Child(wb.document_element(), "workbookPr"), "date1904"));
            return v == "1" || v == "true";
        }();

        std::map<std::string, std::string> rels;
        pugi::xml_document relDoc;
        if (LoadXml(zip, "xl/_rels/workbook.xml.rels", relDoc))
            for (auto r = relDoc.document_element().first_child(); r; r = r.next_sibling())
                rels[Attr(r, "Id")] = Attr(r, "Target");

        std::vector<std::string> shared;
        pugi::xml_document sst;
        if (LoadXml(zip, "xl/sharedStrings.xml", sst))
            for (auto si = sst.document_element().first_child(); si; si = si.next_sibling())
                if (Local(si.name()) == "si")
                    shared.push_back(StringItem(si));

        std::vector<bool> dateStyle;
        pugi::xml_document styles;
        if (LoadXml(zip, "xl/styles.xml", styles))
        {
            std::map<int, std::string> codes;
            for (auto f = Child(Child(styles.document_element(), "numFmts"), "numFmt"); f; f = f.next_sibling())
                codes[std::atoi(Attr(f, "numFmtId").c_str())] = Attr(f, "formatCode");
            for (auto xf = Child(Child(styles.document_element(), "cellXfs"), "xf"); xf; xf = xf.next_sibling())
            {
                if (Local(xf.name()) != "xf")
                    continue;
                const int id = std::atoi(Attr(xf, "numFmtId").c_str());
                dateStyle.push_back(IsDateFormat(id, codes.count(id) ? codes[id] : std::string()));
            }
        }

        for (auto s = Child(Child(wb.document_element(), "sheets"), "sheet"); s; s = s.next_sibling())
        {
            if (Local(s.name()) != "sheet")
                continue;
            Sheet sheet;
            sheet.name = Attr(s, "name");
            const auto rel = rels.find(Attr(s, "id"));
            if (rel == rels.end())
                continue;
            pugi::xml_document ws;
            if (!LoadXml(zip, ResolveTarget(rel->second), ws))
                continue;   // a chart sheet or a missing part
            size_t rowIndex = 0;
            for (auto row = Child(Child(ws.document_element(), "sheetData"), "row"); row; row = row.next_sibling())
            {
                if (Local(row.name()) != "row")
                    continue;
                if (const auto r = Attr(row, "r"); !r.empty())
                    rowIndex = static_cast<size_t>(std::max(1, std::atoi(r.c_str()))) - 1;
                if (rowIndex >= 1048576)
                    break;
                size_t colIndex = 0;
                for (auto c = row.first_child(); c; c = c.next_sibling())
                {
                    if (Local(c.name()) != "c")
                        continue;
                    size_t r = rowIndex, col = colIndex;
                    if (const auto ref = Attr(c, "r"); !ref.empty())
                        ParseCellRef(ref, r, col);
                    colIndex = col + 1;
                    if (col >= 16384)
                        continue;
                    const auto t = Attr(c, "t");
                    const std::string v = Child(c, "v").child_value();
                    Cell cell;
                    if (t == "s")
                    {
                        const auto i = static_cast<size_t>(std::atoll(v.c_str()));
                        if (!v.empty() && i < shared.size())
                            cell = Cell::Str(shared[i]);
                    }
                    else if (t == "inlineStr")
                        cell = Cell::Str(StringItem(Child(c, "is")));
                    else if (t == "str")
                        cell = Cell::Str(v);
                    else if (t == "b")
                        cell = Cell{ CellType::Bool, v == "1" ? 1.0 : 0.0, {} };
                    else if (t == "e")
                        cell = Cell{ CellType::Error, 0, v };
                    else if (t == "d")
                    {
                        double serial = 0;
                        if (IsoToSerial(v, serial))
                            cell = Cell{ CellType::Date, serial, {} };
                        else if (!v.empty())
                            cell = Cell::Str(v);
                    }
                    else if (!v.empty())
                    {
                        cell = Cell::Num(std::strtod(v.c_str(), nullptr));
                        const auto style = static_cast<size_t>(std::atoi(Attr(c, "s").c_str()));
                        if (style < dateStyle.size() && dateStyle[style])
                        {
                            cell.type = CellType::Date;
                            if (date1904)
                                cell.number += 1462;
                        }
                    }
                    sheet.Set(r, col, std::move(cell));
                }
                ++rowIndex;
            }
            sheet.Trim();
            book.sheets.push_back(std::move(sheet));
        }
        if (book.sheets.empty())
        {
            error = "the workbook has no worksheets";
            return false;
        }
        return true;
    }

    bool WriteXlsx(const Workbook& book, std::string& out)
    {
        const auto names = SafeSheetNames(book);
        std::vector<std::string> shared;
        std::map<std::string, size_t> sharedIndex;
        const auto stringId = [&](const std::string& s) {
            const auto [it, added] = sharedIndex.emplace(s, shared.size());
            if (added)
                shared.push_back(s);
            return it->second;
        };

        docs::ZipWriter zip;
        std::string sheetsXml, relsXml, typesXml;
        bool ok = true;
        const size_t count = std::max<size_t>(1, book.sheets.size());
        for (size_t i = 0; i < count; ++i)
        {
            std::string x = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>)";
            if (i < book.sheets.size())
            {
                const auto& sheet = book.sheets[i];
                for (size_t r = 0; r < sheet.rows.size(); ++r)
                {
                    if (sheet.rows[r].empty())
                        continue;
                    x += std::format("<row r=\"{}\">", r + 1);
                    for (size_t c = 0; c < sheet.rows[r].size(); ++c)
                    {
                        const auto& cell = sheet.rows[r][c];
                        const auto ref = ColumnName(c) + std::to_string(r + 1);
                        double number = 0;
                        switch (cell.type)
                        {
                        case CellType::Empty:
                            break;
                        case CellType::Number:
                            x += std::format("<c r=\"{}\"><v>{}</v></c>", ref, FormatNumber(cell.number));
                            break;
                        case CellType::Date:
                            // Style 1 shows a date, style 2 a date and time.
                            x += std::format("<c r=\"{}\" s=\"{}\"><v>{}</v></c>", ref, cell.number == std::floor(cell.number) ? 1 : 2, FormatNumber(cell.number));
                            break;
                        case CellType::Bool:
                            x += std::format("<c r=\"{}\" t=\"b\"><v>{}</v></c>", ref, cell.number != 0 ? 1 : 0);
                            break;
                        case CellType::Error:
                            x += std::format("<c r=\"{}\" t=\"e\"><v>{}</v></c>", ref, XmlEscape(cell.text));
                            break;
                        case CellType::Text:
                            if (sheet.untyped && LooksNumeric(cell.text, number))
                                x += std::format("<c r=\"{}\"><v>{}</v></c>", ref, FormatNumber(number));
                            else
                                x += std::format("<c r=\"{}\" t=\"s\"><v>{}</v></c>", ref, stringId(cell.text));
                            break;
                        }
                    }
                    x += "</row>";
                }
            }
            x += "</sheetData></worksheet>";
            ok = ok && zip.Add(std::format("xl/worksheets/sheet{}.xml", i + 1), x);
            sheetsXml += std::format("<sheet name=\"{}\" sheetId=\"{}\" r:id=\"rId{}\"/>", XmlEscape(i < names.size() ? names[i] : "Sheet1"), i + 1, i + 1);
            relsXml += std::format("<Relationship Id=\"rId{}\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet{}.xml\"/>\n", i + 1, i + 1);
            typesXml += std::format("<Override PartName=\"/xl/worksheets/sheet{}.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>\n", i + 1);
        }
        relsXml += std::format("<Relationship Id=\"rId{}\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>\n", count + 1);
        relsXml += std::format("<Relationship Id=\"rId{}\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>\n", count + 2);

        std::string sst = std::format(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="{0}" uniqueCount="{0}">)",
                                      shared.size());
        for (const auto& s : shared)
        {
            // Leading or trailing spaces need xml:space="preserve" to survive.
            const bool keep = !s.empty() && (s.front() == ' ' || s.back() == ' ' || s.find('\n') != std::string::npos);
            sst += keep ? "<si><t xml:space=\"preserve\">" + XmlEscape(s) + "</t></si>" : "<si><t>" + XmlEscape(s) + "</t></si>";
        }
        sst += "</sst>";

        const std::string workbook = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets>)" +
                                     sheetsXml + "</sheets></workbook>";
        const std::string workbookRels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n" +
                                         relsXml + "</Relationships>";
        const char* stylesXml = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
<numFmts count="1"><numFmt numFmtId="164" formatCode="yyyy\-mm\-dd\ hh:mm:ss"/></numFmts>
<fonts count="1"><font><sz val="11"/><name val="Calibri"/><family val="2"/></font></fonts>
<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>
<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>
<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>
<cellXfs count="3"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/><xf numFmtId="14" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/><xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/></cellXfs>
<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>
</styleSheet>)";
        const std::string types = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>
<Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>
)" + typesXml + "</Types>";
        const char* packageRels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>)";
        ok = ok && zip.Add("[Content_Types].xml", types) && zip.Add("_rels/.rels", packageRels) && zip.Add("xl/workbook.xml", workbook) &&
             zip.Add("xl/_rels/workbook.xml.rels", workbookRels) && zip.Add("xl/styles.xml", stylesXml) && zip.Add("xl/sharedStrings.xml", sst);
        return ok && zip.Finish(out);
    }
}
