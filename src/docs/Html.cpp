#include "docs/Bytes.h"
#include "FileIo.h"
#include "PathUtil.h"
#include "Strings.h"
#include "docs/Formats.h"
#include "Utf.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>

namespace sd::docs
{
    // ---- Shared helpers -------------------------------------------------------------------------------

    std::string Base64Decode(std::string_view s)
    {
        std::string out;
        out.reserve(s.size() * 3 / 4);
        unsigned buffer = 0;
        int bits = 0;
        for (const char c : s)
        {
            int v;
            if (c >= 'A' && c <= 'Z') v = c - 'A';
            else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
            else if (c >= '0' && c <= '9') v = c - '0' + 52;
            else if (c == '+' || c == '-') v = 62;
            else if (c == '/' || c == '_') v = 63;
            else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
            else break;   // '=' padding or junk
            buffer = buffer << 6 | static_cast<unsigned>(v);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
            }
        }
        return out;
    }

    std::string Base64Encode(std::string_view s)
    {
        static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((s.size() + 2) / 3 * 4);
        size_t i = 0;
        for (; i + 2 < s.size(); i += 3)
        {
            const unsigned n = static_cast<unsigned char>(s[i]) << 16 | static_cast<unsigned char>(s[i + 1]) << 8 | static_cast<unsigned char>(s[i + 2]);
            out += kAlphabet[n >> 18];
            out += kAlphabet[(n >> 12) & 63];
            out += kAlphabet[(n >> 6) & 63];
            out += kAlphabet[n & 63];
        }
        if (i < s.size())
        {
            unsigned n = static_cast<unsigned char>(s[i]) << 16;
            if (i + 1 < s.size())
                n |= static_cast<unsigned char>(s[i + 1]) << 8;
            out += kAlphabet[n >> 18];
            out += kAlphabet[(n >> 12) & 63];
            out += i + 1 < s.size() ? kAlphabet[(n >> 6) & 63] : '=';
            out += '=';
        }
        return out;
    }

    std::string PercentDecode(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0)
            {
                out.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
                i += 2;
            }
            else
                out.push_back(s[i]);
        }
        return out;
    }

    int LoadImageRef(std::string_view srcIn, const std::wstring& baseDir, Document& doc)
    {
        const auto src = Trim(srcIn);
        if (src.empty())
            return -1;
        const auto lower = AsciiLower(src.substr(0, std::min<size_t>(src.size(), 16)));
        if (lower.starts_with("data:"))
        {
            const auto comma = src.find(',');
            if (comma == std::string_view::npos || src.substr(0, comma).find(";base64") == std::string_view::npos)
                return -1;
            auto bytes = Base64Decode(src.substr(comma + 1));
            if (SniffImage(bytes).empty())
                return -1;
            return AddImage(doc, std::move(bytes));
        }
        std::wstring path;
        if (lower.starts_with("file:"))
        {
            wchar_t buffer[2048]{};
            DWORD n = ARRAYSIZE(buffer);
            if (FAILED(PathCreateFromUrlW(Widen(src).c_str(), buffer, &n, 0)))
                return -1;
            path = buffer;
        }
        else
        {
            // Any other scheme (http:, https:, ftp:...) is remote: never fetched (offline). A single
            // letter before ':' is a drive ("C:\pics\a.png").
            const auto colon = src.find(':');
            const auto slash = src.find_first_of("/\\");
            if (colon != std::string_view::npos && colon > 1 && (slash == std::string_view::npos || colon < slash))
                return -1;
            if (src.starts_with("//"))
                return -1;
            auto rel = std::string(src.substr(0, src.find_first_of("?#")));
            path = Widen(PercentDecode(rel));
            std::replace(path.begin(), path.end(), L'/', L'\\');
            const std::filesystem::path p(path);
            if (p.is_relative())
            {
                if (baseDir.empty())
                    return -1;
                path = (std::filesystem::path(baseDir) / p).lexically_normal().wstring();
            }
        }
        if (!IsFile(path) || FileSize(path) > 64u * 1024 * 1024)
            return -1;
        std::string bytes;
        if (!ReadFileBytes(path, bytes) || SniffImage(bytes).empty())
            return -1;
        return AddImage(doc, std::move(bytes));
    }

    // ---- Reader ---------------------------------------------------------------------------------------

    namespace
    {
        struct Tag
        {
            std::string name;   // lower case, without '/'
            bool closing = false;
            bool selfClosing = false;
            std::vector<std::pair<std::string, std::string>> attrs;

            std::string Attr(std::string_view key) const
            {
                for (const auto& [k, v] : attrs)
                    if (k == key)
                        return v;
                return {};
            }
        };

        // Parses the tag starting at s[i] == '<'. Returns false when it is not a tag (a stray '<').
        bool ParseTag(std::string_view s, size_t& i, Tag& tag)
        {
            size_t j = i + 1;
            tag = Tag{};
            if (j < s.size() && s[j] == '/')
            {
                tag.closing = true;
                ++j;
            }
            const size_t nameStart = j;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '-' || s[j] == ':'))
                ++j;
            if (j == nameStart)
                return false;
            tag.name = AsciiLower(s.substr(nameStart, j - nameStart));
            while (j < s.size() && s[j] != '>')
            {
                if (s[j] == '/')
                {
                    tag.selfClosing = true;
                    ++j;
                    continue;
                }
                if (std::isspace(static_cast<unsigned char>(s[j])))
                {
                    ++j;
                    continue;
                }
                const size_t keyStart = j;
                while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '=' && s[j] != '>' && s[j] != '/')
                    ++j;
                std::string key = AsciiLower(s.substr(keyStart, j - keyStart));
                while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j])))
                    ++j;
                std::string value;
                if (j < s.size() && s[j] == '=')
                {
                    ++j;
                    while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j])))
                        ++j;
                    if (j < s.size() && (s[j] == '"' || s[j] == '\''))
                    {
                        const char q = s[j++];
                        const auto end = s.find(q, j);
                        const auto stop = end == std::string_view::npos ? s.size() : end;
                        value = DecodeEntities(s.substr(j, stop - j));
                        j = stop == s.size() ? stop : stop + 1;
                    }
                    else
                    {
                        const size_t vs = j;
                        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '>')
                            ++j;
                        value = DecodeEntities(s.substr(vs, j - vs));
                    }
                }
                if (!key.empty())
                {
                    tag.selfClosing = false;   // a '/' before an attribute was not the end
                    tag.attrs.emplace_back(std::move(key), std::move(value));
                }
            }
            i = j < s.size() ? j + 1 : s.size();
            return true;
        }

        bool IsVoid(const std::string& n)
        {
            static const char* kVoid[] = { "br", "img", "hr", "meta", "link", "input", "area", "base", "col", "embed", "param", "source", "track", "wbr" };
            return std::any_of(std::begin(kVoid), std::end(kVoid), [&](const char* v) { return n == v; });
        }

        bool IsBlockTag(const std::string& n)
        {
            static const char* kBlocks[] = { "p", "div", "section", "article", "header", "footer", "main", "nav", "aside", "figure", "figcaption",
                                             "dl", "dt", "dd", "address", "center", "form", "fieldset", "details", "summary", "caption", "body", "html" };
            return std::any_of(std::begin(kBlocks), std::end(kBlocks), [&](const char* v) { return n == v; });
        }

        struct Frame
        {
            std::string tag;
            bool bold = false, italic = false, strike = false, code = false;
            std::string link;
        };

        struct HtmlBuilder
        {
            Document* doc = nullptr;
            std::wstring baseDir;
            std::vector<Frame> frames;
            struct List { bool ordered; int next; };
            std::vector<List> lists;
            int quoteDepth = 0;
            int current = -1;
            int pre = 0;          // inside <pre>
            int table = -1;       // the outermost open table block
            int tableDepth = 0;
            bool cellOpen = false;
            bool pendingSpace = false;

            Span Style() const
            {
                Span s;
                for (const auto& f : frames)
                {
                    s.bold |= f.bold;
                    s.italic |= f.italic;
                    s.strike |= f.strike;
                    s.code |= f.code;
                    if (!f.link.empty())
                        s.link = f.link;
                }
                return s;
            }

            void EndBlock()
            {
                current = -1;
                pendingSpace = false;
            }

            std::vector<Span>* Target(bool create)
            {
                if (table >= 0)
                {
                    auto& t = doc->blocks[static_cast<size_t>(table)];
                    if (!create && (t.rows.empty() || t.rows.back().empty()))
                        return nullptr;
                    if (t.rows.empty())
                        t.rows.emplace_back();
                    if (t.rows.back().empty())
                        t.rows.back().emplace_back();
                    return &t.rows.back().back().spans;
                }
                if (current < 0)
                {
                    if (!create)
                        return nullptr;
                    Block b;
                    if (!lists.empty())
                    {
                        // Text after a nested list, still inside the outer item.
                        b.kind = BlockKind::ListItem;
                        b.level = static_cast<int>(lists.size()) - 1;
                        b.ordered = lists.back().ordered;
                        b.start = std::max(1, lists.back().next - 1);
                    }
                    else
                        b.kind = quoteDepth > 0 ? BlockKind::Quote : BlockKind::Paragraph;
                    doc->blocks.push_back(std::move(b));
                    current = static_cast<int>(doc->blocks.size()) - 1;
                }
                return &doc->blocks[static_cast<size_t>(current)].spans;
            }

            void NewBlock(BlockKind kind, int level = 0)
            {
                Block b;
                b.kind = kind;
                b.level = level;
                doc->blocks.push_back(std::move(b));
                current = static_cast<int>(doc->blocks.size()) - 1;
                pendingSpace = false;
            }

            void Text(std::string_view raw)
            {
                std::string text = DecodeEntities(raw);
                if (pre > 0)
                {
                    if (auto* spans = Target(true))
                    {
                        Span s = Style();
                        s.text = NormalizeNewlines(text);
                        spans->push_back(std::move(s));
                    }
                    return;
                }
                // Collapse whitespace the way a browser does. A space stays with the text it was
                // written in ("Plain " then "<b>bold</b>"), so it never moves into the next style.
                std::string collapsed;
                for (const char c : text)
                {
                    const bool ws = c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f';
                    if (ws)
                        pendingSpace = true;
                    else
                    {
                        if (pendingSpace && (!collapsed.empty() || (HasText() && !EndsWithSpace())))
                            collapsed.push_back(' ');
                        pendingSpace = false;
                        collapsed.push_back(c);
                    }
                }
                if (pendingSpace && (!collapsed.empty() || (HasText() && !EndsWithSpace())))
                {
                    collapsed.push_back(' ');
                    pendingSpace = false;
                }
                if (collapsed.empty())
                    return;
                if (auto* spans = Target(true))
                {
                    Span s = Style();
                    s.text = std::move(collapsed);
                    spans->push_back(std::move(s));
                }
            }

            bool EndsWithSpace()
            {
                auto* spans = Target(false);
                return spans && !spans->empty() && !spans->back().text.empty() && spans->back().text.back() == ' ';
            }

            bool HasText()
            {
                auto* spans = Target(false);
                if (!spans || spans->empty())
                    return false;
                return !spans->back().lineBreak;
            }

            void Break()
            {
                if (auto* spans = Target(true))
                {
                    Span s;
                    s.lineBreak = true;
                    spans->push_back(s);
                }
                pendingSpace = false;
            }

            void PopFrame(const std::string& tag)
            {
                for (size_t k = frames.size(); k-- > 0;)
                    if (frames[k].tag == tag)
                    {
                        frames.resize(k);
                        return;
                    }
            }
        };

        void ApplyStyleAttr(Frame& f, const std::string& style)
        {
            const auto s = AsciiLower(style);
            const auto has = [&](const char* needle) { return s.find(needle) != std::string::npos; };
            if (has("font-weight:bold") || has("font-weight: bold") || has("font-weight:700") || has("font-weight: 700") ||
                has("font-weight:600") || has("font-weight: 600") || has("font-weight:800") || has("font-weight:900"))
                f.bold = true;
            if (has("font-style:italic") || has("font-style: italic"))
                f.italic = true;
            if (has("line-through"))
                f.strike = true;
            if (has("monospace"))
                f.code = true;
        }
    }

    void ReadHtml(std::string_view s, const std::wstring& baseDir, Document& doc)
    {
        HtmlBuilder b;
        b.doc = &doc;
        b.baseDir = baseDir;
        size_t i = 0;
        std::string skipUntil;   // inside <script>, <style>, <head>...: skip to this closing tag
        bool inTitle = false;
        std::string title;
        while (i < s.size())
        {
            if (s[i] != '<')
            {
                const auto next = s.find('<', i);
                const auto end = next == std::string_view::npos ? s.size() : next;
                if (inTitle)
                    title += DecodeEntities(s.substr(i, end - i));
                else if (skipUntil.empty())
                    b.Text(s.substr(i, end - i));
                i = end;
                continue;
            }
            if (s.substr(i, 4) == "<!--")
            {
                const auto end = s.find("-->", i + 4);
                i = end == std::string_view::npos ? s.size() : end + 3;
                continue;
            }
            if (i + 1 < s.size() && (s[i + 1] == '!' || s[i + 1] == '?'))
            {
                const auto end = s.find('>', i);
                i = end == std::string_view::npos ? s.size() : end + 1;
                continue;
            }
            Tag tag;
            size_t j = i;
            if (!ParseTag(s, j, tag))
            {
                if (skipUntil.empty() && !inTitle)
                    b.Text("<");
                ++i;
                continue;
            }
            i = j;
            const auto& n = tag.name;
            if (!skipUntil.empty())
            {
                if (tag.closing && n == skipUntil)
                    skipUntil.clear();
                continue;
            }
            if (n == "title")
            {
                inTitle = !tag.closing;
                continue;
            }
            if (!tag.closing && (n == "script" || n == "style" || n == "noscript" || n == "template" || n == "svg" || n == "iframe" ||
                                 n == "object" || n == "select" || n == "textarea" || n == "math"))
            {
                if (!tag.selfClosing)
                    skipUntil = n;
                continue;
            }
            if (n == "head" || n == "meta" || n == "link" || n == "base")
                continue;

            const bool open = !tag.closing;
            const bool inCell = b.table >= 0;
            if (n.size() == 2 && n[0] == 'h' && n[1] >= '1' && n[1] <= '6')
            {
                if (inCell)
                {
                    if (!open)
                        b.Break();
                    continue;
                }
                if (open)
                    b.NewBlock(BlockKind::Heading, n[1] - '0');
                else
                    b.EndBlock();
                continue;
            }
            if (n == "br")
            {
                b.Break();
                continue;
            }
            if (n == "hr")
            {
                if (!inCell)
                {
                    b.NewBlock(BlockKind::Rule);
                    b.EndBlock();
                }
                continue;
            }
            if (n == "img")
            {
                const int index = LoadImageRef(tag.Attr("src"), b.baseDir, doc);
                const auto alt = tag.Attr("alt");
                if (index >= 0 || !alt.empty())
                    if (auto* spans = b.Target(true))
                    {
                        Span sp = b.Style();
                        sp.text = alt;
                        sp.image = index;
                        spans->push_back(std::move(sp));
                    }
                continue;
            }
            if (n == "ul" || n == "ol")
            {
                if (inCell)
                    continue;
                if (open)
                {
                    int start = 1;
                    if (const auto st = tag.Attr("start"); !st.empty())
                        start = std::max(1, std::atoi(st.c_str()));
                    b.lists.push_back({ n == "ol", start });
                }
                else if (!b.lists.empty())
                    b.lists.pop_back();
                b.EndBlock();
                continue;
            }
            if (n == "li")
            {
                if (inCell)
                {
                    if (open)
                        b.Break();
                    continue;
                }
                if (open)
                {
                    if (b.lists.empty())
                        b.lists.push_back({ false, 1 });   // a stray <li>
                    b.NewBlock(BlockKind::ListItem, static_cast<int>(b.lists.size()) - 1);
                    auto& item = doc.blocks.back();
                    item.ordered = b.lists.back().ordered;
                    if (const auto v = tag.Attr("value"); !v.empty())
                        b.lists.back().next = std::max(1, std::atoi(v.c_str()));
                    item.start = b.lists.back().next++;
                }
                else
                    b.EndBlock();
                continue;
            }
            if (n == "blockquote")
            {
                b.quoteDepth += open ? 1 : (b.quoteDepth > 0 ? -1 : 0);
                b.EndBlock();
                continue;
            }
            if (n == "pre")
            {
                if (inCell)
                    continue;
                if (open)
                {
                    b.NewBlock(BlockKind::Code);
                    ++b.pre;
                    // A newline right after <pre> is not content.
                    if (i < s.size() && s[i] == '\n')
                        ++i;
                    else if (i + 1 < s.size() && s[i] == '\r' && s[i + 1] == '\n')
                        i += 2;
                }
                else if (b.pre > 0)
                {
                    --b.pre;
                    b.EndBlock();
                }
                continue;
            }
            if (n == "table")
            {
                if (open)
                {
                    if (b.tableDepth++ == 0)
                    {
                        b.NewBlock(BlockKind::Table);
                        b.table = b.current;
                        b.current = -1;
                    }
                }
                else if (b.tableDepth > 0 && --b.tableDepth == 0)
                {
                    b.table = -1;
                    b.EndBlock();
                }
                continue;
            }
            if (b.table >= 0 && b.tableDepth == 1)
            {
                auto& t = doc.blocks[static_cast<size_t>(b.table)];
                if (n == "tr" && open)
                {
                    t.rows.emplace_back();
                    b.pendingSpace = false;
                    continue;
                }
                if ((n == "td" || n == "th") && open)
                {
                    if (t.rows.empty())
                        t.rows.emplace_back();
                    t.rows.back().emplace_back();
                    if (n == "th" && t.rows.size() == 1)
                        t.headerRow = true;
                    b.pendingSpace = false;
                    continue;
                }
                if (n == "thead" && open)
                {
                    t.headerRow = true;
                    continue;
                }
                if (n == "tr" || n == "td" || n == "th" || n == "thead" || n == "tbody" || n == "tfoot" || n == "colgroup" || n == "col")
                    continue;
            }
            if (IsBlockTag(n))
            {
                if (inCell)
                {
                    if (!open && (n == "p" || n == "div"))
                        b.Break();
                    continue;
                }
                b.EndBlock();
                // Block-level frames still carry inline styles (<div style="font-weight:bold">).
            }

            // Inline formatting.
            if (open && !tag.selfClosing && !IsVoid(n))
            {
                Frame f;
                f.tag = n;
                if (n == "b" || n == "strong")
                    f.bold = true;
                else if (n == "i" || n == "em" || n == "cite" || n == "var" || n == "dfn")
                    f.italic = true;
                else if (n == "s" || n == "strike" || n == "del")
                    f.strike = true;
                else if (n == "code" || n == "kbd" || n == "samp" || n == "tt")
                    f.code = b.pre == 0;
                else if (n == "a")
                {
                    auto href = tag.Attr("href");
                    if (!AsciiLower(href).starts_with("javascript:"))
                        f.link = std::move(href);
                }
                if (const auto style = tag.Attr("style"); !style.empty())
                    ApplyStyleAttr(f, style);
                b.frames.push_back(std::move(f));
            }
            else if (tag.closing)
                b.PopFrame(n);
        }
        if (doc.title.empty())
            doc.title = std::string(Trim(title));

        // Tidy: trim spaces at the ends of blocks and cells, then merge equal spans.
        const auto tidy = [](std::vector<Span>& spans) {
            NormalizeSpans(spans);
            while (!spans.empty() && spans.back().lineBreak)
                spans.pop_back();
            for (size_t k = 0; k < spans.size(); ++k)
                if (spans[k].image < 0 && !spans[k].lineBreak)
                {
                    if (k == 0 || spans[k - 1].lineBreak)
                        spans[k].text.erase(0, spans[k].text.find_first_not_of(' '));
                    if (k + 1 == spans.size() || spans[k + 1].lineBreak)
                        while (!spans[k].text.empty() && spans[k].text.back() == ' ')
                            spans[k].text.pop_back();
                }
            NormalizeSpans(spans);
        };
        std::vector<Block> kept;
        for (auto& block : doc.blocks)
        {
            if (block.kind == BlockKind::Code)
            {
                NormalizeSpans(block.spans);
                auto text = PlainText(block.spans);
                while (!text.empty() && text.back() == '\n')
                    text.pop_back();
                block.spans.clear();
                Span sp;
                sp.text = std::move(text);
                block.spans.push_back(std::move(sp));
            }
            else
                tidy(block.spans);
            for (auto& row : block.rows)
                for (auto& cell : row)
                    tidy(cell.spans);
            const bool empty = block.kind != BlockKind::Rule && block.kind != BlockKind::Table && block.kind != BlockKind::ListItem &&
                               block.spans.empty();
            if (block.kind == BlockKind::Table && block.rows.empty())
                continue;
            if (!empty)
                kept.push_back(std::move(block));
        }
        doc.blocks = std::move(kept);
    }

    // ---- Writer ---------------------------------------------------------------------------------------

    namespace
    {
        std::string InlineHtml(const Document& doc, const std::vector<Span>& spans, const HtmlOptions& o)
        {
            std::string out;
            for (const auto& s : spans)
            {
                if (s.lineBreak)
                {
                    out += o.xhtml ? "<br/>" : "<br>";
                    continue;
                }
                std::string inner;
                if (s.image >= 0 && static_cast<size_t>(s.image) < doc.images.size())
                {
                    const auto& img = doc.images[static_cast<size_t>(s.image)];
                    inner = "<img src=\"" + XmlEscape(o.imageDir + img.name) + "\" alt=\"" + XmlEscape(s.text) + "\"";
                    if (img.width > 0 && img.height > 0)
                        inner += std::format(" width=\"{}\" height=\"{}\"", img.width, img.height);
                    inner += o.xhtml ? "/>" : ">";
                }
                else
                    inner = XmlEscape(s.text);
                if (s.code)
                    inner = "<code>" + inner + "</code>";
                if (s.strike)
                    inner = "<del>" + inner + "</del>";
                if (s.italic)
                    inner = "<em>" + inner + "</em>";
                if (s.bold)
                    inner = "<strong>" + inner + "</strong>";
                if (!s.link.empty())
                    inner = "<a href=\"" + XmlEscape(s.link) + "\">" + inner + "</a>";
                out += inner;
            }
            return out;
        }

        const char* kCss = R"(body { font-family: "Segoe UI", system-ui, sans-serif; line-height: 1.55; max-width: 46em; margin: 2em auto; padding: 0 1em; color: #1b1b1b; }
h1, h2, h3, h4, h5, h6 { line-height: 1.25; margin: 1.4em 0 0.5em; }
pre { background: #f4f4f4; padding: 0.8em 1em; overflow-x: auto; }
code { font-family: Consolas, "Cascadia Mono", monospace; }
blockquote { border-left: 3px solid #ccc; margin: 1em 0; padding: 0 1em; color: #444; }
table { border-collapse: collapse; margin: 1em 0; }
th, td { border: 1px solid #ccc; padding: 0.3em 0.6em; vertical-align: top; text-align: left; }
th { background: #f4f4f4; }
img { max-width: 100%; height: auto; }
)";
    }

    std::string WriteHtmlBlocks(const Document& doc, size_t first, size_t last, const HtmlOptions& o, int headingId)
    {
        std::string out;
        struct Open { bool ordered; };
        std::vector<Open> lists;   // open <ul>/<ol>, each with an open <li>
        bool inQuote = false;
        const auto closeLists = [&](size_t keep) {
            while (lists.size() > keep)
            {
                out += lists.back().ordered ? "</li></ol>\n" : "</li></ul>\n";
                lists.pop_back();
            }
        };
        for (size_t bi = first; bi < last && bi < doc.blocks.size(); ++bi)
        {
            const auto& b = doc.blocks[bi];
            if (b.kind != BlockKind::ListItem)
                closeLists(0);
            if (b.kind != BlockKind::Quote && inQuote)
            {
                out += "</blockquote>\n";
                inQuote = false;
            }
            switch (b.kind)
            {
            case BlockKind::Heading:
            {
                const int level = std::clamp(b.level, 1, 6);
                out += std::format("<h{} id=\"h{}\">{}</h{}>\n", level, headingId++, InlineHtml(doc, b.spans, o), level);
                break;
            }
            case BlockKind::Paragraph:
                out += "<p>" + InlineHtml(doc, b.spans, o) + "</p>\n";
                break;
            case BlockKind::Quote:
                if (!inQuote)
                {
                    out += "<blockquote>\n";
                    inQuote = true;
                }
                out += "<p>" + InlineHtml(doc, b.spans, o) + "</p>\n";
                break;
            case BlockKind::ListItem:
            {
                const size_t level = static_cast<size_t>(std::max(0, b.level));
                closeLists(level + 1);
                if (lists.size() == level + 1)
                {
                    if (lists.back().ordered != b.ordered)
                        closeLists(level);
                    else
                        out += "</li>\n";
                }
                while (lists.size() < level + 1)
                {
                    const bool ordered = lists.size() == level ? b.ordered : false;
                    if (ordered && b.start != 1)
                        out += std::format("<ol start=\"{}\">", b.start);
                    else
                        out += ordered ? "<ol>" : "<ul>";
                    lists.push_back({ ordered });
                    if (lists.size() < level + 1)
                        out += "<li>";
                }
                out += "<li>" + InlineHtml(doc, b.spans, o);
                break;
            }
            case BlockKind::Code:
                out += "<pre><code>" + XmlEscape(PlainText(b.spans)) + "</code></pre>\n";
                break;
            case BlockKind::Rule:
                out += o.xhtml ? "<hr/>\n" : "<hr>\n";
                break;
            case BlockKind::Table:
            {
                if (b.rows.empty())
                    break;
                out += "<table>\n";
                size_t r = 0;
                if (b.headerRow)
                {
                    out += "<thead><tr>";
                    for (const auto& c : b.rows[0])
                        out += "<th>" + InlineHtml(doc, c.spans, o) + "</th>";
                    out += "</tr></thead>\n";
                    r = 1;
                }
                out += "<tbody>\n";
                for (; r < b.rows.size(); ++r)
                {
                    out += "<tr>";
                    for (const auto& c : b.rows[r])
                        out += "<td>" + InlineHtml(doc, c.spans, o) + "</td>";
                    out += "</tr>\n";
                }
                out += "</tbody></table>\n";
                break;
            }
            }
        }
        closeLists(0);
        if (inQuote)
            out += "</blockquote>\n";
        return out;
    }

    std::string WriteHtml(const Document& doc, const HtmlOptions& o)
    {
        const auto body = WriteHtmlBlocks(doc, 0, doc.blocks.size(), o, 1);
        if (!o.standalone)
            return body;
        const auto title = XmlEscape(o.title.empty() ? TitleOf(doc, "Document") : o.title);
        const auto lang = o.language.empty() ? doc.language : o.language;
        std::string out;
        if (o.xhtml)
        {
            out += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n";
            out += "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"";
            if (!lang.empty())
                out += " lang=\"" + XmlEscape(lang) + "\" xml:lang=\"" + XmlEscape(lang) + "\"";
            out += ">\n<head>\n<meta charset=\"utf-8\"/>\n<title>" + title + "</title>\n";
        }
        else
        {
            out += "<!DOCTYPE html>\n<html";
            if (!lang.empty())
                out += " lang=\"" + XmlEscape(lang) + "\"";
            out += ">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>" + title + "</title>\n";
        }
        if (!o.cssHref.empty())
            out += "<link rel=\"stylesheet\" type=\"text/css\" href=\"" + XmlEscape(o.cssHref) + "\"" + (o.xhtml ? "/>\n" : ">\n");
        else
            out += std::string("<style>\n") + kCss + "</style>\n";
        out += "</head>\n<body>\n" + body + "</body>\n</html>\n";
        return out;
    }

    const char* DefaultCss() { return kCss; }
}
