#include "docs/Bytes.h"
#include "docs/Formats.h"
#include "docs/Paper.h"
#include "docs/Zip.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>

// DOCX (WordprocessingML, ECMA-376) without Word: the package is a zip of XML parts. The reader
// walks word/document.xml with the styles, numbering and relationship parts it needs; the writer
// emits the smallest package Word opens without a repair prompt.
namespace sd::docs
{
    namespace
    {
        // Element and attribute names are matched by their local part, so any namespace prefix works
        // (Strict OOXML and some generators use prefixes other than "w:").
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

        const char* Attr(pugi::xml_node node, std::string_view local)
        {
            for (auto a = node.first_attribute(); a; a = a.next_attribute())
                if (Local(a.name()) == local)
                    return a.value();
            return nullptr;
        }

        std::string AttrStr(pugi::xml_node node, std::string_view local)
        {
            const char* v = Attr(node, local);
            return v ? std::string(v) : std::string();
        }

        // <w:b/>, <w:b w:val="true"/> are on; w:val="0", "false", "off", "none" are off.
        bool OnOff(pugi::xml_node node)
        {
            if (!node)
                return false;
            const char* v = Attr(node, "val");
            if (!v)
                return true;
            const std::string s = AsciiLower(v);
            return !(s == "0" || s == "false" || s == "off" || s == "none");
        }

        // The prefix of a part's folder: "word/document.xml" -> "word/".
        std::string FolderOf(const std::string& part)
        {
            const auto slash = part.rfind('/');
            return slash == std::string::npos ? std::string() : part.substr(0, slash + 1);
        }

        // Resolves a relationship target against the source part's folder ("media/a.png", "../media/a.png", "/word/media/a.png").
        std::string ResolvePart(const std::string& folder, const std::string& target)
        {
            if (!target.empty() && target.front() == '/')
                return target.substr(1);
            std::string base = folder;
            std::string t = target;
            while (t.starts_with("../"))
            {
                t.erase(0, 3);
                if (!base.empty())
                {
                    base.pop_back();
                    const auto slash = base.rfind('/');
                    base = slash == std::string::npos ? std::string() : base.substr(0, slash + 1);
                }
            }
            if (t.starts_with("./"))
                t.erase(0, 2);
            return base + t;
        }

        struct Rel
        {
            std::string type;
            std::string target;
            bool external = false;
        };

        std::map<std::string, Rel> ReadRels(const ZipReader& zip, const std::string& part)
        {
            std::map<std::string, Rel> rels;
            const auto folder = FolderOf(part);
            const auto name = folder + "_rels/" + part.substr(folder.size()) + ".rels";
            const auto xml = zip.Read(name);
            if (!xml)
                return rels;
            pugi::xml_document d;
            if (!d.load_buffer(xml->data(), xml->size()))
                return rels;
            for (auto r = d.first_child().first_child(); r; r = r.next_sibling())
            {
                if (Local(r.name()) != "Relationship")
                    continue;
                Rel rel;
                rel.type = AttrStr(r, "Type");
                rel.target = AttrStr(r, "Target");
                rel.external = AsciiLower(AttrStr(r, "TargetMode")) == "external";
                rels[AttrStr(r, "Id")] = std::move(rel);
            }
            return rels;
        }

        struct StyleInfo
        {
            std::string name;      // lower case
            std::string basedOn;
            int outline = -1;      // 0-based outline level, -1 none
            bool bold = false, italic = false;
            bool mono = false;
            std::string numId;     // list numbering carried by the style
            int numLevel = 0;
        };

        struct NumLevel
        {
            bool ordered = false;
            int start = 1;
        };

        struct Numbering
        {
            std::map<std::string, std::map<int, NumLevel>> abstracts;   // abstractNumId -> levels
            std::map<std::string, std::string> numToAbstract;
            std::map<std::string, std::map<int, int>> startOverride;     // numId -> ilvl -> start

            NumLevel Level(const std::string& numId, int ilvl) const
            {
                NumLevel l;
                const auto a = numToAbstract.find(numId);
                if (a != numToAbstract.end())
                    if (const auto levels = abstracts.find(a->second); levels != abstracts.end())
                        if (const auto it = levels->second.find(ilvl); it != levels->second.end())
                            l = it->second;
                if (const auto o = startOverride.find(numId); o != startOverride.end())
                    if (const auto it = o->second.find(ilvl); it != o->second.end())
                        l.start = it->second;
                return l;
            }
        };

        class DocxReader
        {
        public:
            DocxReader(const ZipReader& zip, Document& doc) : m_zip(zip), m_doc(doc) {}

            bool Read(std::string& error)
            {
                // The main part, from the package relationships (almost always word/document.xml).
                std::string main = "word/document.xml";
                for (const auto& [id, rel] : ReadRels(m_zip, "_rels/.rels"))
                    if (rel.type.ends_with("/officeDocument"))
                        main = ResolvePart("", rel.target);
                const auto xml = m_zip.Read(main);
                if (!xml)
                {
                    error = "not a Word document (no " + main + ")";
                    return false;
                }
                m_folder = FolderOf(main);
                m_rels = ReadRels(m_zip, main);
                for (const auto& [id, rel] : m_rels)
                {
                    if (rel.type.ends_with("/styles"))
                        LoadStyles(ResolvePart(m_folder, rel.target));
                    else if (rel.type.ends_with("/numbering"))
                        LoadNumbering(ResolvePart(m_folder, rel.target));
                }
                LoadTitle();
                pugi::xml_document d;
                if (!d.load_buffer(xml->data(), xml->size(), pugi::parse_default | pugi::parse_ws_pcdata))
                {
                    error = "the document's XML is damaged";
                    return false;
                }
                const auto body = Child(d.document_element(), "body");
                if (!body)
                {
                    error = "the document has no body";
                    return false;
                }
                ReadContainer(body, nullptr);
                for (auto& b : m_doc.blocks)
                {
                    NormalizeSpans(b.spans);
                    for (auto& row : b.rows)
                        for (auto& cell : row)
                            NormalizeSpans(cell.spans);
                }
                return true;
            }

        private:
            void LoadTitle()
            {
                const auto core = m_zip.Read("docProps/core.xml");
                if (!core)
                    return;
                pugi::xml_document d;
                if (d.load_buffer(core->data(), core->size()))
                    if (const auto t = Child(d.document_element(), "title"))
                        m_doc.title = std::string(Trim(t.child_value()));
            }

            void LoadStyles(const std::string& part)
            {
                const auto xml = m_zip.Read(part);
                pugi::xml_document d;
                if (!xml || !d.load_buffer(xml->data(), xml->size()))
                    return;
                for (auto s = d.document_element().first_child(); s; s = s.next_sibling())
                {
                    if (Local(s.name()) != "style")
                        continue;
                    StyleInfo info;
                    info.name = AsciiLower(AttrStr(Child(s, "name"), "val"));
                    info.basedOn = AttrStr(Child(s, "basedOn"), "val");
                    const auto ppr = Child(s, "pPr");
                    if (const auto o = Child(ppr, "outlineLvl"))
                        info.outline = std::atoi(AttrStr(o, "val").c_str());
                    if (const auto num = Child(ppr, "numPr"))
                    {
                        info.numId = AttrStr(Child(num, "numId"), "val");
                        info.numLevel = std::atoi(AttrStr(Child(num, "ilvl"), "val").c_str());
                    }
                    const auto rpr = Child(s, "rPr");
                    info.bold = OnOff(Child(rpr, "b"));
                    info.italic = OnOff(Child(rpr, "i"));
                    info.mono = IsMonoFontName(AttrStr(Child(rpr, "rFonts"), "ascii"));
                    m_styles[AttrStr(s, "styleId")] = std::move(info);
                }
            }

            void LoadNumbering(const std::string& part)
            {
                const auto xml = m_zip.Read(part);
                pugi::xml_document d;
                if (!xml || !d.load_buffer(xml->data(), xml->size()))
                    return;
                for (auto n = d.document_element().first_child(); n; n = n.next_sibling())
                {
                    const auto local = Local(n.name());
                    if (local == "abstractNum")
                    {
                        auto& levels = m_numbering.abstracts[AttrStr(n, "abstractNumId")];
                        for (auto l = n.first_child(); l; l = l.next_sibling())
                        {
                            if (Local(l.name()) != "lvl")
                                continue;
                            NumLevel level;
                            const auto fmt = AttrStr(Child(l, "numFmt"), "val");
                            level.ordered = !fmt.empty() && fmt != "bullet" && fmt != "none";
                            if (const auto st = Child(l, "start"))
                                level.start = std::atoi(AttrStr(st, "val").c_str());
                            levels[std::atoi(AttrStr(l, "ilvl").c_str())] = level;
                        }
                    }
                    else if (local == "num")
                    {
                        const auto id = AttrStr(n, "numId");
                        m_numbering.numToAbstract[id] = AttrStr(Child(n, "abstractNumId"), "val");
                        for (auto o = n.first_child(); o; o = o.next_sibling())
                            if (Local(o.name()) == "lvlOverride")
                                if (const auto st = Child(o, "startOverride"))
                                    m_numbering.startOverride[id][std::atoi(AttrStr(o, "ilvl").c_str())] = std::atoi(AttrStr(st, "val").c_str());
                    }
                }
            }

            // Follows basedOn to find a style property, at most 16 steps (styles can loop).
            template <class F>
            auto FromStyle(const std::string& id, F get) const -> decltype(get(StyleInfo{}))
            {
                std::string cur = id;
                for (int i = 0; i < 16 && !cur.empty(); ++i)
                {
                    const auto it = m_styles.find(cur);
                    if (it == m_styles.end())
                        break;
                    auto v = get(it->second);
                    if constexpr (std::is_same_v<decltype(v), std::string>)
                    {
                        if (!v.empty())
                            return v;
                    }
                    else if (v)
                        return v;
                    cur = it->second.basedOn;
                }
                return {};
            }

            int HeadingLevel(const std::string& styleId, pugi::xml_node ppr) const
            {
                if (const auto o = Child(ppr, "outlineLvl"))
                {
                    const int v = std::atoi(AttrStr(o, "val").c_str());
                    if (v >= 0 && v < 9)
                        return std::min(v + 1, 6);
                }
                const int fromStyle = FromStyle(styleId, [](const StyleInfo& s) -> int {
                    if (s.outline >= 0 && s.outline < 9)
                        return std::min(s.outline + 1, 6);
                    if (s.name.starts_with("heading ") && s.name.size() == 9 && s.name[8] >= '1' && s.name[8] <= '9')
                        return std::min(s.name[8] - '0', 6);
                    if (s.name == "title")
                        return 1;
                    return 0;
                });
                if (fromStyle)
                    return fromStyle;
                // Style ids without a styles part ("Heading2").
                const auto lower = AsciiLower(styleId);
                if (lower.starts_with("heading") && lower.size() == 8 && lower[7] >= '1' && lower[7] <= '9')
                    return std::min(lower[7] - '0', 6);
                return 0;
            }

            bool StyleNameHas(const std::string& styleId, std::string_view needle) const
            {
                return FromStyle(styleId, [&](const StyleInfo& s) { return s.name.find(needle) != std::string::npos; }) ||
                       AsciiLower(styleId).find(needle) != std::string::npos;
            }

            struct Run
            {
                bool bold = false, italic = false, strike = false, code = false;
                std::string link;
            };

            struct Field
            {
                std::string instr;
                bool result = false;
                std::string link;
            };

            void AddText(std::vector<Span>& out, const Run& run, std::string text)
            {
                Span s;
                s.text = std::move(text);
                s.bold = run.bold;
                s.italic = run.italic;
                s.strike = run.strike;
                s.code = run.code;
                s.link = run.link;
                for (const auto& f : m_fields)
                    if (f.result && !f.link.empty())
                        s.link = f.link;
                out.push_back(std::move(s));
            }

            void AddImage(std::vector<Span>& out, const std::string& rid, pugi::xml_node container, const std::string& alt)
            {
                int index = -1;
                if (const auto cached = m_imageByRel.find(rid); cached != m_imageByRel.end())
                    index = cached->second;
                else
                {
                    const auto rel = m_rels.find(rid);
                    if (rel == m_rels.end() || rel->second.external)
                        return;
                    auto bytes = m_zip.Read(ResolvePart(m_folder, rel->second.target));
                    if (!bytes || SniffImage(*bytes).empty())
                        return;
                    index = docs::AddImage(m_doc, std::move(*bytes));
                    m_imageByRel[rid] = index;
                }
                // The displayed size (EMU, 914400 per inch) wins over the picture's own pixels.
                if (const auto extent = FindDeep(container, "extent"))
                {
                    const double cx = std::atof(AttrStr(extent, "cx").c_str()), cy = std::atof(AttrStr(extent, "cy").c_str());
                    if (cx > 0 && cy > 0)
                    {
                        m_doc.images[static_cast<size_t>(index)].width = static_cast<int>(cx / 9525 + 0.5);
                        m_doc.images[static_cast<size_t>(index)].height = static_cast<int>(cy / 9525 + 0.5);
                    }
                }
                Span s;
                s.image = index;
                s.text = alt;
                out.push_back(std::move(s));
            }

            static pugi::xml_node FindDeep(pugi::xml_node node, std::string_view local)
            {
                for (auto c = node.first_child(); c; c = c.next_sibling())
                {
                    if (c.type() != pugi::node_element)
                        continue;
                    if (Local(c.name()) == local)
                        return c;
                    if (auto f = FindDeep(c, local))
                        return f;
                }
                return {};
            }

            void ReadRun(pugi::xml_node r, Run run, std::vector<Span>& out)
            {
                const auto rpr = Child(r, "rPr");
                if (rpr)
                {
                    if (OnOff(Child(rpr, "vanish")))
                        return;   // hidden text
                    const auto rstyle = AttrStr(Child(rpr, "rStyle"), "val");
                    if (!rstyle.empty())
                    {
                        run.bold |= FromStyle(rstyle, [](const StyleInfo& s) { return s.bold; }) || StyleNameHas(rstyle, "strong");
                        run.italic |= FromStyle(rstyle, [](const StyleInfo& s) { return s.italic; }) || StyleNameHas(rstyle, "emphasis");
                        run.code |= FromStyle(rstyle, [](const StyleInfo& s) { return s.mono; }) || StyleNameHas(rstyle, "code");
                    }
                    if (const auto b = Child(rpr, "b"))
                        run.bold = OnOff(b);
                    if (const auto i = Child(rpr, "i"))
                        run.italic = OnOff(i);
                    run.strike = OnOff(Child(rpr, "strike")) || OnOff(Child(rpr, "dstrike"));
                    if (const auto fonts = Child(rpr, "rFonts"); fonts && IsMonoFontName(AttrStr(fonts, "ascii")))
                        run.code = true;
                }
                for (auto c = r.first_child(); c; c = c.next_sibling())
                {
                    if (c.type() != pugi::node_element)
                        continue;
                    const auto n = Local(c.name());
                    if (n == "fldChar")
                    {
                        const auto type = AttrStr(c, "fldCharType");
                        if (type == "begin")
                            m_fields.push_back({});
                        else if (type == "separate" && !m_fields.empty())
                        {
                            auto& f = m_fields.back();
                            f.result = true;
                            // HYPERLINK "https://..." [switches]
                            const auto instr = std::string(Trim(f.instr));
                            if (instr.starts_with("HYPERLINK"))
                            {
                                const auto q1 = instr.find('"');
                                const auto q2 = q1 == std::string::npos ? q1 : instr.find('"', q1 + 1);
                                if (q2 != std::string::npos)
                                    f.link = instr.substr(q1 + 1, q2 - q1 - 1);
                                if (instr.find("\\l") != std::string::npos && !f.link.empty())
                                    f.link = "#" + f.link;
                            }
                        }
                        else if (type == "end" && !m_fields.empty())
                            m_fields.pop_back();
                        continue;
                    }
                    if (n == "instrText")
                    {
                        if (!m_fields.empty())
                            m_fields.back().instr += c.child_value();
                        continue;
                    }
                    // Inside a field's instructions nothing is shown.
                    if (!m_fields.empty() && !m_fields.back().result)
                        continue;
                    if (n == "t")
                        AddText(out, run, c.child_value());
                    else if (n == "tab" || n == "ptab")
                        AddText(out, run, "\t");
                    else if (n == "br" || n == "cr")
                    {
                        const auto type = AttrStr(c, "type");
                        if (type != "page" && type != "column")
                        {
                            Span s;
                            s.lineBreak = true;
                            out.push_back(s);
                        }
                    }
                    else if (n == "noBreakHyphen")
                        AddText(out, run, "-");
                    else if (n == "drawing")
                    {
                        if (const auto blip = FindDeep(c, "blip"))
                        {
                            const auto docPr = FindDeep(c, "docPr");
                            AddImage(out, AttrStr(blip, "embed"), c, docPr ? AttrStr(docPr, "descr") : std::string());
                        }
                    }
                    else if (n == "pict" || n == "object")
                    {
                        if (const auto data = FindDeep(c, "imagedata"))
                            AddImage(out, AttrStr(data, "id"), c, AttrStr(data, "title"));
                    }
                    else if (n == "sym")
                    {
                        const auto code = AttrStr(c, "char");
                        if (!code.empty())
                        {
                            std::string t;
                            auto cp = static_cast<char32_t>(std::strtoul(code.c_str(), nullptr, 16));
                            if (cp >= 0xF000 && cp <= 0xF0FF)
                                cp -= 0xF000;   // symbol-font private range
                            if (cp >= 0x20)
                            {
                                AppendUtf8(t, cp);
                                AddText(out, run, t);
                            }
                        }
                    }
                }
            }

            // Runs and the inline containers that hold them (hyperlinks, insertions, smart tags...).
            void ReadInline(pugi::xml_node parent, const Run& run, std::vector<Span>& out)
            {
                for (auto c = parent.first_child(); c; c = c.next_sibling())
                {
                    if (c.type() != pugi::node_element)
                        continue;
                    const auto n = Local(c.name());
                    if (n == "r")
                        ReadRun(c, run, out);
                    else if (n == "hyperlink")
                    {
                        Run linked = run;
                        const auto rid = AttrStr(c, "id");
                        if (const auto rel = m_rels.find(rid); !rid.empty() && rel != m_rels.end())
                            linked.link = rel->second.target;
                        else if (const auto anchor = AttrStr(c, "anchor"); !anchor.empty())
                            linked.link = "#" + anchor;
                        ReadInline(c, linked, out);
                    }
                    else if (n == "fldSimple")
                    {
                        Run linked = run;
                        const auto instr = std::string(Trim(AttrStr(c, "instr")));
                        if (instr.starts_with("HYPERLINK"))
                        {
                            const auto q1 = instr.find('"');
                            const auto q2 = q1 == std::string::npos ? q1 : instr.find('"', q1 + 1);
                            if (q2 != std::string::npos)
                                linked.link = instr.substr(q1 + 1, q2 - q1 - 1);
                        }
                        ReadInline(c, linked, out);
                    }
                    else if (n == "ins" || n == "smartTag" || n == "customXml" || n == "moveTo" || n == "dir" || n == "bdo")
                        ReadInline(c, run, out);
                    else if (n == "sdt")
                        ReadInline(Child(c, "sdtContent"), run, out);
                    // del, moveFrom, bookmarks, comments, proofing marks: not text
                }
            }

            void ReadParagraph(pugi::xml_node p, std::vector<Span>* cell)
            {
                const auto ppr = Child(p, "pPr");
                const auto styleId = AttrStr(Child(ppr, "pStyle"), "val");
                std::vector<Span> spans;
                Run run;
                // Paragraph styles can make every run bold (rare) or monospace (code styles).
                run.code = FromStyle(styleId, [](const StyleInfo& s) { return s.mono; });
                ReadInline(p, run, spans);
                if (cell)
                {
                    if (!cell->empty())
                    {
                        Span br;
                        br.lineBreak = true;
                        cell->push_back(br);
                    }
                    cell->insert(cell->end(), spans.begin(), spans.end());
                    return;
                }

                Block b;
                const int heading = HeadingLevel(styleId, ppr);
                std::string numId;
                int ilvl = 0;
                if (const auto num = Child(ppr, "numPr"))
                {
                    numId = AttrStr(Child(num, "numId"), "val");
                    ilvl = std::atoi(AttrStr(Child(num, "ilvl"), "val").c_str());
                }
                else if (!styleId.empty())
                {
                    numId = FromStyle(styleId, [](const StyleInfo& s) { return s.numId; });
                    ilvl = FromStyle(styleId, [](const StyleInfo& s) { return s.numId.empty() ? 0 : s.numLevel; });
                }
                const bool codeStyle = FromStyle(styleId, [](const StyleInfo& s) { return s.mono; }) || StyleNameHas(styleId, "code") ||
                                       StyleNameHas(styleId, "preformatted") || StyleNameHas(styleId, "source");
                if (heading > 0)
                {
                    b.kind = BlockKind::Heading;
                    b.level = heading;
                    for (auto& s : spans)
                        s.bold = false;   // headings are bold by style; bold runs inside say nothing more
                }
                else if (!numId.empty() && numId != "0")
                {
                    b.kind = BlockKind::ListItem;
                    b.level = std::clamp(ilvl, 0, 8);
                    const auto level = m_numbering.Level(numId, ilvl);
                    b.ordered = level.ordered;
                    // Word counts per abstract list: deeper levels restart after a shallower item.
                    auto& counters = m_counters[numId];
                    for (auto it = counters.begin(); it != counters.end();)
                        it = it->first > ilvl ? counters.erase(it) : std::next(it);
                    const auto found = counters.find(ilvl);
                    b.start = found == counters.end() ? level.start : found->second + 1;
                    counters[ilvl] = b.start;
                }
                else if (StyleNameHas(styleId, "list bullet") || StyleNameHas(styleId, "list number"))
                {
                    b.kind = BlockKind::ListItem;
                    b.ordered = StyleNameHas(styleId, "list number");
                    b.start = ++m_styleListCounter;
                }
                else if (codeStyle)
                {
                    b.kind = BlockKind::Code;
                    for (auto& s : spans)
                        s.code = false;
                }
                else if (StyleNameHas(styleId, "quote"))
                    b.kind = BlockKind::Quote;
                if (b.kind != BlockKind::ListItem)
                    m_styleListCounter = 0;

                // Consecutive code paragraphs are one code block, joined by line breaks.
                if (b.kind == BlockKind::Code && !m_doc.blocks.empty() && m_doc.blocks.back().kind == BlockKind::Code && m_lastWasCode)
                {
                    auto& prev = m_doc.blocks.back().spans;
                    Span s;
                    s.text = "\n" + PlainText(spans);
                    prev.push_back(std::move(s));
                    return;
                }
                if (b.kind == BlockKind::Code)
                {
                    Span s;
                    s.text = PlainText(spans);
                    spans = { s };
                }
                m_lastWasCode = b.kind == BlockKind::Code;
                b.spans = std::move(spans);
                // Empty paragraphs are spacing, not content.
                if (b.kind != BlockKind::ListItem && b.kind != BlockKind::Code && b.spans.empty())
                    return;
                m_doc.blocks.push_back(std::move(b));
            }

            void ReadTable(pugi::xml_node tbl, std::vector<Span>* cell)
            {
                if (cell)
                {
                    // A table inside a table cell: its text joins the outer cell.
                    for (auto tr = tbl.first_child(); tr; tr = tr.next_sibling())
                        if (Local(tr.name()) == "tr")
                            for (auto tc = tr.first_child(); tc; tc = tc.next_sibling())
                                if (Local(tc.name()) == "tc")
                                    ReadContainer(tc, cell);
                    return;
                }
                Block b;
                b.kind = BlockKind::Table;
                bool firstRowHeader = false;
                for (auto tr = tbl.first_child(); tr; tr = tr.next_sibling())
                {
                    if (Local(tr.name()) != "tr")
                        continue;
                    if (b.rows.empty() && Child(Child(tr, "trPr"), "tblHeader"))
                        firstRowHeader = true;
                    std::vector<TableCell> row;
                    for (auto tc = tr.first_child(); tc; tc = tc.next_sibling())
                    {
                        if (Local(tc.name()) == "sdt")
                            tc = Child(Child(tc, "sdtContent"), "tc");
                        if (!tc || Local(tc.name()) != "tc")
                            continue;
                        TableCell c;
                        ReadContainer(tc, &c.spans);
                        row.push_back(std::move(c));
                        // A merged cell spanning columns is followed by empty cells to keep the grid.
                        const int span = std::atoi(AttrStr(Child(Child(tc, "tcPr"), "gridSpan"), "val").c_str());
                        for (int k = 1; k < span && k < 64; ++k)
                            row.emplace_back();
                    }
                    b.rows.push_back(std::move(row));
                }
                if (b.rows.empty())
                    return;
                if (!firstRowHeader)
                {
                    // A first row that is all bold reads as a header.
                    bool allBold = !b.rows[0].empty();
                    for (const auto& c : b.rows[0])
                        for (const auto& s : c.spans)
                            if (!s.lineBreak && s.image < 0 && !Trim(s.text).empty() && !s.bold)
                                allBold = false;
                    firstRowHeader = allBold && b.rows.size() > 1;
                }
                b.headerRow = firstRowHeader;
                if (b.headerRow)
                    for (auto& c : b.rows[0])
                        for (auto& s : c.spans)
                            s.bold = false;
                m_lastWasCode = false;
                m_doc.blocks.push_back(std::move(b));
            }

            void ReadContainer(pugi::xml_node node, std::vector<Span>* cell)
            {
                for (auto c = node.first_child(); c; c = c.next_sibling())
                {
                    if (c.type() != pugi::node_element)
                        continue;
                    const auto n = Local(c.name());
                    if (n == "p")
                        ReadParagraph(c, cell);
                    else if (n == "tbl")
                        ReadTable(c, cell);
                    else if (n == "sdt")
                        ReadContainer(Child(c, "sdtContent"), cell);
                    else if (n == "customXml" || n == "ins" || n == "moveTo")
                        ReadContainer(c, cell);
                }
            }

            const ZipReader& m_zip;
            Document& m_doc;
            std::string m_folder;
            std::map<std::string, Rel> m_rels;
            std::map<std::string, StyleInfo> m_styles;
            Numbering m_numbering;
            std::map<std::string, std::map<int, int>> m_counters;
            std::map<std::string, int> m_imageByRel;
            std::vector<Field> m_fields;
            int m_styleListCounter = 0;
            bool m_lastWasCode = false;
        };

        // ---- Writer ---------------------------------------------------------------------------------

        const char* kStyles = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii="Calibri" w:hAnsi="Calibri" w:eastAsia="Calibri" w:cs="Calibri"/><w:sz w:val="22"/><w:szCs w:val="22"/><w:lang w:val="en-US"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after="160" w:line="264" w:lineRule="auto"/></w:pPr></w:pPrDefault></w:docDefaults>
<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/><w:qFormat/></w:style>
<w:style w:type="paragraph" w:styleId="Title"><w:name w:val="Title"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/><w:qFormat/><w:pPr><w:spacing w:after="240"/><w:outlineLvl w:val="0"/></w:pPr><w:rPr><w:sz w:val="52"/><w:szCs w:val="52"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/><w:qFormat/><w:pPr><w:keepNext/><w:spacing w:before="360" w:after="120"/><w:outlineLvl w:val="0"/></w:pPr><w:rPr><w:b/><w:color w:val="1F3864"/><w:sz w:val="36"/><w:szCs w:val="36"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Heading2"><w:name w:val="heading 2"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/><w:qFormat/><w:pPr><w:keepNext/><w:spacing w:before="280" w:after="100"/><w:outlineLvl w:val="1"/></w:pPr><w:rPr><w:b/><w:color w:val="1F3864"/><w:sz w:val="30"/><w:szCs w:val="30"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Heading3"><w:name w:val="heading 3"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/><w:qFormat/><w:pPr><w:keepNext/><w:spacing w:before="240" w:after="80"/><w:outlineLvl w:val="2"/></w:pPr><w:rPr><w:b/><w:color w:val="1F3864"/><w:sz w:val="26"/><w:szCs w:val="26"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Heading4"><w:name w:val="heading 4"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/><w:qFormat/><w:pPr><w:keepNext/><w:spacing w:before="200" w:after="60"/><w:outlineLvl w:val="3"/></w:pPr><w:rPr><w:b/><w:i/><w:sz w:val="24"/><w:szCs w:val="24"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Heading5"><w:name w:val="heading 5"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/><w:qFormat/><w:pPr><w:keepNext/><w:spacing w:before="200" w:after="60"/><w:outlineLvl w:val="4"/></w:pPr><w:rPr><w:b/><w:sz w:val="22"/><w:szCs w:val="22"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Heading6"><w:name w:val="heading 6"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/><w:qFormat/><w:pPr><w:keepNext/><w:spacing w:before="200" w:after="60"/><w:outlineLvl w:val="5"/></w:pPr><w:rPr><w:i/><w:sz w:val="22"/><w:szCs w:val="22"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="ListParagraph"><w:name w:val="List Paragraph"/><w:basedOn w:val="Normal"/><w:qFormat/><w:pPr><w:spacing w:after="60"/><w:ind w:left="720"/><w:contextualSpacing/></w:pPr></w:style>
<w:style w:type="paragraph" w:styleId="Quote"><w:name w:val="Quote"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/><w:qFormat/><w:pPr><w:ind w:left="720" w:right="720"/></w:pPr><w:rPr><w:i/><w:color w:val="404040"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="SourceCode"><w:name w:val="Source Code"/><w:basedOn w:val="Normal"/><w:qFormat/><w:pPr><w:shd w:val="clear" w:color="auto" w:fill="F2F2F2"/><w:spacing w:after="0" w:line="240" w:lineRule="auto"/></w:pPr><w:rPr><w:rFonts w:ascii="Consolas" w:hAnsi="Consolas" w:cs="Consolas"/><w:sz w:val="19"/><w:szCs w:val="19"/></w:rPr></w:style>
<w:style w:type="character" w:default="1" w:styleId="DefaultParagraphFont"><w:name w:val="Default Paragraph Font"/><w:uiPriority w:val="1"/><w:semiHidden/></w:style>
<w:style w:type="character" w:styleId="CodeChar"><w:name w:val="Inline Code"/><w:basedOn w:val="DefaultParagraphFont"/><w:rPr><w:rFonts w:ascii="Consolas" w:hAnsi="Consolas" w:cs="Consolas"/><w:shd w:val="clear" w:color="auto" w:fill="F2F2F2"/></w:rPr></w:style>
<w:style w:type="character" w:styleId="Hyperlink"><w:name w:val="Hyperlink"/><w:basedOn w:val="DefaultParagraphFont"/><w:rPr><w:color w:val="0563C1"/><w:u w:val="single"/></w:rPr></w:style>
<w:style w:type="table" w:default="1" w:styleId="TableNormal"><w:name w:val="Normal Table"/><w:semiHidden/><w:tblPr><w:tblInd w:w="0" w:type="dxa"/><w:tblCellMar><w:top w:w="0" w:type="dxa"/><w:left w:w="108" w:type="dxa"/><w:bottom w:w="0" w:type="dxa"/><w:right w:w="108" w:type="dxa"/></w:tblCellMar></w:tblPr></w:style>
<w:style w:type="table" w:styleId="TableGrid"><w:name w:val="Table Grid"/><w:basedOn w:val="TableNormal"/><w:pPr><w:spacing w:after="0" w:line="240" w:lineRule="auto"/></w:pPr><w:tblPr><w:tblBorders><w:top w:val="single" w:sz="4" w:space="0" w:color="BFBFBF"/><w:left w:val="single" w:sz="4" w:space="0" w:color="BFBFBF"/><w:bottom w:val="single" w:sz="4" w:space="0" w:color="BFBFBF"/><w:right w:val="single" w:sz="4" w:space="0" w:color="BFBFBF"/><w:insideH w:val="single" w:sz="4" w:space="0" w:color="BFBFBF"/><w:insideV w:val="single" w:sz="4" w:space="0" w:color="BFBFBF"/></w:tblBorders></w:tblPr></w:style>
</w:styles>
)";

        std::string NumberingXml(const std::vector<int>& orderedStarts)
        {
            static const char* kBullets[] = { "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA" };   // • ◦ ▪
            std::string x = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:numbering xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
<w:abstractNum w:abstractNumId="0"><w:multiLevelType w:val="hybridMultilevel"/>)";
            for (int l = 0; l < 9; ++l)
                x += std::format(R"(<w:lvl w:ilvl="{}"><w:start w:val="1"/><w:numFmt w:val="bullet"/><w:lvlText w:val="{}"/><w:lvlJc w:val="left"/><w:pPr><w:ind w:left="{}" w:hanging="360"/></w:pPr></w:lvl>)",
                                 l, kBullets[l % 3], 720 * (l + 1));
            x += R"(</w:abstractNum>
<w:abstractNum w:abstractNumId="1"><w:multiLevelType w:val="hybridMultilevel"/>)";
            static const char* kFormats[] = { "decimal", "lowerLetter", "lowerRoman" };
            for (int l = 0; l < 9; ++l)
                x += std::format(R"(<w:lvl w:ilvl="{}"><w:start w:val="1"/><w:numFmt w:val="{}"/><w:lvlText w:val="%{}."/><w:lvlJc w:val="left"/><w:pPr><w:ind w:left="{}" w:hanging="360"/></w:pPr></w:lvl>)",
                                 l, kFormats[l % 3], l + 1, 720 * (l + 1));
            x += "</w:abstractNum>\n<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>\n";
            // One numbering instance per ordered list, so each list starts again at its own number.
            for (size_t i = 0; i < orderedStarts.size(); ++i)
                x += std::format(R"(<w:num w:numId="{}"><w:abstractNumId w:val="1"/><w:lvlOverride w:ilvl="0"><w:startOverride w:val="{}"/></w:lvlOverride></w:num>
)",
                                 i + 2, orderedStarts[i]);
            x += "</w:numbering>\n";
            return x;
        }

        bool DocxImageType(const Image& img, std::string& ext)
        {
            if (img.mime == "image/png") ext = "png";
            else if (img.mime == "image/jpeg") ext = "jpeg";
            else if (img.mime == "image/gif") ext = "gif";
            else if (img.mime == "image/bmp") ext = "bmp";
            else return false;
            return true;
        }

        class DocxWriter
        {
        public:
            explicit DocxWriter(const Document& doc) : m_doc(doc) {}

            bool Write(std::string& out)
            {
                const auto paper = DefaultPaper();
                // Twentieths of a point; 1 inch margins.
                const int pageW = static_cast<int>(paper.widthPt * 20 + 0.5), pageH = static_cast<int>(paper.heightPt * 20 + 0.5);
                m_contentTwips = pageW - 2 * 1440;

                std::string body;
                for (size_t i = 0; i < m_doc.blocks.size(); ++i)
                {
                    const auto& b = m_doc.blocks[i];
                    if (b.kind != BlockKind::ListItem)
                        m_listRun = -1;
                    body += Block(b);
                }
                if (!m_doc.blocks.empty() && m_doc.blocks.back().kind == BlockKind::Table)
                    body += "<w:p/>";   // Word wants a paragraph between a final table and the section
                std::string document = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture"><w:body>)";
                document += body;
                document += std::format(R"(<w:sectPr><w:pgSz w:w="{}" w:h="{}"/><w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440" w:header="708" w:footer="708" w:gutter="0"/></w:sectPr></w:body></w:document>)",
                                        pageW, pageH);

                std::string rels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering" Target="numbering.xml"/>
)";
                std::set<std::string> extensions;
                for (const auto& [index, rid] : m_imageRels)
                {
                    std::string ext;
                    DocxImageType(m_doc.images[static_cast<size_t>(index)], ext);
                    extensions.insert(ext);
                    rels += std::format("<Relationship Id=\"{}\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/image{}.{}\"/>\n",
                                        rid, index + 1, ext);
                }
                for (const auto& [target, rid] : m_linkRels)
                    rels += std::format("<Relationship Id=\"{}\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\" Target=\"{}\" TargetMode=\"External\"/>\n",
                                        rid, XmlEscape(target));
                rels += "</Relationships>\n";

                std::string types = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
)";
                for (const auto& ext : extensions)
                    types += std::format("<Default Extension=\"{}\" ContentType=\"image/{}\"/>\n", ext, ext);
                types += R"(<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>
<Override PartName="/word/numbering.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"/>
<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
</Types>
)";
                const std::string packageRels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
</Relationships>
)";
                const auto now = UtcNowIso();
                const std::string core = std::format(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><dc:title>{}</dc:title><dcterms:created xsi:type="dcterms:W3CDTF">{}</dcterms:created><dcterms:modified xsi:type="dcterms:W3CDTF">{}</dcterms:modified></cp:coreProperties>
)",
                                                     XmlEscape(m_doc.title), now, now);

                ZipWriter zip;
                bool ok = zip.Add("[Content_Types].xml", types) && zip.Add("_rels/.rels", packageRels) && zip.Add("docProps/core.xml", core) &&
                          zip.Add("word/document.xml", document) && zip.Add("word/styles.xml", kStyles) &&
                          zip.Add("word/numbering.xml", NumberingXml(m_orderedStarts)) && zip.Add("word/_rels/document.xml.rels", rels);
                for (const auto& [index, rid] : m_imageRels)
                {
                    std::string ext;
                    DocxImageType(m_doc.images[static_cast<size_t>(index)], ext);
                    ok = ok && zip.Add(std::format("word/media/image{}.{}", index + 1, ext), m_doc.images[static_cast<size_t>(index)].data, true);
                }
                return ok && zip.Finish(out);
            }

        private:
            static std::string Text(std::string_view s)
            {
                // Tabs are their own element; everything else is a preserved-space <w:t>.
                std::string out;
                size_t start = 0;
                while (start <= s.size())
                {
                    const auto tab = s.find('\t', start);
                    const auto end = tab == std::string_view::npos ? s.size() : tab;
                    if (end > start)
                        out += "<w:t xml:space=\"preserve\">" + XmlEscape(s.substr(start, end - start)) + "</w:t>";
                    if (tab == std::string_view::npos)
                        break;
                    out += "<w:tab/>";
                    start = tab + 1;
                }
                return out;
            }

            std::string ImageRun(const Span& s)
            {
                const auto& img = m_doc.images[static_cast<size_t>(s.image)];
                std::string ext;
                if (!DocxImageType(img, ext))
                    return s.text.empty() ? std::string() : "<w:r>" + Text("[" + s.text + "]") + "</w:r>";
                auto rid = m_imageRels.find(s.image);
                if (rid == m_imageRels.end())
                    rid = m_imageRels.emplace(s.image, std::format("rIdImg{}", s.image + 1)).first;
                // Size in EMU (914400 per inch; 9525 per pixel at 96 DPI), shrunk to the text width.
                double w = img.width > 0 ? img.width : 400, h = img.height > 0 ? img.height : 300;
                const double maxW = m_contentTwips / 1440.0 * 96.0;
                if (w > maxW)
                {
                    h *= maxW / w;
                    w = maxW;
                }
                const auto cx = static_cast<long long>(w * 9525), cy = static_cast<long long>(h * 9525);
                const int id = ++m_drawingId;
                return std::format(
                    R"(<w:r><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0"><wp:extent cx="{0}" cy="{1}"/><wp:effectExtent l="0" t="0" r="0" b="0"/><wp:docPr id="{2}" name="Picture {2}" descr="{3}"/><wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect="1"/></wp:cNvGraphicFramePr><a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture"><pic:pic><pic:nvPicPr><pic:cNvPr id="{2}" name="{4}"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed="{5}"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="{0}" cy="{1}"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>)",
                    cx, cy, id, XmlEscape(s.text), XmlEscape(img.name), rid->second);
            }

            std::string Runs(const std::vector<Span>& spans, bool forceBold = false)
            {
                std::string out;
                for (const auto& s : spans)
                {
                    if (s.lineBreak)
                    {
                        out += "<w:r><w:br/></w:r>";
                        continue;
                    }
                    if (s.image >= 0 && static_cast<size_t>(s.image) < m_doc.images.size())
                    {
                        out += ImageRun(s);
                        continue;
                    }
                    std::string rpr;
                    if (!s.link.empty())
                        rpr += "<w:rStyle w:val=\"Hyperlink\"/>";
                    else if (s.code)
                        rpr += "<w:rStyle w:val=\"CodeChar\"/>";
                    if (s.bold || forceBold)
                        rpr += "<w:b/>";
                    if (s.italic)
                        rpr += "<w:i/>";
                    if (s.strike)
                        rpr += "<w:strike/>";
                    // Code text keeps its line breaks.
                    std::string runs;
                    size_t start = 0;
                    while (start <= s.text.size())
                    {
                        const auto nl = s.text.find('\n', start);
                        const auto end = nl == std::string::npos ? s.text.size() : nl;
                        runs += "<w:r>" + (rpr.empty() ? std::string() : "<w:rPr>" + rpr + "</w:rPr>") + Text(std::string_view(s.text).substr(start, end - start)) + "</w:r>";
                        if (nl == std::string::npos)
                            break;
                        runs += "<w:r><w:br/></w:r>";
                        start = nl + 1;
                    }
                    if (!s.link.empty())
                    {
                        if (s.link.starts_with("#"))
                            runs = "<w:hyperlink w:anchor=\"" + XmlEscape(s.link.substr(1)) + "\">" + runs + "</w:hyperlink>";
                        else
                        {
                            auto rid = m_linkRels.find(s.link);
                            if (rid == m_linkRels.end())
                                rid = m_linkRels.emplace(s.link, std::format("rIdLink{}", m_linkRels.size() + 1)).first;
                            runs = "<w:hyperlink r:id=\"" + rid->second + "\" w:history=\"1\">" + runs + "</w:hyperlink>";
                        }
                    }
                    out += runs;
                }
                return out;
            }

            std::string Block(const docs::Block& b)
            {
                switch (b.kind)
                {
                case BlockKind::Heading:
                    return std::format("<w:p><w:pPr><w:pStyle w:val=\"Heading{}\"/></w:pPr>{}</w:p>", std::clamp(b.level, 1, 6), Runs(b.spans));
                case BlockKind::Paragraph:
                    return "<w:p>" + Runs(b.spans) + "</w:p>";
                case BlockKind::Quote:
                    return "<w:p><w:pPr><w:pStyle w:val=\"Quote\"/></w:pPr>" + Runs(b.spans) + "</w:p>";
                case BlockKind::Code:
                {
                    // One paragraph per line keeps Word's own "Source Code" look.
                    std::string out;
                    const auto text = PlainText(b.spans);
                    size_t start = 0;
                    while (start <= text.size())
                    {
                        const auto nl = text.find('\n', start);
                        const auto end = nl == std::string::npos ? text.size() : nl;
                        out += "<w:p><w:pPr><w:pStyle w:val=\"SourceCode\"/></w:pPr><w:r>" + Text(std::string_view(text).substr(start, end - start)) + "</w:r></w:p>";
                        if (nl == std::string::npos)
                            break;
                        start = nl + 1;
                    }
                    return out;
                }
                case BlockKind::Rule:
                    return R"(<w:p><w:pPr><w:pBdr><w:bottom w:val="single" w:sz="6" w:space="1" w:color="BFBFBF"/></w:pBdr></w:pPr></w:p>)";
                case BlockKind::ListItem:
                {
                    int numId = 1;
                    if (b.ordered)
                    {
                        if (m_listRun < 0)
                        {
                            m_orderedStarts.push_back(std::max(1, b.start));
                            m_listRun = static_cast<int>(m_orderedStarts.size()) + 1;
                        }
                        numId = m_listRun;
                    }
                    return std::format("<w:p><w:pPr><w:pStyle w:val=\"ListParagraph\"/><w:numPr><w:ilvl w:val=\"{}\"/><w:numId w:val=\"{}\"/></w:numPr></w:pPr>{}</w:p>",
                                       std::clamp(b.level, 0, 8), numId, Runs(b.spans));
                }
                case BlockKind::Table:
                {
                    size_t cols = 0;
                    for (const auto& r : b.rows)
                        cols = std::max(cols, r.size());
                    if (cols == 0)
                        return {};
                    const int colW = static_cast<int>(m_contentTwips / static_cast<int>(cols));
                    std::string out = R"(<w:tbl><w:tblPr><w:tblStyle w:val="TableGrid"/><w:tblW w:w="5000" w:type="pct"/><w:tblLook w:val="04A0" w:firstRow="1" w:lastRow="0" w:firstColumn="0" w:lastColumn="0" w:noHBand="0" w:noVBand="1"/></w:tblPr><w:tblGrid>)";
                    for (size_t c = 0; c < cols; ++c)
                        out += std::format("<w:gridCol w:w=\"{}\"/>", colW);
                    out += "</w:tblGrid>";
                    for (size_t r = 0; r < b.rows.size(); ++r)
                    {
                        const bool header = r == 0 && b.headerRow;
                        out += header ? "<w:tr><w:trPr><w:tblHeader/></w:trPr>" : "<w:tr>";
                        for (size_t c = 0; c < cols; ++c)
                        {
                            out += std::format("<w:tc><w:tcPr><w:tcW w:w=\"{}\" w:type=\"dxa\"/></w:tcPr><w:p>", colW);
                            if (c < b.rows[r].size())
                                out += Runs(b.rows[r][c].spans, header);
                            out += "</w:p></w:tc>";
                        }
                        out += "</w:tr>";
                    }
                    out += "</w:tbl>";
                    return out;
                }
                }
                return {};
            }

            const Document& m_doc;
            int m_contentTwips = 9026;
            int m_listRun = -1;               // numId of the ordered list being written, -1 between lists
            std::vector<int> m_orderedStarts; // start of each ordered list (numId = index + 2)
            std::map<int, std::string> m_imageRels;
            std::map<std::string, std::string> m_linkRels;
            int m_drawingId = 0;
        };
    }

    bool ReadDocx(std::string bytes, Document& doc, std::string& error)
    {
        ZipReader zip;
        if (!zip.Open(std::move(bytes)))
        {
            error = "not a Word document (not a zip package)";
            return false;
        }
        return DocxReader(zip, doc).Read(error);
    }

    bool WriteDocx(const Document& doc, std::string& out)
    {
        return DocxWriter(doc).Write(out);
    }
}
