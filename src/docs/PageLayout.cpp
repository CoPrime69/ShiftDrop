#include "docs/PageLayout.h"
#include "Utf.h"

#include <algorithm>
#include <format>
#include <numeric>
#include <utility>

namespace sd::docs
{
    namespace
    {
        bool IsCjk(wchar_t c)
        {
            return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF);
        }

        bool IsSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == 0x3000; }

        struct Piece
        {
            size_t frag = 0;
            std::wstring text;
            float width = 0;
        };

        // A unit of line breaking: spaces, an unbreakable group of pieces (one word, possibly in
        // several styles), or a hard break.
        struct Unit
        {
            enum Kind { Space, Group, Break } kind = Group;
            std::vector<Piece> pieces;
            float width = 0;
            size_t frag = 0;
        };

        std::vector<Unit> Tokenize(const std::vector<Fragment>& fragments, ITextMeasure& m, bool keepSpaces)
        {
            std::vector<Unit> units;
            bool glue = false;   // the next word piece joins the group before it (no space between)
            const auto addPiece = [&](size_t f, std::wstring text, bool space, bool breakable) {
                Piece p;
                p.frag = f;
                p.width = m.Width(text, fragments[f].font);
                p.text = std::move(text);
                if (space)
                {
                    if (!units.empty() && units.back().kind == Unit::Space && !keepSpaces)
                        return;   // runs of spaces collapse to one
                    Unit u;
                    u.kind = Unit::Space;
                    u.width = p.width;
                    u.frag = f;
                    u.pieces.push_back(std::move(p));
                    units.push_back(std::move(u));
                    glue = false;
                    return;
                }
                if (glue && !units.empty() && units.back().kind == Unit::Group && !breakable)
                {
                    units.back().width += p.width;
                    units.back().pieces.push_back(std::move(p));
                }
                else
                {
                    Unit u;
                    u.kind = Unit::Group;
                    u.width = p.width;
                    u.frag = f;
                    u.pieces.push_back(std::move(p));
                    units.push_back(std::move(u));
                }
                glue = !breakable;
            };
            for (size_t f = 0; f < fragments.size(); ++f)
            {
                const auto& frag = fragments[f];
                if (frag.lineBreak)
                {
                    Unit u;
                    u.kind = Unit::Break;
                    u.frag = f;
                    units.push_back(std::move(u));
                    glue = false;
                    continue;
                }
                if (frag.image >= 0)
                {
                    Unit u;
                    u.kind = Unit::Group;
                    u.frag = f;
                    Piece p;
                    p.frag = f;
                    p.width = frag.imageW;
                    u.pieces.push_back(p);
                    u.width = frag.imageW;
                    units.push_back(std::move(u));
                    glue = false;
                    continue;
                }
                std::wstring word;
                for (wchar_t c : frag.text)
                {
                    if (c == L'\r' || c == L'\n')
                        c = L' ';
                    if (IsSpace(c))
                    {
                        if (!word.empty())
                            addPiece(f, std::exchange(word, {}), false, false);
                        addPiece(f, c == L'\t' ? std::wstring(keepSpaces ? 4 : 1, L' ') : std::wstring(1, L' '), true, false);
                    }
                    else if (IsCjk(c))
                    {
                        if (!word.empty())
                            addPiece(f, std::exchange(word, {}), false, false);
                        glue = false;
                        addPiece(f, std::wstring(1, c), false, true);
                    }
                    else
                        word.push_back(c == 0xA0 ? L' ' : c);
                }
                if (!word.empty())
                    addPiece(f, std::move(word), false, false);
            }
            return units;
        }

        class LineBuilder
        {
        public:
            LineBuilder(const std::vector<Fragment>& fragments, ITextMeasure& m) : m_frags(fragments), m_measure(m) {}

            void Place(const Piece& p)
            {
                const auto& f = m_frags[p.frag];
                if (f.image >= 0)
                {
                    PageItem item;
                    item.kind = ItemKind::Image;
                    item.image = f.image;
                    item.x = m_line.width;
                    item.y = -f.imageH;
                    item.w = f.imageW;
                    item.h = f.imageH;
                    m_line.items.push_back(item);
                    m_line.ascent = std::max(m_line.ascent, f.imageH);
                    m_line.width += f.imageW;
                    m_hasContent = true;
                    return;
                }
                auto& items = m_line.items;
                if (!items.empty() && items.back().kind == ItemKind::Text && m_lastFrag < m_frags.size() &&
                    SameLook(m_frags[m_lastFrag], f))
                    items.back().text += p.text;
                else
                {
                    PageItem item;
                    item.kind = ItemKind::Text;
                    item.x = m_line.width;
                    item.text = p.text;
                    item.font = f.font;
                    item.color = f.color;
                    item.underline = f.underline;
                    items.push_back(std::move(item));
                }
                m_lastFrag = p.frag;
                Grow(f.font);
                m_line.width += p.width;
                m_hasContent = true;
            }

            void Grow(const FontStyle& font)
            {
                const auto fm = m_measure.Metrics(font);
                m_line.ascent = std::max(m_line.ascent, fm.ascent);
                m_line.descent = std::max(m_line.descent, fm.descent);
                m_line.gap = std::max(m_line.gap, fm.lineGap);
            }

            bool Empty() const { return !m_hasContent; }
            float Width() const { return m_line.width; }

            void Finish(std::vector<Line>& out, size_t fragForEmpty)
            {
                // Trailing spaces take no room at a line end.
                if (!m_line.items.empty() && m_line.items.back().kind == ItemKind::Text && !m_keepSpaces)
                {
                    auto& t = m_line.items.back().text;
                    size_t n = 0;
                    while (!t.empty() && t.back() == L' ')
                    {
                        t.pop_back();
                        ++n;
                    }
                    if (n)
                        m_line.width = m_line.items.back().x + m_measure.Width(t, m_line.items.back().font);
                }
                if (m_line.ascent + m_line.descent <= 0 && fragForEmpty < m_frags.size())
                    Grow(m_frags[fragForEmpty].font);
                out.push_back(std::move(m_line));
                m_line = Line{};
                m_hasContent = false;
                m_lastFrag = SIZE_MAX;
            }

            void KeepSpaces(bool keep) { m_keepSpaces = keep; }

        private:
            static bool SameLook(const Fragment& a, const Fragment& b)
            {
                return a.font == b.font && a.color == b.color && a.underline == b.underline && a.image < 0 && b.image < 0;
            }

            const std::vector<Fragment>& m_frags;
            ITextMeasure& m_measure;
            Line m_line;
            bool m_hasContent = false;
            bool m_keepSpaces = false;
            size_t m_lastFrag = SIZE_MAX;
        };
    }

    std::vector<Line> BreakLines(const std::vector<Fragment>& fragments, float maxWidth, ITextMeasure& m, bool keepSpaces)
    {
        std::vector<Line> lines;
        if (fragments.empty())
            return lines;
        const auto units = Tokenize(fragments, m, keepSpaces);
        LineBuilder line(fragments, m);
        line.KeepSpaces(keepSpaces);
        std::vector<const Unit*> pendingSpaces;
        size_t lastFrag = 0;
        for (const auto& u : units)
        {
            lastFrag = u.frag;
            if (u.kind == Unit::Break)
            {
                line.Finish(lines, u.frag);
                pendingSpaces.clear();
                continue;
            }
            if (u.kind == Unit::Space)
            {
                if (!line.Empty() || keepSpaces)
                    pendingSpaces.push_back(&u);
                continue;
            }
            float spaceWidth = 0;
            for (const auto* s : pendingSpaces)
                spaceWidth += s->width;
            if (!line.Empty() && line.Width() + spaceWidth + u.width > maxWidth)
            {
                line.Finish(lines, u.frag);
                pendingSpaces.clear();
                spaceWidth = 0;
            }
            if (keepSpaces || !line.Empty())
                for (const auto* s : pendingSpaces)
                    for (const auto& p : s->pieces)
                        line.Place(p);
            pendingSpaces.clear();
            if (line.Width() + u.width <= maxWidth || (u.pieces.size() == 1 && fragments[u.pieces[0].frag].image >= 0))
            {
                for (const auto& p : u.pieces)
                    line.Place(p);
                continue;
            }
            // A word wider than the line: split it at characters, as many as fit on each line.
            for (const auto& p : u.pieces)
            {
                std::wstring_view rest = p.text;
                while (!rest.empty())
                {
                    const float room = maxWidth - line.Width();
                    size_t lo = 0, hi = rest.size();
                    while (lo < hi)
                    {
                        const size_t mid = (lo + hi + 1) / 2;
                        if (m.Width(rest.substr(0, mid), fragments[p.frag].font) <= room)
                            lo = mid;
                        else
                            hi = mid - 1;
                    }
                    if (lo == 0)
                    {
                        if (!line.Empty())
                        {
                            line.Finish(lines, p.frag);
                            continue;
                        }
                        lo = 1;   // not even one character fits: place it anyway
                    }
                    Piece part;
                    part.frag = p.frag;
                    part.text = std::wstring(rest.substr(0, lo));
                    part.width = m.Width(part.text, fragments[p.frag].font);
                    line.Place(part);
                    rest.remove_prefix(lo);
                    if (!rest.empty())
                        line.Finish(lines, p.frag);
                }
            }
        }
        if (!line.Empty() || lines.empty() || fragments.back().lineBreak)
            line.Finish(lines, lastFrag);
        return lines;
    }

    std::vector<float> FitColumns(const std::vector<float>& natural, const std::vector<float>& minimum, float available)
    {
        const size_t n = natural.size();
        std::vector<float> w(n, 0);
        if (n == 0)
            return w;
        const float total = std::accumulate(natural.begin(), natural.end(), 0.f);
        if (total <= available)
            return natural;
        const float share = available / static_cast<float>(n);
        float floors = 0, extra = 0;
        for (size_t i = 0; i < n; ++i)
        {
            w[i] = std::min(i < minimum.size() ? minimum[i] : 0.f, share);
            floors += w[i];
            extra += std::max(0.f, natural[i] - w[i]);
        }
        const float room = std::max(0.f, available - floors);
        for (size_t i = 0; i < n; ++i)
            if (extra > 0)
                w[i] += room * std::max(0.f, natural[i] - w[i]) / extra;
        return w;
    }

    namespace
    {
        constexpr uint32_t kHeadingColor = 0x1F3864;
        constexpr uint32_t kLinkColor = 0x0563C1;
        constexpr uint32_t kGrey = 0x595959;
        constexpr uint32_t kBorder = 0xBFBFBF;
        constexpr uint32_t kShade = 0xF2F2F2;

        class DocumentLayout
        {
        public:
            DocumentLayout(const Document& doc, const LayoutOptions& o, ITextMeasure& m) : m_doc(doc), m_o(o), m_m(m)
            {
                m_left = o.page.margin;
                m_right = o.page.width - o.page.margin;
                m_top = o.page.margin;
                m_bottom = o.page.height - o.page.margin - (o.page.pageNumbers ? o.page.footer : 0);
                m_pages.emplace_back();
                m_y = m_top;
            }

            std::vector<Page> Run()
            {
                for (size_t i = 0; i < m_doc.blocks.size(); ++i)
                    Block(i);
                if (m_o.page.pageNumbers)
                    for (size_t p = 0; p < m_pages.size(); ++p)
                    {
                        PageItem n;
                        n.kind = ItemKind::Text;
                        n.text = std::to_wstring(p + 1);
                        n.font.size = 9;
                        n.color = kGrey;
                        n.x = m_o.page.width / 2 - m_m.Width(n.text, n.font) / 2;
                        n.y = m_o.page.height - m_o.page.margin / 2;
                        m_pages[p].items.push_back(std::move(n));
                    }
                return std::move(m_pages);
            }

        private:
            float Width() const { return m_right - m_left; }
            float PageRoom() const { return m_bottom - m_top; }

            void NewPage()
            {
                CloseDecoration();
                m_pages.emplace_back();
                m_y = m_top;
                OpenDecoration();
            }

            void Space(float pts)
            {
                if (m_y > m_top)
                    m_y = std::min(m_y + pts, m_bottom);
            }

            // ---- Fragments ----------------------------------------------------------------------

            std::vector<Fragment> Fragments(const std::vector<Span>& spans, const FontStyle& base, uint32_t color, float maxImageW) const
            {
                std::vector<Fragment> out;
                for (const auto& s : spans)
                {
                    Fragment f;
                    f.font = base;
                    f.color = color;
                    if (s.lineBreak)
                    {
                        f.lineBreak = true;
                        out.push_back(std::move(f));
                        continue;
                    }
                    if (s.image >= 0 && static_cast<size_t>(s.image) < m_doc.images.size())
                    {
                        const auto& img = m_doc.images[static_cast<size_t>(s.image)];
                        // Pixels at 96 DPI to points, shrunk to the column and to most of a page.
                        float w = (img.width > 0 ? img.width : 320) * 0.75f, h = (img.height > 0 ? img.height : 240) * 0.75f;
                        const float scale = std::min({ 1.f, maxImageW / w, PageRoom() * 0.9f / h });
                        f.image = s.image;
                        f.imageW = w * scale;
                        f.imageH = h * scale;
                        out.push_back(std::move(f));
                        continue;
                    }
                    f.text = Widen(s.text);
                    f.font.bold = base.bold || s.bold;
                    f.font.italic = base.italic || s.italic;
                    if (s.code)
                    {
                        f.font.mono = true;
                        f.font.size = base.size * 0.92f;
                    }
                    if (!s.link.empty())
                    {
                        f.color = kLinkColor;
                        f.underline = true;
                    }
                    out.push_back(std::move(f));
                }
                if (out.empty())
                {
                    Fragment f;
                    f.font = base;
                    out.push_back(std::move(f));
                }
                return out;
            }

            // ---- Placing lines ------------------------------------------------------------------

            enum class Deco { None, Shade, Bar };

            void OpenDecoration()
            {
                if (m_deco == Deco::None)
                    return;
                m_decoPage = m_pages.size() - 1;
                m_decoIndex = m_pages.back().items.size();
                m_decoTop = m_y;
            }

            void CloseDecoration()
            {
                if (m_deco == Deco::None || m_decoPage >= m_pages.size())
                    return;
                auto& items = m_pages[m_decoPage].items;
                PageItem item;
                if (m_deco == Deco::Shade)
                {
                    item.kind = ItemKind::Rect;
                    item.x = m_decoLeft;
                    item.y = m_decoTop;
                    item.w = m_decoRight - m_decoLeft;
                    item.h = m_y - m_decoTop;
                    item.fill = kShade;
                    item.color = kNoColor;
                }
                else
                {
                    item.kind = ItemKind::Line;
                    item.x = m_decoLeft;
                    item.y = m_decoTop;
                    item.w = 0;
                    item.h = m_y - m_decoTop;
                    item.color = kBorder;
                    item.lineWidth = 2;
                }
                if (item.h > 0)
                    items.insert(items.begin() + static_cast<std::ptrdiff_t>(std::min(m_decoIndex, items.size())), item);
                m_decoPage = SIZE_MAX;
            }

            void StartDecoration(Deco d, float left, float right)
            {
                m_deco = d;
                m_decoLeft = left;
                m_decoRight = right;
                OpenDecoration();
            }

            void EndDecoration()
            {
                CloseDecoration();
                m_deco = Deco::None;
            }

            void PlaceLine(const Line& line, float left)
            {
                if (m_y + line.Height() > m_bottom && m_y > m_top)
                    NewPage();
                const float baseline = m_y + line.gap / 2 + line.ascent;
                auto& items = m_pages.back().items;
                for (auto item : line.items)
                {
                    item.x += left;
                    item.y = item.kind == ItemKind::Image ? baseline + item.y : baseline;
                    items.push_back(std::move(item));
                }
                m_y += line.Height();
            }

            void PlaceLines(const std::vector<Line>& lines, float left)
            {
                for (const auto& l : lines)
                    PlaceLine(l, left);
            }

            // ---- Blocks -------------------------------------------------------------------------

            FontStyle Body() const
            {
                FontStyle f;
                f.size = m_o.bodySize;
                return f;
            }

            void Block(size_t index)
            {
                const auto& b = m_doc.blocks[index];
                const float body = m_o.bodySize;
                switch (b.kind)
                {
                case BlockKind::Heading:
                {
                    static const float kScale[] = { 1, 1.8f, 1.45f, 1.22f, 1.09f, 1, 1 };
                    static const float kBefore[] = { 0, 18, 14, 12, 10, 8, 8 };
                    const int level = std::clamp(b.level, 1, 6);
                    FontStyle f = Body();
                    f.size = body * kScale[level];
                    f.bold = true;
                    f.italic = level == 6;
                    Space(kBefore[level]);
                    const auto lines = BreakLines(Fragments(b.spans, f, kHeadingColor, Width()), Width(), m_m);
                    // Keep a heading with the first lines after it.
                    float need = 0;
                    for (const auto& l : lines)
                        need += l.Height();
                    const auto bodyMetrics = m_m.Metrics(Body());
                    need += 2 * (bodyMetrics.ascent + bodyMetrics.descent + bodyMetrics.lineGap);
                    if (m_y + need > m_bottom && m_y > m_top && need < PageRoom())
                        NewPage();
                    PlaceLines(lines, m_left);
                    Space(6);
                    break;
                }
                case BlockKind::Paragraph:
                    PlaceLines(BreakLines(Fragments(b.spans, Body(), 0x000000, Width()), Width(), m_m), m_left);
                    Space(6);
                    break;
                case BlockKind::Quote:
                {
                    const float indent = 16;
                    FontStyle f = Body();
                    f.italic = true;
                    StartDecoration(Deco::Bar, m_left + 4, m_left + 4);
                    PlaceLines(BreakLines(Fragments(b.spans, f, 0x404040, Width() - indent), Width() - indent, m_m), m_left + indent);
                    EndDecoration();
                    Space(6);
                    break;
                }
                case BlockKind::ListItem:
                {
                    const float step = 18;
                    const int level = std::clamp(b.level, 0, 8);
                    std::wstring marker;
                    if (b.ordered)
                        marker = std::format(L"{}.", b.start);
                    else
                        marker = level % 3 == 0 ? L"•" : level % 3 == 1 ? L"◦" : L"▪";
                    const FontStyle f = Body();
                    const float markerW = m_m.Width(marker, f);
                    const float textLeft = m_left + step * static_cast<float>(level) + std::max(step, markerW + 6);
                    const float width = m_right - textLeft;
                    const auto lines = BreakLines(Fragments(b.spans, f, 0x000000, width), width, m_m);
                    if (!lines.empty())
                    {
                        if (m_y + lines[0].Height() > m_bottom && m_y > m_top)
                            NewPage();
                        PageItem item;
                        item.kind = ItemKind::Text;
                        item.text = marker;
                        item.font = f;
                        item.x = textLeft - 6 - markerW;
                        item.y = m_y + lines[0].gap / 2 + lines[0].ascent;
                        m_pages.back().items.push_back(std::move(item));
                    }
                    PlaceLines(lines, textLeft);
                    // Items of one list sit close; the list as a whole gets paragraph spacing.
                    const bool last = index + 1 >= m_doc.blocks.size() || m_doc.blocks[index + 1].kind != BlockKind::ListItem;
                    Space(last ? 6 : 2);
                    break;
                }
                case BlockKind::Code:
                {
                    FontStyle f = Body();
                    f.mono = true;
                    f.size = body * 0.86f;
                    const float pad = 6;
                    std::vector<Fragment> frags;
                    const auto text = Widen(PlainText(b.spans));
                    size_t start = 0;
                    while (start <= text.size())
                    {
                        const auto nl = text.find(L'\n', start);
                        Fragment fr;
                        fr.font = f;
                        fr.text = text.substr(start, (nl == std::wstring::npos ? text.size() : nl) - start);
                        frags.push_back(std::move(fr));
                        if (nl == std::wstring::npos)
                            break;
                        Fragment br;
                        br.font = f;
                        br.lineBreak = true;
                        frags.push_back(std::move(br));
                        start = nl + 1;
                    }
                    Space(2);
                    StartDecoration(Deco::Shade, m_left, m_right);
                    m_y += pad;
                    PlaceLines(BreakLines(frags, Width() - 2 * pad, m_m, true), m_left + pad);
                    m_y = std::min(m_y + pad, m_bottom);
                    EndDecoration();
                    Space(8);
                    break;
                }
                case BlockKind::Rule:
                {
                    if (m_y + 12 > m_bottom)
                        NewPage();
                    PageItem item;
                    item.kind = ItemKind::Line;
                    item.x = m_left;
                    item.y = m_y + 6;
                    item.w = Width();
                    item.h = 0;
                    item.color = kBorder;
                    item.lineWidth = 0.75f;
                    m_pages.back().items.push_back(item);
                    m_y += 12;
                    break;
                }
                case BlockKind::Table:
                    Table(b);
                    Space(8);
                    break;
                }
            }

            struct CellLayout
            {
                std::vector<Line> lines;
                float height = 0;
            };

            void Table(const docs::Block& b)
            {
                size_t cols = 0;
                for (const auto& r : b.rows)
                    cols = std::max(cols, r.size());
                if (cols == 0)
                    return;
                const float pad = 4;
                FontStyle f = Body();
                f.size = std::max(7.f, m_o.bodySize - 1);
                FontStyle head = f;
                head.bold = true;

                // Natural and minimum widths per column.
                std::vector<float> natural(cols, 2 * pad + 8), minimum(cols, 2 * pad + 8);
                for (size_t r = 0; r < b.rows.size(); ++r)
                    for (size_t c = 0; c < b.rows[r].size(); ++c)
                    {
                        const auto& font = r == 0 && b.headerRow ? head : f;
                        const auto frags = Fragments(b.rows[r][c].spans, font, 0, Width());
                        for (const auto& l : BreakLines(frags, 1e9f, m_m))
                            natural[c] = std::max(natural[c], l.width + 2 * pad);
                        for (const auto& fr : frags)
                        {
                            if (fr.image >= 0)
                                minimum[c] = std::max(minimum[c], std::min(fr.imageW, Width() / 2) + 2 * pad);
                            size_t s = 0;
                            while (s < fr.text.size())
                            {
                                auto e = fr.text.find(L' ', s);
                                if (e == std::wstring::npos)
                                    e = fr.text.size();
                                if (e > s)
                                    minimum[c] = std::max(minimum[c], m_m.Width(std::wstring_view(fr.text).substr(s, e - s), fr.font) + 2 * pad);
                                s = e + 1;
                            }
                        }
                    }
                const auto widths = FitColumns(natural, minimum, Width());

                const auto layoutRow = [&](size_t r) {
                    std::vector<CellLayout> cells(cols);
                    float h = 0;
                    for (size_t c = 0; c < cols; ++c)
                    {
                        if (c < b.rows[r].size())
                        {
                            const auto& font = r == 0 && b.headerRow ? head : f;
                            const float inner = std::max(1.f, widths[c] - 2 * pad);
                            cells[c].lines = BreakLines(Fragments(b.rows[r][c].spans, font, 0x000000, inner), inner, m_m);
                        }
                        for (const auto& l : cells[c].lines)
                            cells[c].height += l.Height();
                        h = std::max(h, cells[c].height);
                    }
                    if (h <= 0)
                    {
                        const auto fm = m_m.Metrics(f);
                        h = fm.ascent + fm.descent + fm.lineGap;
                    }
                    return std::pair{ cells, h + 2 * pad };
                };

                const auto drawRow = [&](const std::vector<CellLayout>& cells, float rowH, bool header) {
                    // A row taller than a page is cut at the page's end.
                    rowH = std::min(rowH, PageRoom());
                    float x = m_left;
                    auto& items = m_pages.back().items;
                    for (size_t c = 0; c < cols; ++c)
                    {
                        PageItem box;
                        box.kind = ItemKind::Rect;
                        box.x = x;
                        box.y = m_y;
                        box.w = widths[c];
                        box.h = rowH;
                        box.color = kBorder;
                        box.fill = header ? kShade : kNoColor;
                        items.push_back(box);
                        float y = m_y + pad;
                        for (const auto& l : cells[c].lines)
                        {
                            if (y + l.Height() > m_y + rowH - pad + 0.01f)
                                break;
                            const float baseline = y + l.gap / 2 + l.ascent;
                            for (auto item : l.items)
                            {
                                item.x += x + pad;
                                item.y = item.kind == ItemKind::Image ? baseline + item.y : baseline;
                                items.push_back(std::move(item));
                            }
                            y += l.Height();
                        }
                        x += widths[c];
                    }
                    m_y += rowH;
                };

                std::vector<CellLayout> headerCells;
                float headerH = 0;
                if (b.headerRow && !b.rows.empty())
                    std::tie(headerCells, headerH) = layoutRow(0);
                for (size_t r = 0; r < b.rows.size(); ++r)
                {
                    auto [cells, h] = r == 0 && b.headerRow ? std::pair{ headerCells, headerH } : layoutRow(r);
                    if (m_y + std::min(h, PageRoom()) > m_bottom && m_y > m_top)
                    {
                        NewPage();
                        if (b.headerRow && r > 0 && m_o.repeatTableHeader && headerH + h <= PageRoom())
                            drawRow(headerCells, headerH, true);
                    }
                    drawRow(cells, h, r == 0 && b.headerRow);
                }
            }

            const Document& m_doc;
            const LayoutOptions& m_o;
            ITextMeasure& m_m;
            std::vector<Page> m_pages;
            float m_left = 0, m_right = 0, m_top = 0, m_bottom = 0, m_y = 0;
            Deco m_deco = Deco::None;
            size_t m_decoPage = SIZE_MAX, m_decoIndex = 0;
            float m_decoTop = 0, m_decoLeft = 0, m_decoRight = 0;
        };
    }

    std::vector<Page> LayoutDocument(const Document& doc, const LayoutOptions& options, ITextMeasure& measure)
    {
        return DocumentLayout(doc, options, measure).Run();
    }
}
