#include "docs/Bytes.h"
#include "docs/Formats.h"

#include <md4c.h>

#include <algorithm>
#include <format>

namespace sd::docs
{
    namespace
    {
        // Builds the model from md4c's callbacks (a SAX-style walk of the CommonMark + GFM tree).
        struct MdBuilder
        {
            Document* doc = nullptr;
            std::wstring baseDir;

            struct List
            {
                bool ordered = false;
                int next = 1;
            };
            std::vector<List> lists;
            int quoteDepth = 0;
            int current = -1;        // block receiving text, or -1
            bool freshItem = false;  // a list item just opened and has no paragraph yet
            bool inCodeBlock = false;
            bool inHtmlBlock = false;

            // Tables
            int table = -1;
            bool inHead = false;

            // Inline style counters
            int em = 0, strong = 0, del = 0, code = 0;
            std::vector<std::string> links;
            int imageDepth = 0;
            std::string imageAlt;
            int imageIndex = -1;

            Block& Cur() { return doc->blocks[static_cast<size_t>(current)]; }

            std::vector<Span>* Target()
            {
                if (table >= 0)
                {
                    auto& t = doc->blocks[static_cast<size_t>(table)];
                    if (t.rows.empty() || t.rows.back().empty())
                        return nullptr;
                    return &t.rows.back().back().spans;
                }
                if (current < 0)
                {
                    Block b;
                    b.kind = quoteDepth > 0 ? BlockKind::Quote : BlockKind::Paragraph;
                    if (!lists.empty())
                    {
                        b.kind = BlockKind::ListItem;
                        b.level = static_cast<int>(lists.size()) - 1;
                        b.ordered = lists.back().ordered;
                    }
                    doc->blocks.push_back(std::move(b));
                    current = static_cast<int>(doc->blocks.size()) - 1;
                }
                return &Cur().spans;
            }

            Span Style() const
            {
                Span s;
                s.bold = strong > 0;
                s.italic = em > 0;
                s.strike = del > 0;
                s.code = code > 0;
                if (!links.empty())
                    s.link = links.back();
                return s;
            }

            void AddText(std::string text)
            {
                if (imageDepth > 0)
                {
                    imageAlt += text;
                    return;
                }
                auto* spans = Target();
                if (!spans)
                    return;
                Span s = Style();
                s.text = std::move(text);
                spans->push_back(std::move(s));
            }

            void AddBreak()
            {
                if (imageDepth > 0)
                    return;
                if (auto* spans = Target())
                {
                    Span s;
                    s.lineBreak = true;
                    spans->push_back(s);
                }
            }

            void NewBlock(BlockKind kind, int level = 0)
            {
                Block b;
                b.kind = kind;
                b.level = level;
                doc->blocks.push_back(std::move(b));
                current = static_cast<int>(doc->blocks.size()) - 1;
            }
        };

        std::string Attr(const MD_ATTRIBUTE& a)
        {
            std::string out;
            if (!a.text)
                return out;
            if (!a.substr_offsets || !a.substr_types)
                return std::string(a.text, a.size);
            for (int i = 0; a.substr_offsets[i] < a.size; ++i)
            {
                const auto off = a.substr_offsets[i];
                const auto end = a.substr_offsets[i + 1];
                const std::string_view piece(a.text + off, end - off);
                if (a.substr_types[i] == MD_TEXT_ENTITY && piece.size() > 2)
                {
                    const auto decoded = DecodeEntity(piece.substr(1, piece.size() - 2));
                    out += decoded.empty() ? std::string(piece) : decoded;
                }
                else if (a.substr_types[i] == MD_TEXT_NULLCHAR)
                    out += "\xEF\xBF\xBD";
                else
                    out += piece;
            }
            return out;
        }

        int EnterBlock(MD_BLOCKTYPE type, void* detail, void* user)
        {
            auto& b = *static_cast<MdBuilder*>(user);
            switch (type)
            {
            case MD_BLOCK_DOC:
                break;
            case MD_BLOCK_QUOTE:
                ++b.quoteDepth;
                b.current = -1;
                break;
            case MD_BLOCK_UL:
                b.lists.push_back({ false, 1 });
                b.current = -1;
                break;
            case MD_BLOCK_OL:
                b.lists.push_back({ true, static_cast<int>(static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start) });
                b.current = -1;
                break;
            case MD_BLOCK_LI:
            {
                b.NewBlock(BlockKind::ListItem, static_cast<int>(b.lists.size()) - 1);
                auto& item = b.Cur();
                if (!b.lists.empty())
                {
                    item.ordered = b.lists.back().ordered;
                    item.start = b.lists.back().next++;
                }
                const auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
                if (li && li->is_task)
                {
                    Span box;
                    box.text = (li->task_mark == 'x' || li->task_mark == 'X') ? "\xE2\x98\x91 " : "\xE2\x98\x90 ";   // ☑ / ☐
                    item.spans.push_back(box);
                }
                b.freshItem = true;
                break;
            }
            case MD_BLOCK_HR:
                b.NewBlock(BlockKind::Rule);
                b.current = -1;
                break;
            case MD_BLOCK_H:
                b.NewBlock(BlockKind::Heading, static_cast<int>(static_cast<MD_BLOCK_H_DETAIL*>(detail)->level));
                break;
            case MD_BLOCK_CODE:
                b.NewBlock(BlockKind::Code);
                b.inCodeBlock = true;
                break;
            case MD_BLOCK_HTML:
                b.inHtmlBlock = true;
                break;
            case MD_BLOCK_P:
                if (b.table >= 0)
                    break;
                if (!b.lists.empty() && b.current >= 0 && b.Cur().kind == BlockKind::ListItem)
                {
                    // A loose list item's first paragraph stays in the item; later ones continue it
                    // after a blank line.
                    if (!b.freshItem)
                    {
                        b.AddBreak();
                        b.AddBreak();
                    }
                    b.freshItem = false;
                    break;
                }
                b.NewBlock(b.quoteDepth > 0 ? BlockKind::Quote : BlockKind::Paragraph);
                break;
            case MD_BLOCK_TABLE:
            {
                b.NewBlock(BlockKind::Table);
                b.table = b.current;
                b.current = -1;
                break;
            }
            case MD_BLOCK_THEAD:
                b.inHead = true;
                if (b.table >= 0)
                    b.doc->blocks[static_cast<size_t>(b.table)].headerRow = true;
                break;
            case MD_BLOCK_TBODY:
                b.inHead = false;
                break;
            case MD_BLOCK_TR:
                if (b.table >= 0)
                    b.doc->blocks[static_cast<size_t>(b.table)].rows.emplace_back();
                break;
            case MD_BLOCK_TH:
            case MD_BLOCK_TD:
                if (b.table >= 0 && !b.doc->blocks[static_cast<size_t>(b.table)].rows.empty())
                    b.doc->blocks[static_cast<size_t>(b.table)].rows.back().emplace_back();
                break;
            }
            return 0;
        }

        int LeaveBlock(MD_BLOCKTYPE type, void*, void* user)
        {
            auto& b = *static_cast<MdBuilder*>(user);
            switch (type)
            {
            case MD_BLOCK_QUOTE:
                --b.quoteDepth;
                b.current = -1;
                break;
            case MD_BLOCK_UL:
            case MD_BLOCK_OL:
                if (!b.lists.empty())
                    b.lists.pop_back();
                b.current = -1;
                break;
            case MD_BLOCK_LI:
                b.current = -1;
                b.freshItem = false;
                break;
            case MD_BLOCK_H:
                b.current = -1;
                break;
            case MD_BLOCK_P:
                if (b.table < 0 && (b.lists.empty() || b.current < 0 || b.Cur().kind != BlockKind::ListItem))
                    b.current = -1;
                break;
            case MD_BLOCK_CODE:
                if (b.current >= 0)
                {
                    // md4c ends every code line with '\n'; the model keeps lines without the last one.
                    auto& spans = b.Cur().spans;
                    if (!spans.empty() && !spans.back().text.empty() && spans.back().text.back() == '\n')
                        spans.back().text.pop_back();
                }
                b.inCodeBlock = false;
                b.current = -1;
                break;
            case MD_BLOCK_HTML:
                b.inHtmlBlock = false;
                break;
            case MD_BLOCK_TABLE:
                b.table = -1;
                b.current = -1;
                break;
            default:
                break;
            }
            return 0;
        }

        int EnterSpan(MD_SPANTYPE type, void* detail, void* user)
        {
            auto& b = *static_cast<MdBuilder*>(user);
            switch (type)
            {
            case MD_SPAN_EM: ++b.em; break;
            case MD_SPAN_STRONG: ++b.strong; break;
            case MD_SPAN_DEL: ++b.del; break;
            case MD_SPAN_CODE: ++b.code; break;
            case MD_SPAN_U: ++b.em; break;   // underline has no slot in the model; italic is the closest
            case MD_SPAN_A:
                b.links.push_back(Attr(static_cast<MD_SPAN_A_DETAIL*>(detail)->href));
                break;
            case MD_SPAN_IMG:
                if (b.imageDepth++ == 0)
                {
                    b.imageAlt.clear();
                    b.imageIndex = LoadImageRef(Attr(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src), b.baseDir, *b.doc);
                }
                break;
            default:
                break;
            }
            return 0;
        }

        int LeaveSpan(MD_SPANTYPE type, void*, void* user)
        {
            auto& b = *static_cast<MdBuilder*>(user);
            switch (type)
            {
            case MD_SPAN_EM: --b.em; break;
            case MD_SPAN_STRONG: --b.strong; break;
            case MD_SPAN_DEL: --b.del; break;
            case MD_SPAN_CODE: --b.code; break;
            case MD_SPAN_U: --b.em; break;
            case MD_SPAN_A:
                if (!b.links.empty())
                    b.links.pop_back();
                break;
            case MD_SPAN_IMG:
                if (--b.imageDepth == 0)
                {
                    auto alt = std::move(b.imageAlt);
                    b.imageAlt.clear();
                    if (auto* spans = b.Target())
                    {
                        Span s = b.Style();
                        s.text = std::move(alt);
                        s.image = b.imageIndex;
                        if (s.image < 0 && s.text.empty())
                            break;
                        spans->push_back(std::move(s));
                    }
                }
                break;
            default:
                break;
            }
            return 0;
        }

        int Text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* user)
        {
            auto& b = *static_cast<MdBuilder*>(user);
            if (b.inHtmlBlock)
                return 0;
            const std::string_view t(text, size);
            switch (type)
            {
            case MD_TEXT_NORMAL:
            case MD_TEXT_CODE:
            case MD_TEXT_LATEXMATH:
                b.AddText(std::string(t));
                break;
            case MD_TEXT_NULLCHAR:
                b.AddText("\xEF\xBF\xBD");
                break;
            case MD_TEXT_BR:
                b.AddBreak();
                break;
            case MD_TEXT_SOFTBR:
                b.AddText(" ");
                break;
            case MD_TEXT_ENTITY:
            {
                const auto decoded = t.size() > 2 ? DecodeEntity(t.substr(1, t.size() - 2)) : std::string();
                b.AddText(decoded.empty() ? std::string(t) : decoded);
                break;
            }
            case MD_TEXT_HTML:
            {
                // Raw inline HTML: only <br> carries meaning for the model.
                const auto lower = AsciiLower(t);
                if (lower.starts_with("<br"))
                    b.AddBreak();
                break;
            }
            }
            return 0;
        }

        // ---- Writer ---------------------------------------------------------------------------------

        // Backslash-escapes the characters that would start Markdown syntax inside running text.
        std::string EscapeMd(std::string_view s, bool lineStart)
        {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                const char c = s[i];
                switch (c)
                {
                case '\\': case '`': case '*': case '_': case '[': case ']': case '<': case '>': case '|': case '~':
                    out.push_back('\\');
                    out.push_back(c);
                    break;
                case '#': case '+': case '-': case '=':
                    if (lineStart && out.find_first_not_of(' ') == std::string::npos)
                        out.push_back('\\');
                    out.push_back(c);
                    break;
                case '.': case ')':
                    // "1." at the start of a line would become a numbered list.
                    if (lineStart && !out.empty() && out.find_first_not_of("0123456789") == std::string::npos)
                        out.push_back('\\');
                    out.push_back(c);
                    break;
                default:
                    out.push_back(c);
                }
            }
            return out;
        }

        std::string LinkTarget(const std::string& href)
        {
            // Angle brackets keep targets with spaces or parentheses intact.
            if (href.find_first_of(" ()<>") != std::string::npos)
            {
                std::string t;
                for (const char c : href)
                    t += (c == '<' || c == '>') ? std::format("%{:02X}", static_cast<unsigned char>(c)) : std::string(1, c);
                return "<" + t + ">";
            }
            return href;
        }

        std::string InlineMd(const Document& doc, const std::vector<Span>& spans, const std::string& imageDir, const std::string& breakPrefix)
        {
            std::string out;
            bool lineStart = true;
            for (size_t i = 0; i < spans.size(); ++i)
            {
                const auto& s = spans[i];
                if (s.lineBreak)
                {
                    out += "\\\n" + breakPrefix;
                    lineStart = true;
                    continue;
                }
                if (s.image >= 0 && static_cast<size_t>(s.image) < doc.images.size())
                {
                    out += "![" + EscapeMd(s.text, false) + "](" + LinkTarget(imageDir + doc.images[static_cast<size_t>(s.image)].name) + ")";
                    lineStart = false;
                    continue;
                }
                std::string text;
                if (s.code)
                {
                    // A fence longer than any backtick run inside.
                    size_t longest = 0, run = 0;
                    for (const char c : s.text)
                    {
                        run = c == '`' ? run + 1 : 0;
                        longest = std::max(longest, run);
                    }
                    const std::string fence(longest + 1, '`');
                    const bool pad = !s.text.empty() && (s.text.front() == '`' || s.text.back() == '`');
                    text = fence + (pad ? " " : "") + s.text + (pad ? " " : "") + fence;
                }
                else
                {
                    // Emphasis markers must hug the text: move outer spaces outside them.
                    std::string_view body = s.text;
                    size_t lead = 0, trail = 0;
                    while (lead < body.size() && body[lead] == ' ')
                        ++lead;
                    while (trail < body.size() - lead && body[body.size() - 1 - trail] == ' ')
                        ++trail;
                    const auto core = body.substr(lead, body.size() - lead - trail);
                    std::string open, close;
                    if (!core.empty())
                    {
                        if (s.strike) { open += "~~"; close = "~~" + close; }
                        if (s.bold) { open += "**"; close = "**" + close; }
                        if (s.italic) { open += "*"; close = "*" + close; }
                    }
                    text = std::string(lead, ' ') + open + EscapeMd(core, lineStart && lead == 0) + close + std::string(trail, ' ');
                }
                if (!s.link.empty())
                    text = "[" + text + "](" + LinkTarget(s.link) + ")";
                out += text;
                lineStart = false;
            }
            return out;
        }

        std::string CellMd(const Document& doc, const std::vector<Span>& spans, const std::string& imageDir)
        {
            auto t = InlineMd(doc, spans, imageDir, "");
            // A table row is one line: line breaks become <br>.
            std::string out;
            for (size_t i = 0; i < t.size(); ++i)
            {
                if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '\n')
                {
                    out += "<br>";
                    ++i;
                }
                else if (t[i] == '\n')
                    out += ' ';
                else
                    out.push_back(t[i]);
            }
            return out.empty() ? " " : out;
        }
    }

    void ReadMarkdown(std::string_view utf8, const std::wstring& baseDir, Document& doc)
    {
        MdBuilder b;
        b.doc = &doc;
        b.baseDir = baseDir;
        MD_PARSER parser{};
        parser.abi_version = 0;
        parser.flags = MD_DIALECT_GITHUB;   // tables, strikethrough, task lists, bare links
        parser.enter_block = &EnterBlock;
        parser.leave_block = &LeaveBlock;
        parser.enter_span = &EnterSpan;
        parser.leave_span = &LeaveSpan;
        parser.text = &Text;
        md_parse(utf8.data(), static_cast<MD_SIZE>(utf8.size()), &parser, &b);
        for (auto& block : doc.blocks)
        {
            NormalizeSpans(block.spans);
            for (auto& row : block.rows)
                for (auto& cell : row)
                    NormalizeSpans(cell.spans);
        }
    }

    std::string WriteMarkdown(const Document& doc, const std::string& imageDir)
    {
        std::string out;
        const auto blank = [&] {
            if (!out.empty())
                out += "\n";
        };
        // Content indent of each open list level, for nested items.
        std::vector<size_t> indents;
        for (size_t bi = 0; bi < doc.blocks.size(); ++bi)
        {
            const auto& b = doc.blocks[bi];
            if (b.kind != BlockKind::ListItem)
                indents.clear();
            switch (b.kind)
            {
            case BlockKind::Heading:
                blank();
                out += std::string(static_cast<size_t>(std::clamp(b.level, 1, 6)), '#') + " " + InlineMd(doc, b.spans, imageDir, "") + "\n";
                break;
            case BlockKind::Paragraph:
                blank();
                out += InlineMd(doc, b.spans, imageDir, "") + "\n";
                break;
            case BlockKind::Quote:
                blank();
                out += "> " + InlineMd(doc, b.spans, imageDir, "> ") + "\n";
                break;
            case BlockKind::ListItem:
            {
                const size_t level = static_cast<size_t>(std::max(0, b.level));
                const bool continuing = bi > 0 && doc.blocks[bi - 1].kind == BlockKind::ListItem;
                if (!continuing)
                {
                    blank();
                    indents.clear();
                }
                if (indents.size() > level + 1)
                    indents.resize(level + 1);
                while (indents.size() < level + 1)
                    indents.push_back(indents.empty() ? 0 : indents.back());
                const size_t indent = level == 0 ? 0 : indents[level - 1];
                // Number from the item's own start, so a list split by other blocks keeps counting.
                const std::string marker = b.ordered ? std::format("{}. ", b.start) : std::string("- ");
                indents[level] = indent + marker.size();
                out += std::string(indent, ' ') + marker + InlineMd(doc, b.spans, imageDir, std::string(indent + marker.size(), ' ')) + "\n";
                break;
            }
            case BlockKind::Code:
            {
                blank();
                const auto text = PlainText(b.spans);
                size_t longest = 0, run = 0;
                for (const char c : text)
                {
                    run = c == '`' ? run + 1 : 0;
                    longest = std::max(longest, run);
                }
                const std::string fence(std::max<size_t>(3, longest + 1), '`');
                out += fence + "\n" + text + "\n" + fence + "\n";
                break;
            }
            case BlockKind::Rule:
                blank();
                out += "---\n";
                break;
            case BlockKind::Table:
            {
                if (b.rows.empty())
                    break;
                blank();
                size_t cols = 0;
                for (const auto& r : b.rows)
                    cols = std::max(cols, r.size());
                const auto row = [&](const std::vector<TableCell>& r) {
                    out += "|";
                    for (size_t c = 0; c < cols; ++c)
                        out += " " + (c < r.size() ? CellMd(doc, r[c].spans, imageDir) : std::string(" ")) + " |";
                    out += "\n";
                };
                // GFM tables need a header row; an empty one is written when the table has none.
                if (b.headerRow)
                    row(b.rows[0]);
                else
                    row(std::vector<TableCell>(cols));
                out += "|";
                for (size_t c = 0; c < cols; ++c)
                    out += " --- |";
                out += "\n";
                for (size_t r = b.headerRow ? 1 : 0; r < b.rows.size(); ++r)
                    row(b.rows[r]);
                break;
            }
            }
        }
        return out;
    }
}
