#include "mediaeditor/MediaProbe.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace sd::mediatools
{
    namespace
    {
        bool Has(std::string_view line, std::string_view what) { return line.find(what) != std::string_view::npos; }

        // The comma-separated fields of a stream line after "Audio: ".
        std::vector<std::string_view> Fields(std::string_view line, std::string_view key)
        {
            std::vector<std::string_view> fields;
            const auto at = line.find(key);
            if (at == std::string_view::npos)
                return fields;
            line.remove_prefix(at + key.size());
            // Commas inside parentheses ("yuv420p(tv, bt709)") do not split.
            int depth = 0;
            size_t start = 0;
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '(')
                    ++depth;
                else if (line[i] == ')')
                    depth = std::max(0, depth - 1);
                else if (line[i] == ',' && depth == 0)
                {
                    fields.push_back(line.substr(start, i - start));
                    start = i + 1;
                }
            }
            fields.push_back(line.substr(start));
            for (auto& f : fields)
            {
                while (!f.empty() && f.front() == ' ')
                    f.remove_prefix(1);
                while (!f.empty() && f.back() == ' ')
                    f.remove_suffix(1);
            }
            return fields;
        }
    }

    int ChannelsOfLayout(std::string_view layout)
    {
        if (const auto paren = layout.find('('); paren != std::string_view::npos)
            layout = layout.substr(0, paren);   // "5.1(side)"
        struct Known { std::string_view name; int channels; };
        static constexpr Known kKnown[] = { { "mono", 1 }, { "stereo", 2 }, { "2.1", 3 }, { "3.0", 3 }, { "quad", 4 }, { "4.0", 4 },
                                            { "4.1", 5 }, { "5.0", 5 }, { "5.1", 6 }, { "6.0", 6 }, { "6.1", 7 }, { "7.0", 7 },
                                            { "7.1", 8 }, { "downmix", 2 } };
        for (const auto& k : kKnown)
            if (layout == k.name)
                return k.channels;
        // "3 channels"
        int n = 0;
        const auto r = std::from_chars(layout.data(), layout.data() + layout.size(), n);
        if (r.ec == std::errc{} && n > 0 && std::string_view(r.ptr, static_cast<size_t>(layout.data() + layout.size() - r.ptr)).starts_with(" channel"))
            return n;
        return 0;
    }

    ProbeInfo ParseProbe(const std::vector<std::string>& log)
    {
        ProbeInfo info;
        info.media = ParseMediaInfo(log);
        bool inVideo = false;   // the side data of the first video stream follows its stream line
        bool sawVideo = false;
        for (const std::string_view line : log)
        {
            if (Has(line, "Stream #0:"))
            {
                inVideo = false;
                if (Has(line, "Video: ") && Has(line, "(attached pic)"))
                    info.coverArt = true;
                else if (Has(line, "Video: ") && !sawVideo)
                {
                    sawVideo = true;
                    inVideo = true;
                    if (const auto sar = line.find("[SAR "); sar != std::string_view::npos)
                    {
                        int num = 0, den = 0;
                        const char* p = line.data() + sar + 5;
                        const char* end = line.data() + line.size();
                        auto r = std::from_chars(p, end, num);
                        if (r.ec == std::errc{} && r.ptr < end && *r.ptr == ':')
                        {
                            r = std::from_chars(r.ptr + 1, end, den);
                            if (r.ec == std::errc{} && num > 0 && den > 0)
                                info.sar = static_cast<double>(num) / den;
                        }
                    }
                }
                else if (Has(line, "Audio: ") && info.channels == 0)
                {
                    // "aac (LC) (mp4a / 0x6134706D), 44100 Hz, stereo, fltp, 191 kb/s"
                    for (const auto field : Fields(line, "Audio: "))
                    {
                        if (field.ends_with(" Hz"))
                            std::from_chars(field.data(), field.data() + field.size(), info.sampleRate);
                        else if (info.channels == 0)
                            info.channels = ChannelsOfLayout(field);
                    }
                }
                continue;
            }
            // "      Display Matrix: rotation of -90.00 degrees" (older builds: "displaymatrix: rotation of").
            if (inVideo)
                if (const auto at = line.find("rotation of "); at != std::string_view::npos)
                {
                    double degrees = 0;
                    const char* p = line.data() + at + 12;
                    if (std::from_chars(p, line.data() + line.size(), degrees).ec == std::errc{})
                    {
                        const int turned = static_cast<int>(std::lround(degrees / 90.0)) * 90;
                        info.rotation = ((turned % 360) + 360) % 360;
                    }
                }
        }
        info.width = info.media.width;
        info.height = info.media.height;
        if (info.rotation == 90 || info.rotation == 270)
            std::swap(info.width, info.height);
        return info;
    }

    ProbeInfo Probe(const FfmpegEngine& engine, const std::wstring& path, std::stop_token stop)
    {
        std::vector<std::string> log;
        engine.Run(ProbeArgs(path), nullptr, stop, &log);   // exits with 1 by design: no output is named
        return ParseProbe(log);
    }
}
