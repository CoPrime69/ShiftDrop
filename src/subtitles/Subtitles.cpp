#include "subtitles/Subtitles.h"
#include "docs/Bytes.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace sd::subtitles
{
    namespace
    {
        bool AllDigits(std::string_view s)
        {
            return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
        }

        std::string_view RightTrim(std::string_view s)
        {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                s.remove_suffix(1);
            return s;
        }

        std::vector<std::string_view> SplitLines(std::string_view s)
        {
            std::vector<std::string_view> lines;
            size_t start = 0;
            while (start <= s.size())
            {
                const size_t nl = s.find('\n', start);
                if (nl == std::string_view::npos)
                {
                    lines.push_back(s.substr(start));
                    break;
                }
                lines.push_back(s.substr(start, nl - start));
                start = nl + 1;
            }
            return lines;
        }

        // "a --> b [settings]": the cue timing line. Both times must parse.
        bool ParseTiming(std::string_view line, int64_t& start, int64_t& end)
        {
            const size_t arrow = line.find("-->");
            if (arrow == std::string_view::npos)
                return false;
            auto rest = docs::Trim(line.substr(arrow + 3));
            const size_t space = rest.find_first_of(" \t");
            const auto endText = space == std::string_view::npos ? rest : rest.substr(0, space);
            return ParseTimestamp(line.substr(0, arrow), start) && ParseTimestamp(endText, end);
        }

        // A VTT block keyword at the start of a line: "NOTE", "STYLE" or "REGION" alone or followed
        // by a space or tab.
        bool IsVttBlockKeyword(std::string_view line)
        {
            for (std::string_view k : { "NOTE", "STYLE", "REGION" })
                if (line.starts_with(k) && (line.size() == k.size() || line[k.size()] == ' ' || line[k.size()] == '\t'))
                    return true;
            return false;
        }

        // Splits cue text into markup tokens and text. A tag is "<" followed by a letter, "/" or a
        // digit (VTT timestamps) up to the next ">" on the same line; an override block is "{\...}".
        struct Token
        {
            enum Kind
            {
                Text,
                Tag,
                Override,
            } kind;
            std::string_view text;
        };

        std::vector<Token> Tokenize(std::string_view s)
        {
            std::vector<Token> tokens;
            size_t i = 0, textStart = 0;
            const auto flush = [&](size_t end) {
                if (end > textStart)
                    tokens.push_back({ Token::Text, s.substr(textStart, end - textStart) });
            };
            while (i < s.size())
            {
                if (s[i] == '<' && i + 1 < s.size() && (std::isalnum(static_cast<unsigned char>(s[i + 1])) || s[i + 1] == '/'))
                {
                    const size_t close = s.find_first_of(">\n", i);
                    if (close != std::string_view::npos && s[close] == '>')
                    {
                        flush(i);
                        tokens.push_back({ Token::Tag, s.substr(i, close + 1 - i) });
                        i = textStart = close + 1;
                        continue;
                    }
                }
                if (s[i] == '{' && i + 1 < s.size() && s[i + 1] == '\\')
                {
                    const size_t close = s.find_first_of("}\n", i);
                    if (close != std::string_view::npos && s[close] == '}')
                    {
                        flush(i);
                        tokens.push_back({ Token::Override, s.substr(i, close + 1 - i) });
                        i = textStart = close + 1;
                        continue;
                    }
                }
                ++i;
            }
            flush(s.size());
            return tokens;
        }

        // The tag's name, lower case, without "/" or classes: "</B>" -> "b", "<c.yellow>" -> "c",
        // "<v Bob>" -> "v", "<00:01.000>" -> "" (a timestamp).
        std::string TagName(std::string_view tag, bool* closing = nullptr)
        {
            auto body = tag.substr(1, tag.size() - 2);
            const bool close = !body.empty() && body[0] == '/';
            if (close)
                body.remove_prefix(1);
            if (closing)
                *closing = close;
            if (!body.empty() && std::isdigit(static_cast<unsigned char>(body[0])))
                return {};
            const size_t end = body.find_first_of(" \t.");
            return docs::AsciiLower(body.substr(0, end));
        }

        // Entity references decoded ("&amp;" -> "&"); an "&" that starts no known reference stays.
        std::string DecodeEntities(std::string_view s)
        {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '&')
                {
                    const size_t semi = s.find(';', i + 1);
                    if (semi != std::string_view::npos && semi - i <= 12)
                    {
                        auto name = s.substr(i + 1, semi - i - 1);
                        std::string decoded;
                        if (name == "lrm")
                            decoded = "\xE2\x80\x8E";
                        else if (name == "rlm")
                            decoded = "\xE2\x80\x8F";
                        else
                            decoded = docs::DecodeEntity(name);
                        if (!decoded.empty())
                        {
                            out += decoded;
                            i = semi;
                            continue;
                        }
                    }
                }
                out += s[i];
            }
            return out;
        }

        bool StartsEntity(std::string_view s)
        {
            // s starts after the "&".
            const size_t semi = s.find(';');
            if (semi == std::string_view::npos || semi == 0 || semi > 10)
                return false;
            const auto name = s.substr(0, semi);
            if (name[0] == '#')
                return name.size() > 1 && std::all_of(name.begin() + 1, name.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) || c == 'x' || c == 'X'; });
            return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)); });
        }

        // Text for a VTT cue: & and < escaped (existing references kept), "-->" broken up.
        std::string EscapeVtt(std::string_view s)
        {
            std::string out;
            for (size_t i = 0; i < s.size(); ++i)
            {
                const char c = s[i];
                if (c == '&' && !StartsEntity(s.substr(i + 1)))
                    out += "&amp;";
                else if (c == '<')
                    out += "&lt;";
                else if (c == '>' && i >= 2 && s[i - 1] == '-' && s[i - 2] == '-')
                    out += "&gt;";
                else
                    out += c;
            }
            return out;
        }

        // Cue text cleaned for writing: lines right-trimmed, blank lines dropped (a blank line
        // would end the cue).
        std::vector<std::string> CueLines(std::string_view text)
        {
            std::vector<std::string> lines;
            for (auto line : SplitLines(text))
            {
                line = RightTrim(line);
                if (!docs::Trim(line).empty())
                    lines.emplace_back(line);
            }
            return lines;
        }

        std::string FormatTime(int64_t ms, char separator)
        {
            ms = std::max<int64_t>(0, ms);
            return std::format("{:02}:{:02}:{:02}{}{:03}", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, separator, ms % 1000);
        }
    }

    bool ParseTimestamp(std::string_view s, int64_t& ms)
    {
        s = docs::Trim(s);
        std::vector<std::string_view> parts;
        char lastSeparator = 0;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i)
        {
            if (i == s.size() || s[i] == ':' || s[i] == ',' || s[i] == '.')
            {
                parts.push_back(s.substr(start, i - start));
                if (i < s.size())
                {
                    if (lastSeparator == ',' || lastSeparator == '.')
                        return false;   // nothing may follow the fraction
                    lastSeparator = s[i];
                }
                start = i + 1;
            }
            else if (s[i] < '0' || s[i] > '9')
                return false;
        }
        for (auto p : parts)
            if (p.empty() || p.size() > 9)
                return false;

        std::string_view fraction;
        std::vector<std::string_view> clock = parts;
        if (lastSeparator == ',' || lastSeparator == '.' || parts.size() == 4)
        {
            fraction = parts.back();
            clock.pop_back();
        }
        if (clock.size() < 2 || clock.size() > 3)
            return false;

        int64_t h = 0, m = 0, sec = 0;
        size_t k = 0;
        if (clock.size() == 3)
            h = std::stoll(std::string(clock[k++]));
        m = std::stoll(std::string(clock[k++]));
        sec = std::stoll(std::string(clock[k]));
        if (sec > 59 || (clock.size() == 3 && m > 59))
            return false;
        int64_t frac = 0;
        if (!fraction.empty())
        {
            std::string f(fraction.substr(0, 3));
            while (f.size() < 3)
                f += '0';
            frac = std::stoll(f);
        }
        ms = ((h * 60 + m) * 60 + sec) * 1000 + frac;
        return true;
    }

    std::string FormatSrtTime(int64_t ms)
    {
        return FormatTime(ms, ',');
    }

    std::string FormatVttTime(int64_t ms)
    {
        return FormatTime(ms, '.');
    }

    Track Parse(std::string_view input)
    {
        if (input.starts_with("\xEF\xBB\xBF"))
            input.remove_prefix(3);
        const std::string text = docs::NormalizeNewlines(input);
        const auto lines = SplitLines(text);

        Track track;
        size_t first = 0;
        while (first < lines.size() && docs::Trim(lines[first]).empty())
            ++first;
        bool skipping = false;   // inside the VTT header or a NOTE / STYLE / REGION block
        if (first < lines.size() && lines[first].starts_with("WEBVTT"))
        {
            track.source = Format::Vtt;
            skipping = true;
        }

        Cue* cue = nullptr;                  // the cue whose text lines are being read
        std::vector<std::string> pending;    // lines outside any cue: an identifier, or stray text
        const auto flushPending = [&](bool keepLastAsId) -> std::string {
            std::string id;
            if (keepLastAsId && !pending.empty())
            {
                id = pending.back();
                pending.pop_back();
            }
            // Text after a stray blank line inside a cue belongs to the cue before.
            if (!track.cues.empty())
                for (auto& p : pending)
                    track.cues.back().text += "\n" + p;
            pending.clear();
            return id;
        };

        for (size_t i = first; i < lines.size(); ++i)
        {
            const auto line = RightTrim(lines[i]);
            const bool blank = docs::Trim(line).empty();
            int64_t start = 0, end = 0;
            const bool timing = !blank && ParseTiming(line, start, end);

            if (skipping)
            {
                if (blank)
                    skipping = false;
                if (!timing)
                    continue;
                skipping = false;
            }
            if (blank)
            {
                cue = nullptr;
                flushPending(false);
                continue;
            }
            if (timing)
            {
                std::string id;
                if (cue)
                {
                    // No blank line before this cue: a lone number at the end of the last cue's
                    // text is really this cue's index.
                    const size_t nl = cue->text.rfind('\n');
                    const std::string_view last = nl == std::string::npos ? std::string_view(cue->text) : std::string_view(cue->text).substr(nl + 1);
                    if (AllDigits(docs::Trim(last)) && nl != std::string::npos)
                    {
                        id = std::string(docs::Trim(last));
                        cue->text.resize(nl);
                    }
                }
                else
                    id = flushPending(true);
                Cue c;
                c.startMs = start;
                c.endMs = std::max(start, end);
                c.id = std::move(id);
                track.cues.push_back(std::move(c));
                cue = &track.cues.back();
                continue;
            }
            if (cue)
            {
                if (!cue->text.empty())
                    cue->text += '\n';
                cue->text += line;
                continue;
            }
            if (track.source == Format::Vtt && pending.empty() && IsVttBlockKeyword(line))
            {
                skipping = true;
                continue;
            }
            pending.emplace_back(line);
        }
        flushPending(false);

        std::erase_if(track.cues, [](const Cue& c) { return CueLines(c.text).empty(); });
        for (auto& c : track.cues)
        {
            std::string joined;
            for (const auto& l : CueLines(c.text))
                joined += (joined.empty() ? "" : "\n") + l;
            c.text = std::move(joined);
        }
        std::stable_sort(track.cues.begin(), track.cues.end(), [](const Cue& a, const Cue& b) { return a.startMs < b.startMs; });
        return track;
    }

    std::string SrtToVttText(std::string_view text)
    {
        std::string out;
        for (const auto& t : Tokenize(text))
        {
            if (t.kind == Token::Text)
                out += EscapeVtt(t.text);
            else if (t.kind == Token::Tag)
            {
                bool closing = false;
                const auto name = TagName(t.text, &closing);
                if (name == "b" || name == "i" || name == "u")
                    out += std::format("<{}{}>", closing ? "/" : "", name);
                // <font> and anything else: dropped, VTT has no equivalent.
            }
        }
        return out;
    }

    std::string VttToSrtText(std::string_view text)
    {
        std::string out;
        int inRubyText = 0;
        for (const auto& t : Tokenize(text))
        {
            if (t.kind == Token::Text)
            {
                if (!inRubyText)
                    out += DecodeEntities(t.text);
            }
            else if (t.kind == Token::Tag)
            {
                bool closing = false;
                const auto name = TagName(t.text, &closing);
                if (name == "b" || name == "i" || name == "u")
                    out += std::format("<{}{}>", closing ? "/" : "", name);
                else if (name == "rt")
                    inRubyText = closing ? std::max(0, inRubyText - 1) : inRubyText + 1;
            }
        }
        return out;
    }

    std::string PlainCueText(std::string_view text, Format)
    {
        std::string out;
        int inRubyText = 0;
        for (const auto& t : Tokenize(text))
        {
            if (t.kind == Token::Text)
            {
                if (!inRubyText)
                    out += DecodeEntities(t.text);
            }
            else if (t.kind == Token::Tag)
            {
                bool closing = false;
                if (TagName(t.text, &closing) == "rt")
                    inRubyText = closing ? std::max(0, inRubyText - 1) : inRubyText + 1;
            }
        }
        return out;
    }

    std::string WriteSrt(const Track& track)
    {
        std::string out = "\xEF\xBB\xBF";
        int n = 0;
        for (const auto& c : track.cues)
        {
            const auto lines = CueLines(track.source == Format::Vtt ? VttToSrtText(c.text) : c.text);
            if (lines.empty())
                continue;
            out += std::format("{}\r\n{} --> {}\r\n", ++n, FormatSrtTime(c.startMs), FormatSrtTime(c.endMs));
            for (const auto& l : lines)
                out += l + "\r\n";
            out += "\r\n";
        }
        return out;
    }

    std::string WriteVtt(const Track& track)
    {
        std::string out = "WEBVTT\n";
        int n = 0;
        for (const auto& c : track.cues)
        {
            const auto lines = CueLines(track.source == Format::Srt ? SrtToVttText(c.text) : c.text);
            if (lines.empty())
                continue;
            ++n;
            out += '\n';
            if (track.source == Format::Srt)
                out += std::format("{}\n", n);
            else if (!c.id.empty() && c.id.find("-->") == std::string::npos)
                out += c.id + '\n';
            out += std::format("{} --> {}\n", FormatVttTime(c.startMs), FormatVttTime(c.endMs));
            for (const auto& l : lines)
                out += l + '\n';
        }
        return out;
    }

    std::string WriteText(const Track& track)
    {
        std::string out;
        std::string previous;
        for (const auto& c : track.cues)
        {
            std::string merged;
            for (auto line : SplitLines(PlainCueText(c.text, track.source)))
            {
                // Collapse runs of spaces and tabs.
                std::string clean;
                for (char ch : docs::Trim(line))
                    if (!((ch == ' ' || ch == '\t') && (clean.empty() || clean.back() == ' ')))
                        clean += ch == '\t' ? ' ' : ch;
                if (clean.empty() || clean == previous)
                    continue;
                previous = clean;
                merged += (merged.empty() ? "" : " ") + clean;
            }
            if (!merged.empty())
                out += merged + "\r\n";
        }
        return out;
    }
}
