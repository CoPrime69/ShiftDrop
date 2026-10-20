#include "inspect/ProbeReport.h"
#include "Media.h"
#include "Utf.h"

#include <charconv>
#include <cctype>
#include <format>
#include <string_view>

namespace sd::inspect
{
    namespace
    {
        size_t Indent(std::string_view line)
        {
            size_t n = 0;
            while (n < line.size() && line[n] == ' ')
                ++n;
            return n;
        }

        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.remove_suffix(1);
            return s;
        }

        // The number just before `suffix` in `line` ("44100" before " Hz"), or 0.
        double NumberBefore(std::string_view line, std::string_view suffix)
        {
            const auto at = line.find(suffix);
            if (at == std::string_view::npos || at == 0)
                return 0;
            size_t start = at;
            while (start > 0 && (std::isdigit(static_cast<unsigned char>(line[start - 1])) || line[start - 1] == '.'))
                --start;
            double v = 0;
            std::from_chars(line.data() + start, line.data() + at, v);
            return v;
        }

        void ParseSize(std::string_view line, int& w, int& h)
        {
            for (size_t x = line.find('x'); x != std::string_view::npos; x = line.find('x', x + 1))
            {
                size_t start = x;
                while (start > 0 && std::isdigit(static_cast<unsigned char>(line[start - 1])))
                    --start;
                size_t end = x + 1;
                while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end])))
                    ++end;
                if (x - start >= 2 && end - x - 1 >= 2)
                {
                    std::from_chars(line.data() + start, line.data() + x, w);
                    std::from_chars(line.data() + x + 1, line.data() + end, h);
                    return;
                }
            }
        }

        ProbeStream ParseStream(std::string_view line)
        {
            ProbeStream s;
            auto rest = line.substr(line.find("Stream #") + 8);
            // "0:1[0x2](eng): Audio: aac (LC) ..." -> id "0:1", language "eng"
            size_t idEnd = 0;
            // Digits and the ':' between them; the ':' before " Video" ends the id.
            while (idEnd < rest.size() && (std::isdigit(static_cast<unsigned char>(rest[idEnd])) ||
                                           (rest[idEnd] == ':' && idEnd + 1 < rest.size() && std::isdigit(static_cast<unsigned char>(rest[idEnd + 1])))))
                ++idEnd;
            s.id = std::string(rest.substr(0, idEnd));
            const auto colon = rest.find(": ", idEnd);
            if (colon == std::string_view::npos)
                return s;
            const auto head = rest.substr(idEnd, colon - idEnd);
            if (const auto open = head.find('('); open != std::string_view::npos)
            {
                const auto close = head.find(')', open);
                const auto lang = head.substr(open + 1, close == std::string_view::npos ? std::string_view::npos : close - open - 1);
                if (lang != "und")
                    s.language = std::string(lang);
            }
            rest = rest.substr(colon + 2);
            const auto kindEnd = rest.find(": ");
            s.kind = std::string(rest.substr(0, kindEnd));
            if (kindEnd == std::string_view::npos)
                return s;
            const auto body = rest.substr(kindEnd + 2);
            s.codec = std::string(body.substr(0, body.find_first_of(" ,")));
            s.details = std::string(Trim(body));
            s.isDefault = body.find("(default)") != std::string_view::npos;
            s.attachedPic = body.find("(attached pic)") != std::string_view::npos;
            if (s.kind == "Video")
            {
                ParseSize(body, s.width, s.height);
                s.fps = NumberBefore(body, " fps");
            }
            if (s.kind == "Audio")
            {
                s.sampleRate = static_cast<int>(NumberBefore(body, " Hz"));
                if (const auto hz = body.find(" Hz, "); hz != std::string_view::npos)
                {
                    const auto after = body.substr(hz + 5);
                    s.channels = std::string(after.substr(0, after.find(',')));
                }
            }
            if (const double kbps = NumberBefore(body, " kb/s"); kbps > 0)
                s.bitrate = static_cast<int64_t>(kbps * 1000.0);
            return s;
        }

        std::wstring Text(const std::string& s) { return Widen(s); }

        std::wstring Seconds(double s)
        {
            return DurationText(static_cast<int64_t>(s * 1e6 + 0.5));
        }

        std::wstring Bitrate(int64_t bps)
        {
            return bps >= 1'000'000 ? std::format(L"{:.1f} Mbit/s", bps / 1e6) : std::format(L"{} kbit/s", bps / 1000);
        }
    }

    int ChannelCount(const std::string& layout)
    {
        if (layout.empty())
            return 0;
        if (layout == "mono")
            return 1;
        if (layout == "stereo" || layout == "downmix")
            return 2;
        // "5.1", "5.1(side)", "7.1", "6 channels"
        int a = 0, b = 0;
        const char* p = layout.data();
        const char* end = p + layout.size();
        auto r = std::from_chars(p, end, a);
        if (r.ec != std::errc{})
            return 0;
        if (r.ptr < end && *r.ptr == '.')
            std::from_chars(r.ptr + 1, end, b);
        return a + b;
    }

    ProbeReport ParseProbeReport(const std::vector<std::string>& log)
    {
        ProbeReport report;
        Tags* target = nullptr;        // where "key : value" lines go
        size_t targetIndent = 0;       // indent of the "Metadata:" header
        enum class Last { Input, Chapter, Stream } last = Last::Input;
        for (const std::string_view line : log)
        {
            const size_t indent = Indent(line);
            const auto text = Trim(line);
            if (text.empty())
                continue;
            if (target && indent <= targetIndent)
                target = nullptr;   // the block ended

            if (text.starts_with("Input #0"))
            {
                report.ok = true;
                const auto from = text.find(", from '");
                const auto start = text.find(", ");
                if (start != std::string_view::npos && from != std::string_view::npos && from > start)
                    report.container = std::string(text.substr(start + 2, from - start - 2));
                last = Last::Input;
                continue;
            }
            if (text.starts_with("Input #") || text.starts_with("Output #"))
                break;   // only the first input matters
            if (!report.ok)
                continue;
            if (text == "Metadata:" || text == "Side data:")
            {
                if (last == Last::Input)
                    target = &report.tags;
                else if (last == Last::Chapter && !report.chapters.empty())
                    target = &report.chapters.back().tags;
                else if (last == Last::Stream && !report.streams.empty())
                    target = &report.streams.back().tags;
                targetIndent = indent;
                continue;
            }
            if (text.starts_with("Duration: "))
            {
                report.durationUs = ParseDurationLine(text);
                if (const double kbps = NumberBefore(text, " kb/s"); kbps > 0)
                    report.bitrate = static_cast<int64_t>(kbps * 1000.0);
                target = nullptr;
                continue;
            }
            if (text == "Chapters:")
            {
                target = nullptr;
                continue;
            }
            if (text.starts_with("Chapter #"))
            {
                ProbeChapter c;
                const auto s = text.find("start "), e = text.find(", end ");
                if (s != std::string_view::npos)
                    std::from_chars(text.data() + s + 6, text.data() + text.size(), c.start);
                if (e != std::string_view::npos)
                    std::from_chars(text.data() + e + 6, text.data() + text.size(), c.end);
                report.chapters.push_back(std::move(c));
                last = Last::Chapter;
                target = nullptr;
                continue;
            }
            if (text.starts_with("Stream #"))
            {
                report.streams.push_back(ParseStream(text));
                last = Last::Stream;
                target = nullptr;
                continue;
            }
            if (target)
            {
                const auto sep = text.find(": ");
                const auto key = Trim(text.substr(0, sep == std::string_view::npos ? text.size() : sep));
                const auto value = sep == std::string_view::npos ? std::string_view{} : Trim(text.substr(sep + 2));
                if (key.empty() || text.front() == ':')
                {
                    // A value that goes on over several lines: "                : more text"
                    if (!target->empty())
                        target->back().second += "\n" + std::string(Trim(text.substr(1)));
                }
                else
                {
                    target->emplace_back(std::string(key), std::string(value));
                    if (last == Last::Chapter && key == "title" && !report.chapters.empty())
                        report.chapters.back().title = std::string(value);
                }
            }
        }
        return report;
    }

    std::vector<Field> ProbeFields(const ProbeReport& r)
    {
        std::vector<Field> f;
        if (!r.ok)
            return f;
        f.push_back({ L"Media", L"Container", Text(r.container) });
        f.push_back({ L"Media", L"Duration", DurationText(r.durationUs) });
        if (r.bitrate > 0)
            f.push_back({ L"Media", L"Overall bitrate", Bitrate(r.bitrate) });
        int video = 0, audio = 0, subtitle = 0, other = 0;
        for (const auto& s : r.streams)
            (s.kind == "Video" && !s.attachedPic ? video : s.kind == "Audio" ? audio : s.kind == "Subtitle" ? subtitle : other)++;
        f.push_back({ L"Media", L"Tracks", std::format(L"{} video, {} audio, {} subtitle{}", video, audio, subtitle,
                                                       other ? std::format(L", {} other", other) : L"") });
        if (!r.chapters.empty())
            f.push_back({ L"Media", L"Chapters", std::to_wstring(r.chapters.size()) });

        int n[4] = {};
        for (const auto& s : r.streams)
        {
            const bool cover = s.kind == "Video" && s.attachedPic;
            const int k = cover ? 3 : s.kind == "Video" ? 0 : s.kind == "Audio" ? 1 : s.kind == "Subtitle" ? 2 : 3;
            const wchar_t* names[] = { L"Video track", L"Audio track", L"Subtitle track", L"Other track" };
            const auto group = std::format(L"{} {}", names[k], ++n[k]);
            f.push_back({ group, L"Codec", Text(s.codec) });
            if (cover)
                f.push_back({ group, L"Kind", L"Cover art" });
            if (s.width > 0)
                f.push_back({ group, L"Size", std::format(L"{} × {}", s.width, s.height) });
            if (s.fps > 0)
                f.push_back({ group, L"Frame rate", std::format(L"{:g} fps", s.fps) });
            if (s.sampleRate > 0)
                f.push_back({ group, L"Sample rate", std::format(L"{} Hz", s.sampleRate) });
            if (!s.channels.empty())
                f.push_back({ group, L"Channels", Text(s.channels) });
            if (s.bitrate > 0)
                f.push_back({ group, L"Bitrate", Bitrate(s.bitrate) });
            if (!s.language.empty())
                f.push_back({ group, L"Language", Text(s.language) });
            f.push_back({ group, L"Default", s.isDefault ? L"Yes" : L"No" });
            f.push_back({ group, L"Details", Text(s.details) });
            for (const auto& [k2, v] : s.tags)
                f.push_back({ group, Text(k2), Text(v) });
        }
        for (size_t i = 0; i < r.chapters.size(); ++i)
        {
            const auto& c = r.chapters[i];
            f.push_back({ L"Chapters", std::format(L"{}. {}", i + 1, c.title.empty() ? L"(untitled)" : Text(c.title)),
                          std::format(L"{} → {}", Seconds(c.start), Seconds(c.end)) });
        }
        for (const auto& [k, v] : r.tags)
            f.push_back({ L"Tags", Text(k), Text(v) });
        return f;
    }
}
