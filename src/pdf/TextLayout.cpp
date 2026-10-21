#include "pdf/TextLayout.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstddef>
#include <map>

namespace sd::pdf
{
    namespace
    {
        bool IsSpace(wchar_t c) { return std::iswspace(c) || c == 0xA0 || c == 0x3000; }

        std::wstring Collapse(std::wstring_view s)
        {
            std::wstring out;
            bool space = false;
            for (const wchar_t c : s)
            {
                if (IsSpace(c))
                {
                    space = !out.empty();
                    continue;
                }
                if (space)
                    out += L' ';
                space = false;
                out += c;
            }
            return out;
        }

        size_t NonSpace(std::wstring_view s)
        {
            return static_cast<size_t>(std::count_if(s.begin(), s.end(), [](wchar_t c) { return !IsSpace(c); }));
        }

        // "• item", "◦ item", "- item", "* item": the text after the marker, or empty if not a bullet.
        bool StripBullet(const std::wstring& text, std::wstring& rest)
        {
            if (text.empty())
                return false;
            static constexpr std::wstring_view kGlyphs = L"•◦▪‣●○■□⁃·";
            const wchar_t c = text[0];
            const bool glyph = kGlyphs.find(c) != std::wstring_view::npos;
            const bool ascii = (c == L'-' || c == L'*' || c == L'–') && text.size() > 1 && text[1] == L' ';
            if (!glyph && !ascii)
                return false;
            rest = Collapse(std::wstring_view(text).substr(1));
            return !rest.empty();
        }

        struct Block
        {
            std::vector<const TextLine*> lines;
            double size = 0;
            bool bullet = false;
        };

        // The usual line spacing: the lower quartile of the gaps, since paragraph breaks only ever
        // add to it (a median drifts up on pages with many short paragraphs).
        double TypicalGap(std::vector<double> v)
        {
            if (v.empty())
                return 0;
            std::sort(v.begin(), v.end());
            return v[(v.size() - 1) / 4];
        }

        bool SimilarSize(double a, double b) { return a > 0 && b > 0 && std::max(a, b) / std::min(a, b) <= 1.15; }

        // Paragraphs: a new one where the line spacing opens up, the font size changes, the text
        // moves back up the page (a new column) or a bullet starts.
        std::vector<Block> Blocks(const PageLines& lines)
        {
            std::vector<double> gaps;
            for (size_t i = 1; i < lines.size(); ++i)
            {
                const double g = lines[i - 1].y - lines[i].y;
                if (g > 0 && g < 3 * lines[i].size && SimilarSize(lines[i - 1].size, lines[i].size))
                    gaps.push_back(g);
            }
            const double typical = TypicalGap(gaps);

            std::vector<Block> blocks;
            std::wstring rest;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                const auto& line = lines[i];
                const bool bullet = StripBullet(line.text, rest);
                bool fresh = blocks.empty() || bullet;
                if (!fresh)
                {
                    const auto& prev = lines[i - 1];
                    const double g = prev.y - line.y;
                    const double limit = typical > 0 ? typical * 1.35 : 1.8 * std::max(prev.size, line.size);
                    fresh = g <= 0.1 * line.size || g > limit || !SimilarSize(prev.size, line.size);
                }
                if (fresh)
                {
                    blocks.push_back(Block{ {}, line.size, bullet });
                }
                blocks.back().lines.push_back(&line);
                blocks.back().size = std::max(blocks.back().size, line.size);
            }
            return blocks;
        }

        // One paragraph's lines as one line: "exam-" + "ple" -> "example", otherwise a space.
        std::wstring Reflow(const Block& block)
        {
            std::wstring out;
            for (size_t i = 0; i < block.lines.size(); ++i)
            {
                std::wstring text = block.lines[i]->text;
                if (i == 0 && block.bullet)
                {
                    std::wstring rest;
                    if (StripBullet(text, rest))
                        text = rest;
                }
                if (out.empty())
                {
                    out = text;
                    continue;
                }
                const bool hyphen = out.size() >= 2 && out.back() == L'-' && IsCharAlphaW(out[out.size() - 2]) && !text.empty() &&
                                    IsCharLowerW(text[0]);
                if (hyphen)
                    out.pop_back();
                else
                    out += L' ';
                out += text;
            }
            return out;
        }

        // The body text size: the size most characters are set in (rounded to half a point).
        double BodySize(const std::vector<PageLines>& pages)
        {
            std::map<long, size_t> chars;
            for (const auto& page : pages)
                for (const auto& line : page)
                    chars[std::lround(line.size * 2)] += NonSpace(line.text);
            long best = 0;
            size_t most = 0;
            for (const auto& [size, n] : chars)
                if (n > most)
                    most = n, best = size;
            return best / 2.0;
        }

        std::wstring Escape(const std::wstring& paragraph)
        {
            if (!paragraph.empty() && (paragraph[0] == L'#' || paragraph[0] == L'>' || (paragraph[0] == L'+' && paragraph.size() > 1 && paragraph[1] == L' ')))
                return L"\\" + paragraph;
            return paragraph;
        }

        double Median(std::vector<double> v)
        {
            if (v.empty())
                return 0;
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
            return v[v.size() / 2];
        }

        // Roughly where a line's letters reach up to: the baseline plus most of the font size.
        double TopOf(const TextLine& line) { return line.y + 0.75 * line.size; }

        double TopOf(const Block& block)
        {
            double top = -1e300;
            for (const auto* line : block.lines)
                top = std::max(top, TopOf(*line));
            return top;
        }

        // A stretch of the reading order: lines that may share paragraphs, and the pictures among them.
        struct Run
        {
            PageLines lines;
            std::vector<PagePicture> pictures;
        };

        constexpr double kEdge = 0.01;       // points: on the gutter's edge still counts as beside it
        constexpr int kMaxColumnDepth = 2;   // a column may split once more (three or four columns)

        // The page (or one column of it) as runs in reading order; see ReadingOrder.
        void Flow(const PageLines& lines, const std::vector<PagePicture>& pictures, int depth, std::vector<Run>& out)
        {
            double gutterLeft = 0, gutterRight = 0;
            if (depth > kMaxColumnDepth || !FindGutter(lines, gutterLeft, gutterRight))
            {
                if (!lines.empty() || !pictures.empty())
                    out.push_back(Run{ lines, pictures });
                return;
            }

            // Everything sits left of the gutter, right of it, or across it. The things across it cut the
            // page into bands, top to bottom.
            enum class Side { Left, Right, Across };
            const auto sideOf = [&](double left, double right) {
                return right <= gutterLeft + kEdge ? Side::Left : left >= gutterRight - kEdge ? Side::Right : Side::Across;
            };
            struct Across
            {
                double top;
                const TextLine* line;
                const PagePicture* picture;
            };
            std::vector<Across> across;
            for (const auto& line : lines)
                if (sideOf(line.x, line.endX) == Side::Across)
                    across.push_back({ TopOf(line), &line, nullptr });
            for (const auto& picture : pictures)
                if (sideOf(picture.left, picture.right) == Side::Across)
                    across.push_back({ picture.top, nullptr, &picture });
            std::stable_sort(across.begin(), across.end(), [](const Across& a, const Across& b) { return a.top > b.top; });

            struct Band
            {
                Run left, right;
            };
            std::vector<Band> bands(across.size() + 1);
            // Band k holds what sits below k of the cuts (and above the rest).
            const auto bandOf = [&](double top) {
                return static_cast<size_t>(std::lower_bound(across.begin(), across.end(), top, [](const Across& a, double t) { return a.top > t; }) -
                                           across.begin());
            };
            for (const auto& line : lines)
            {
                const Side side = sideOf(line.x, line.endX);
                if (side != Side::Across)
                {
                    auto& band = bands[bandOf(TopOf(line))];
                    (side == Side::Left ? band.left : band.right).lines.push_back(line);
                }
            }
            for (const auto& picture : pictures)
            {
                const Side side = sideOf(picture.left, picture.right);
                if (side != Side::Across)
                {
                    auto& band = bands[bandOf(picture.top)];
                    (side == Side::Left ? band.left : band.right).pictures.push_back(picture);
                }
            }

            Run spanning;   // consecutive things across the gutter read as one run
            const auto flushSpanning = [&] {
                if (!spanning.lines.empty() || !spanning.pictures.empty())
                    out.push_back(std::move(spanning));
                spanning = Run{};
            };
            const auto column = [&](Run& run) {
                // Within a column, top to bottom whatever order the PDF drew it in.
                std::stable_sort(run.lines.begin(), run.lines.end(), [](const TextLine& a, const TextLine& b) { return a.y > b.y; });
                Flow(run.lines, run.pictures, depth + 1, out);
            };
            for (size_t k = 0; k < bands.size(); ++k)
            {
                auto& band = bands[k];
                if (!band.left.lines.empty() || !band.left.pictures.empty() || !band.right.lines.empty() || !band.right.pictures.empty())
                {
                    flushSpanning();
                    column(band.left);
                    column(band.right);
                }
                if (k < across.size())
                {
                    if (across[k].line)
                        spanning.lines.push_back(*across[k].line);
                    else
                        spanning.pictures.push_back(*across[k].picture);
                }
            }
            flushSpanning();
        }

        std::wstring BlockMarkdown(const Block& block, double body)
        {
            const std::wstring text = Reflow(block);
            if (text.empty())
                return {};
            const double ratio = body > 0 ? block.size / body : 1.0;
            if (block.bullet)
                return L"- " + text;
            if (ratio >= 1.2 && block.lines.size() <= 3 && text.size() <= 150)
                return std::wstring(ratio >= 1.8 ? L"# " : ratio >= 1.4 ? L"## " : L"### ") + text;
            return Escape(text);
        }
    }

    bool FindGutter(const PageLines& lines, double& left, double& right)
    {
        const size_t n = lines.size();
        if (n < 6)
            return false;
        double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
        std::vector<double> starts, ends, sizes;
        for (const auto& line : lines)
        {
            minX = std::min(minX, line.x);
            maxX = std::max(maxX, line.endX);
            minY = std::min(minY, line.y);
            maxY = std::max(maxY, line.y);
            starts.push_back(line.x);
            ends.push_back(line.endX);
            sizes.push_back(line.size);
        }
        const double width = maxX - minX;
        if (width < 72)
            return false;
        const double body = Median(sizes);
        const double narrowest = std::max(0.7 * body, 3.0);   // LaTeX's default column gap is about 1 em
        std::sort(starts.begin(), starts.end());
        std::sort(ends.begin(), ends.end());
        const auto endingBy = [&](double x) { return static_cast<size_t>(std::upper_bound(ends.begin(), ends.end(), x) - ends.begin()); };
        const auto startingFrom = [&](double x) { return static_cast<size_t>(starts.end() - std::lower_bound(starts.begin(), starts.end(), x)); };
        // How many lines a strip [a, b] cuts through: all but those wholly left or wholly right of it.
        const auto crossing = [&](double a, double b) {
            const size_t beside = endingBy(a) + startingFrom(b);
            return beside >= n ? size_t{ 0 } : n - beside;
        };

        // Slide a strip as wide as the narrowest gutter over the middle of the page; keep the one the
        // fewest lines cross, then widen it to the lines on either side.
        const double from = minX + 0.2 * width, to = maxX - 0.2 * width;
        const double step = std::max(0.5, width / 2000);
        size_t best = n + 1;
        double a = 0, b = 0;
        for (double centre = from; centre <= to; centre += step)
        {
            const double sa = centre - narrowest / 2, sb = centre + narrowest / 2;
            const size_t c = crossing(sa, sb);
            if (c < best)
            {
                best = c;
                a = sa;
                b = sb;
            }
        }
        if (best > n)
            return false;
        // The nearest line end at or left of the strip, the nearest start at or right of it.
        const auto lastEnd = std::upper_bound(ends.begin(), ends.end(), a);
        const auto firstStart = std::lower_bound(starts.begin(), starts.end(), b);
        if (lastEnd == ends.begin() || firstStart == starts.end())
            return false;
        left = *(lastEnd - 1);
        right = *firstStart;

        // Both sides need a real column of text...
        std::vector<double> leftWidths, rightWidths;
        double leftX = 1e300, rightX = -1e300;
        double leftTop = -1e300, leftBottom = 1e300, rightTop = -1e300, rightBottom = 1e300;
        for (const auto& line : lines)
        {
            if (line.endX <= left + kEdge)
            {
                leftWidths.push_back(line.endX - line.x);
                leftX = std::min(leftX, line.x);
                leftTop = std::max(leftTop, line.y);
                leftBottom = std::min(leftBottom, line.y);
            }
            else if (line.x >= right - kEdge)
            {
                rightWidths.push_back(line.endX - line.x);
                rightX = std::max(rightX, line.endX);
                rightTop = std::max(rightTop, line.y);
                rightBottom = std::min(rightBottom, line.y);
            }
        }
        const size_t inColumns = leftWidths.size() + rightWidths.size();
        if (leftWidths.size() < 3 || rightWidths.size() < 3 || 3 * (n - inColumns) > inColumns)
            return false;
        // ...the two side by side over a good part of the page (not one paragraph under the other)...
        const double overlap = std::min(leftTop, rightTop) - std::max(leftBottom, rightBottom);
        if (overlap < 2 * body || overlap < 0.4 * (maxY - minY + body))
            return false;
        // ...and mostly filled to their width, as running text is (short labels with their values
        // beside them are a table, not two columns).
        return Median(leftWidths) >= 0.4 * (left - leftX) && Median(rightWidths) >= 0.4 * (rightX - right);
    }

    std::vector<PageLines> ReadingOrder(const PageLines& lines)
    {
        std::vector<Run> runs;
        Flow(lines, {}, 0, runs);
        std::vector<PageLines> out;
        for (auto& run : runs)
            out.push_back(std::move(run.lines));
        return out;
    }

    PageLines JoinSpans(const std::vector<TextSpan>& spans)
    {
        PageLines lines;
        for (const auto& span : spans)
        {
            if (span.text.empty())
                continue;
            if (!lines.empty())
            {
                auto& line = lines.back();
                const double size = std::max(line.size, span.size);
                const bool sameBaseline = std::abs(span.y - line.y) <= 0.5 * size;
                const bool forward = span.x >= line.endX - 0.5 * size;
                if (sameBaseline && forward)
                {
                    const double gap = span.x - line.endX;
                    if (gap > 0.15 * size && !IsSpace(line.text.back()) && !IsSpace(span.text.front()))
                        line.text += L' ';
                    line.text += span.text;
                    line.endX = std::max(line.endX, span.endX);
                    // A superscript or a single symbol does not make the line bigger.
                    if (NonSpace(span.text) >= 3 || line.size == 0)
                        line.size = std::max(line.size, span.size);
                    continue;
                }
            }
            lines.push_back(TextLine{ span.x, span.y, span.endX, span.size, span.text });
        }
        PageLines out;
        for (auto& line : lines)
        {
            line.text = Collapse(line.text);
            if (!line.text.empty())
                out.push_back(std::move(line));
        }
        return out;
    }

    PageLines LinesFromOcr(const ocr::Result& ocr, double scale)
    {
        PageLines out;
        for (const auto& l : ocr.lines)
        {
            if (l.words.empty())
                continue;
            double left = l.words.front().x, right = 0, bottom = 0, heights = 0;
            for (const auto& w : l.words)
            {
                left = std::min(left, w.x);
                right = std::max(right, w.x + w.w);
                bottom = std::max(bottom, w.y + w.h);
                heights += w.h;
            }
            TextLine line;
            line.x = left * scale;
            line.endX = right * scale;
            line.y = (static_cast<double>(ocr.height) - bottom) * scale;
            line.size = heights / static_cast<double>(l.words.size()) * scale;
            line.text = Collapse(l.text);
            if (!line.text.empty())
                out.push_back(std::move(line));
        }
        return out;
    }

    std::wstring ToText(const std::vector<PageLines>& pages)
    {
        std::wstring out;
        for (const auto& page : pages)
        {
            for (const auto& run : ReadingOrder(page))
            {
                for (const auto& block : Blocks(run))
                {
                    if (!out.empty())
                        out += L"\r\n\r\n";
                    for (size_t i = 0; i < block.lines.size(); ++i)
                        out += (i ? L"\r\n" : L"") + block.lines[i]->text;
                }
            }
        }
        if (!out.empty())
            out += L"\r\n";
        return out;
    }

    std::wstring ToMarkdown(const std::vector<PageLines>& pages)
    {
        auto parts = ToMarkdownParts(pages, {});
        return parts.empty() ? std::wstring() : std::move(parts.front().markdown);
    }

    std::vector<MarkdownPart> ToMarkdownParts(const std::vector<PageLines>& pages, const std::vector<std::vector<PagePicture>>& pictures)
    {
        const double body = BodySize(pages);
        std::vector<MarkdownPart> parts;
        std::wstring text;
        bool lastBullet = false;
        const auto addPicture = [&](int id) {
            if (!text.empty())
                parts.push_back(MarkdownPart{ std::move(text) + L"\n", -1 });
            text.clear();
            parts.push_back(MarkdownPart{ {}, id });
            lastBullet = false;
        };
        static const std::vector<PagePicture> kNone;
        for (size_t i = 0; i < pages.size(); ++i)
        {
            std::vector<Run> runs;
            Flow(pages[i], i < pictures.size() ? pictures[i] : kNone, 0, runs);
            for (auto& run : runs)
            {
                // A picture goes before the first paragraph that starts below its top edge.
                std::stable_sort(run.pictures.begin(), run.pictures.end(), [](const PagePicture& a, const PagePicture& b) { return a.top > b.top; });
                size_t next = 0;
                for (const auto& block : Blocks(run.lines))
                {
                    const double top = TopOf(block);
                    while (next < run.pictures.size() && run.pictures[next].top > top)
                        addPicture(run.pictures[next++].id);
                    const std::wstring md = BlockMarkdown(block, body);
                    if (md.empty())
                        continue;
                    if (!text.empty())
                        text += block.bullet && lastBullet ? L"\n" : L"\n\n";
                    text += md;
                    lastBullet = block.bullet;
                }
                while (next < run.pictures.size())
                    addPicture(run.pictures[next++].id);
            }
        }
        if (!text.empty())
            parts.push_back(MarkdownPart{ std::move(text) + L"\n", -1 });
        return parts;
    }

    size_t VisibleChars(const PageLines& lines)
    {
        size_t n = 0;
        for (const auto& line : lines)
            n += NonSpace(line.text);
        return n;
    }
}
