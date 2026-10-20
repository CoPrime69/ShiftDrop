#include "pdf/TextLayout.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
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
            for (const auto& block : Blocks(page))
            {
                if (!out.empty())
                    out += L"\r\n\r\n";
                for (size_t i = 0; i < block.lines.size(); ++i)
                    out += (i ? L"\r\n" : L"") + block.lines[i]->text;
            }
        }
        if (!out.empty())
            out += L"\r\n";
        return out;
    }

    std::wstring ToMarkdown(const std::vector<PageLines>& pages)
    {
        const double body = BodySize(pages);
        std::wstring out;
        bool lastBullet = false;
        for (const auto& page : pages)
        {
            for (const auto& block : Blocks(page))
            {
                const std::wstring text = Reflow(block);
                if (text.empty())
                    continue;
                std::wstring md;
                const double ratio = body > 0 ? block.size / body : 1.0;
                if (block.bullet)
                    md = L"- " + text;
                else if (ratio >= 1.2 && block.lines.size() <= 3 && text.size() <= 150)
                    md = std::wstring(ratio >= 1.8 ? L"# " : ratio >= 1.4 ? L"## " : L"### ") + text;
                else
                    md = Escape(text);
                if (!out.empty())
                    out += block.bullet && lastBullet ? L"\n" : L"\n\n";
                out += md;
                lastBullet = block.bullet;
            }
        }
        if (!out.empty())
            out += L"\n";
        return out;
    }

    size_t VisibleChars(const PageLines& lines)
    {
        size_t n = 0;
        for (const auto& line : lines)
            n += NonSpace(line.text);
        return n;
    }
}
