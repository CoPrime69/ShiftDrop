#include "docs/Bytes.h"
#include "docs/Formats.h"

#include <algorithm>
#include <format>

namespace sd::docs
{
    void ReadPlainText(std::string_view utf8, Document& doc)
    {
        const auto text = NormalizeNewlines(utf8);
        // Paragraphs are separated by blank lines; single line breaks inside one are kept.
        Block current;
        bool any = false;
        const auto flush = [&] {
            if (any)
            {
                NormalizeSpans(current.spans);
                doc.blocks.push_back(std::move(current));
            }
            current = Block{};
            any = false;
        };
        size_t start = 0;
        while (start <= text.size())
        {
            auto end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            auto line = std::string_view(text).substr(start, end - start);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
                line.remove_suffix(1);
            if (line.empty())
                flush();
            else
            {
                if (any)
                {
                    Span br;
                    br.lineBreak = true;
                    current.spans.push_back(br);
                }
                Span s;
                s.text = std::string(line);
                current.spans.push_back(std::move(s));
                any = true;
            }
            if (end == text.size())
                break;
            start = end + 1;
        }
        flush();
    }

    namespace
    {
        std::string InlineText(const Document& doc, const std::vector<Span>& spans, const std::string& breakPrefix)
        {
            std::string out;
            for (const auto& s : spans)
            {
                if (s.lineBreak)
                {
                    out += "\r\n" + breakPrefix;
                    continue;
                }
                if (s.image >= 0)
                {
                    (void)doc;
                    if (!s.text.empty())
                        out += "[" + s.text + "]";
                    continue;
                }
                out += s.text;
                // A link whose text is not its address keeps the address in brackets.
                if (!s.link.empty() && s.link != s.text && !s.link.starts_with("#") && s.text.find(s.link) == std::string::npos)
                    out += " (" + s.link + ")";
            }
            return out;
        }
    }

    std::string WritePlainText(const Document& doc)
    {
        std::string out;
        const auto blank = [&] {
            if (!out.empty())
                out += "\r\n";
        };
        for (size_t bi = 0; bi < doc.blocks.size(); ++bi)
        {
            const auto& b = doc.blocks[bi];
            switch (b.kind)
            {
            case BlockKind::Heading:
            case BlockKind::Paragraph:
                blank();
                out += InlineText(doc, b.spans, "") + "\r\n";
                break;
            case BlockKind::Quote:
                blank();
                out += "> " + InlineText(doc, b.spans, "> ") + "\r\n";
                break;
            case BlockKind::ListItem:
            {
                if (bi == 0 || doc.blocks[bi - 1].kind != BlockKind::ListItem)
                    blank();
                const std::string indent(static_cast<size_t>(std::max(0, b.level)) * 2, ' ');
                const std::string marker = b.ordered ? std::format("{}. ", b.start) : std::string("\xE2\x80\xA2 ");   // •
                const std::string hang(indent.size() + (b.ordered ? marker.size() : 2), ' ');
                out += indent + marker + InlineText(doc, b.spans, hang) + "\r\n";
                break;
            }
            case BlockKind::Code:
            {
                blank();
                const auto text = PlainText(b.spans);
                size_t start = 0;
                while (start <= text.size())
                {
                    auto end = text.find('\n', start);
                    if (end == std::string::npos)
                        end = text.size();
                    out += "    " + text.substr(start, end - start) + "\r\n";
                    if (end == text.size())
                        break;
                    start = end + 1;
                }
                break;
            }
            case BlockKind::Rule:
                blank();
                out += "----------\r\n";
                break;
            case BlockKind::Table:
                blank();
                for (const auto& row : b.rows)
                {
                    std::string line;
                    for (size_t c = 0; c < row.size(); ++c)
                    {
                        if (c)
                            line += '\t';
                        auto cell = InlineText(doc, row[c].spans, "");
                        std::replace(cell.begin(), cell.end(), '\r', ' ');
                        std::replace(cell.begin(), cell.end(), '\n', ' ');
                        line += cell;
                    }
                    out += line + "\r\n";
                }
                break;
            }
        }
        return out;
    }
}
