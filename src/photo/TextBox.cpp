#include "photo/TextBox.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace sd::photo
{
    namespace
    {
        bool High(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
        bool Low(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

        // The position before / after `pos`, never inside a surrogate pair.
        size_t Prev(const std::wstring& t, size_t pos)
        {
            if (pos == 0)
                return 0;
            if (pos >= 2 && Low(t[pos - 1]) && High(t[pos - 2]))
                return pos - 2;
            return pos - 1;
        }

        size_t Next(const std::wstring& t, size_t pos)
        {
            if (pos >= t.size())
                return t.size();
            if (pos + 1 < t.size() && High(t[pos]) && Low(t[pos + 1]))
                return pos + 2;
            return pos + 1;
        }

        // 0 a space, 1 a letter or digit, 2 anything else (punctuation), 3 a line break.
        int ClassOf(wchar_t c)
        {
            if (c == L'\n')
                return 3;
            if (std::iswspace(c))
                return 0;
            if (std::iswalnum(c) || c == L'_' || c >= 0x80)
                return 1;
            return 2;
        }

        size_t WordLeft(const std::wstring& t, size_t pos)
        {
            // Back over spaces, then over the word (or the punctuation) before them.
            while (pos > 0 && ClassOf(t[pos - 1]) == 0)
                --pos;
            if (pos > 0 && ClassOf(t[pos - 1]) == 3)
                return pos - 1;
            const int cls = pos > 0 ? ClassOf(t[pos - 1]) : 0;
            while (pos > 0 && ClassOf(t[pos - 1]) == cls && cls != 0)
                --pos;
            return pos;
        }

        size_t WordRight(const std::wstring& t, size_t pos)
        {
            // Over this word (or punctuation), then over the spaces after it: the next word's start.
            if (pos < t.size() && ClassOf(t[pos]) == 3)
                return pos + 1;
            const int cls = pos < t.size() ? ClassOf(t[pos]) : 0;
            if (cls != 0)
                while (pos < t.size() && ClassOf(t[pos]) == cls)
                    ++pos;
            while (pos < t.size() && ClassOf(t[pos]) == 0)
                ++pos;
            return pos;
        }

        bool SoftWrapped(const TextLayoutInfo& l, size_t i) { return i + 1 < l.lines.size() && l.lines[i + 1].start == l.lines[i].end; }

        // Where the caret goes at the end of line i: before its break, or before the space it wrapped at.
        size_t LineEnd(const TextLayoutInfo& l, size_t i)
        {
            const auto& line = l.lines[i];
            return SoftWrapped(l, i) && line.end > line.start ? line.end - 1 : line.end;
        }

        double X(const TextLayoutInfo& l, size_t pos) { return pos < l.caretX.size() ? l.caretX[pos] : l.caretX.empty() ? 0 : l.caretX.back(); }

        // The caret position on line i nearest to x.
        size_t NearestOnLine(const TextLayoutInfo& l, size_t i, double x)
        {
            const auto& line = l.lines[i];
            const size_t last = LineEnd(l, i);
            size_t best = line.start;
            double bestDist = std::abs(X(l, line.start) - x);
            for (size_t p = line.start + 1; p <= last; ++p)
            {
                const double d = std::abs(X(l, p) - x);
                if (d < bestDist)
                {
                    best = p;
                    bestDist = d;
                }
            }
            return best;
        }
    }

    size_t TextLayoutInfo::LineOf(size_t pos) const
    {
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (pos < lines[i].start)
                return i == 0 ? 0 : i - 1;
            if (pos < lines[i].end || (pos == lines[i].end && !SoftWrapped(*this, i)))
                return i;
        }
        return lines.empty() ? 0 : lines.size() - 1;
    }

    size_t PositionAt(const TextLayoutInfo& l, PointD at)
    {
        if (l.lines.empty())
            return 0;
        size_t i = 0;
        while (i + 1 < l.lines.size() && at.y >= l.lines[i].top + l.lines[i].height)
            ++i;
        return NearestOnLine(l, i, at.x);
    }

    CaretBox CaretAt(const TextLayoutInfo& l, size_t pos)
    {
        if (l.lines.empty())
            return {};
        const auto& line = l.lines[l.LineOf(pos)];
        return { X(l, pos), line.top, line.height };
    }

    std::vector<RectD> SelectionRects(const TextLayoutInfo& l, size_t from, size_t to, double newlineWidth)
    {
        std::vector<RectD> out;
        if (from >= to)
            return out;
        for (size_t i = 0; i < l.lines.size(); ++i)
        {
            const auto& line = l.lines[i];
            const size_t s = std::max(from, line.start), e = std::min(to, line.end);
            const bool breakSelected = !SoftWrapped(l, i) && i + 1 < l.lines.size() && from <= line.end && to > line.end;
            if (s >= e && !breakSelected)
                continue;
            const double x0 = s >= e ? X(l, line.end) : X(l, s);
            double x1 = s >= e ? x0 : e == line.end ? line.right : X(l, e);
            if (breakSelected)
                x1 += newlineWidth;
            out.push_back({ std::min(x0, x1), line.top, std::abs(x1 - x0), line.height });
        }
        return out;
    }

    void TextCursor::Clamp(const std::wstring& text)
    {
        caret = std::min(caret, text.size());
        anchor = std::min(anchor, text.size());
    }

    void MoveCaret(TextCursor& c, const std::wstring& text, const TextLayoutInfo& l, CaretMove move, bool extend, double& goalX)
    {
        c.Clamp(text);
        const bool vertical = move == CaretMove::Up || move == CaretMove::Down;
        if (!vertical)
            goalX = -1;
        size_t pos = c.caret;
        switch (move)
        {
        case CaretMove::Left: pos = !extend && c.HasSelection() ? c.Start() : Prev(text, c.caret); break;
        case CaretMove::Right: pos = !extend && c.HasSelection() ? c.End() : Next(text, c.caret); break;
        case CaretMove::WordLeft: pos = WordLeft(text, c.caret); break;
        case CaretMove::WordRight: pos = WordRight(text, c.caret); break;
        case CaretMove::Home:
            if (!l.lines.empty())
                pos = l.lines[l.LineOf(c.caret)].start;
            break;
        case CaretMove::End:
            if (!l.lines.empty())
                pos = LineEnd(l, l.LineOf(c.caret));
            break;
        case CaretMove::Up:
        case CaretMove::Down:
        {
            if (l.lines.empty())
                break;
            if (goalX < 0)
                goalX = X(l, c.caret);
            const size_t line = l.LineOf(c.caret);
            if (move == CaretMove::Up)
                pos = line == 0 ? 0 : NearestOnLine(l, line - 1, goalX);
            else
                pos = line + 1 >= l.lines.size() ? text.size() : NearestOnLine(l, line + 1, goalX);
            break;
        }
        case CaretMove::TextStart: pos = 0; break;
        case CaretMove::TextEnd: pos = text.size(); break;
        }
        c.caret = std::min(pos, text.size());
        if (!extend)
            c.anchor = c.caret;
    }

    bool InsertText(std::wstring& text, TextCursor& c, std::wstring_view typed, size_t limit)
    {
        c.Clamp(text);
        const size_t start = c.Start(), end = c.End();
        const size_t kept = text.size() - (end - start);
        size_t room = kept >= limit ? 0 : limit - kept;
        std::wstring_view add = typed.substr(0, std::min(room, typed.size()));
        if (!add.empty() && add.size() < typed.size() && High(add.back()))
            add.remove_suffix(1);   // never half a surrogate pair
        if (add.empty() && start == end)
            return false;
        text.replace(start, end - start, add);
        c.Collapse(start + add.size());
        return true;
    }

    bool DeleteBackward(std::wstring& text, TextCursor& c, bool word)
    {
        c.Clamp(text);
        if (c.HasSelection())
            return InsertText(text, c, {}, text.size());
        if (c.caret == 0)
            return false;
        const size_t from = word ? WordLeft(text, c.caret) : Prev(text, c.caret);
        text.erase(from, c.caret - from);
        c.Collapse(from);
        return true;
    }

    bool DeleteForward(std::wstring& text, TextCursor& c, bool word)
    {
        c.Clamp(text);
        if (c.HasSelection())
            return InsertText(text, c, {}, text.size());
        if (c.caret >= text.size())
            return false;
        const size_t to = word ? WordRight(text, c.caret) : Next(text, c.caret);
        text.erase(c.caret, to - c.caret);
        return true;
    }

    std::wstring SelectedText(const std::wstring& text, const TextCursor& c)
    {
        const size_t s = std::min(c.Start(), text.size()), e = std::min(c.End(), text.size());
        return text.substr(s, e - s);
    }

    void SelectWord(TextCursor& c, const std::wstring& text, size_t pos)
    {
        pos = std::min(pos, text.size());
        if (text.empty())
        {
            c.Collapse(0);
            return;
        }
        size_t at = pos < text.size() && ClassOf(text[pos]) != 3 ? pos : pos > 0 ? pos - 1 : 0;
        const int cls = ClassOf(text[at]);
        if (cls == 3)
        {
            c.Collapse(pos);
            return;
        }
        size_t s = at, e = at + 1;
        while (s > 0 && ClassOf(text[s - 1]) == cls)
            --s;
        while (e < text.size() && ClassOf(text[e]) == cls)
            ++e;
        c.anchor = s;
        c.caret = e;
    }

    std::wstring CleanPasted(std::wstring_view text)
    {
        std::wstring out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            const wchar_t ch = text[i];
            if (ch == L'\r')
            {
                out.push_back(L'\n');
                if (i + 1 < text.size() && text[i + 1] == L'\n')
                    ++i;
            }
            else if (ch == L'\n')
                out.push_back(L'\n');
            else if (ch == L'\t')
                out.push_back(L' ');
            else if (ch >= 0x20 && ch != 0x7F)
                out.push_back(ch);
        }
        return out;
    }
}
