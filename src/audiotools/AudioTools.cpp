#include "audiotools/AudioTools.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <map>

namespace sd::audiotools
{
    using mediatools::Num;
    using mediatools::Seconds;

    namespace
    {
        std::vector<std::wstring> Join(std::vector<std::wstring> a, const std::vector<std::wstring>& b)
        {
            a.insert(a.end(), b.begin(), b.end());
            return a;
        }

        // The codec, rate and muxer of the source's own format.
        std::vector<std::wstring> SameFormat(const std::wstring& ext, const ProbeInfo& info)
        {
            return mediatools::AudioCodecArgs(mediatools::AudioOutputFormat(ext), info.sampleRate);
        }

        bool ParseNumber(std::string_view text, double& value)
        {
            if (text.starts_with('+'))
                text.remove_prefix(1);
            const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
            return r.ec == std::errc{} && r.ptr == text.data() + text.size();
        }
    }

    // ---- 16. Normalize ---------------------------------------------------------------------------

    LoudnessTarget TargetFor(double integrated)
    {
        LoudnessTarget t;
        t.integrated = std::clamp(integrated, -70.0, -5.0);
        t.truePeak = -1;
        // Broadcast material is allowed a wider loudness range than streaming and podcasts.
        t.range = integrated <= -20 ? 20 : 11;
        return t;
    }

    std::optional<Loudness> ParseLoudnorm(const std::vector<std::string>& log)
    {
        // The block looks like:  {  "input_i" : "-27.61",  "input_tp" : "-4.47", ... }
        std::map<std::string, std::string> values, block;
        for (const std::string_view line : log)
        {
            const auto q1 = line.find('"');
            if (q1 == std::string_view::npos)
                continue;
            const auto q2 = line.find('"', q1 + 1);
            const auto colon = q2 == std::string_view::npos ? q2 : line.find(':', q2);
            const auto q3 = colon == std::string_view::npos ? colon : line.find('"', colon);
            const auto q4 = q3 == std::string_view::npos ? q3 : line.find('"', q3 + 1);
            if (q4 == std::string_view::npos)
                continue;
            const std::string key(line.substr(q1 + 1, q2 - q1 - 1));
            if (key == "input_i")
                block.clear();   // a new block starts: keep only the last one
            block[key] = std::string(line.substr(q3 + 1, q4 - q3 - 1));
            if (key == "target_offset")
                values = block;
        }
        if (values.empty())
            values = block;
        if (!values.contains("input_i"))
            return std::nullopt;
        Loudness l;
        const auto get = [&](const char* key, double& out) {
            const auto it = values.find(key);
            if (it == values.end())
                return false;
            if (it->second.find("inf") != std::string::npos)
            {
                out = it->second.starts_with('-') ? -99.0 : 99.0;
                return false;
            }
            return ParseNumber(it->second, out);
        };
        if (!get("input_i", l.inputI))
            l.silent = true;
        get("input_tp", l.inputTP);
        get("input_lra", l.inputLRA);
        get("input_thresh", l.inputThresh);
        get("output_i", l.outputI);
        get("output_tp", l.outputTP);
        get("output_lra", l.outputLRA);
        get("output_thresh", l.outputThresh);
        get("target_offset", l.targetOffset);
        return l;
    }

    std::optional<Loudness> ParseEbur128(const std::vector<std::string>& log)
    {
        // The summary looks like:  "... Summary:", "  Integrated loudness:", "    I:  -19.8 LUFS",
        // "    Threshold: -30.0 LUFS", "  Loudness range:", "    LRA:  4.5 LU", "    Threshold: ...",
        // "  True peak:", "    Peak:  -2.3 dBFS". Only the last summary counts.
        size_t start = log.size();
        for (size_t i = 0; i < log.size(); ++i)
            if (log[i].find("Summary:") != std::string::npos)
                start = i;
        const auto value = [](std::string_view line, std::string_view key, double& out) {
            const auto at = line.find(key);
            if (at == std::string_view::npos)
                return false;
            auto rest = line.substr(at + key.size());
            while (!rest.empty() && rest.front() == ' ')
                rest.remove_prefix(1);
            if (rest.starts_with("-inf"))
            {
                out = -99;
                return true;
            }
            return ParseNumber(rest.substr(0, rest.find(' ')), out);
        };
        Loudness l;
        bool haveI = false;
        char section = 0;   // 'I', 'R' (range) or 'P' (true peak)
        for (size_t i = start + 1; i < log.size(); ++i)
        {
            const std::string_view line = log[i];
            if (line.find("Integrated loudness:") != std::string_view::npos)
                section = 'I';
            else if (line.find("Loudness range:") != std::string_view::npos)
                section = 'R';
            else if (line.find("True peak:") != std::string_view::npos)
                section = 'P';
            else if (section == 'I' && value(line, "I:", l.inputI))
                haveI = true;
            else if (section == 'I')
                value(line, "Threshold:", l.inputThresh);
            else if (section == 'R')
                value(line, "LRA:", l.inputLRA);
            else if (section == 'P')
                value(line, "Peak:", l.inputTP);
        }
        if (!haveI)
            return std::nullopt;
        l.silent = l.inputI <= -70;   // ebur128's floor: nothing passed the absolute gate
        return l;
    }

    std::wstring MeasureFilter(const LoudnessTarget&)
    {
        return L"ebur128=peak=true:framelog=quiet";
    }

    bool IsLinear(const LoudnessTarget& t, const Loudness& m)
    {
        // loudnorm's own rule for its linear mode: the gain must not lift the true peak over the target.
        return m.inputTP + (t.integrated - m.inputI) <= t.truePeak;
    }

    std::wstring ApplyFilter(const LoudnessTarget& t, const Loudness& m)
    {
        if (IsLinear(t, m))
            return std::format(L"volume={}dB", Num(std::round((t.integrated - m.inputI) * 100) / 100));
        // The peaks would go over: loudnorm limits them. linear=true only works when the allowed range
        // covers the measured one; widening the range to the measured one keeps the music's dynamics
        // as untouched as the peaks allow.
        const double lra = std::clamp(std::max(t.range, std::ceil(m.inputLRA * 10 + 1) / 10), 1.0, 50.0);
        return std::format(L"loudnorm=I={}:TP={}:LRA={}:measured_I={}:measured_TP={}:measured_LRA={}:measured_thresh={}:offset={}:linear=true:print_format=json",
                           Num(t.integrated), Num(t.truePeak), Num(lra), Num(std::clamp(m.inputI, -99.0, 0.0)), Num(std::clamp(m.inputTP, -99.0, 99.0)),
                           Num(std::clamp(m.inputLRA, 0.0, 99.0)), Num(std::clamp(m.inputThresh, -99.0, 0.0)), Num(std::clamp(m.targetOffset, -99.0, 99.0)));
    }

    std::vector<std::wstring> MeasureArgs(const std::wstring& input, const LoudnessTarget& target)
    {
        // The null muxer writes nothing; the measurement is in the log.
        return FfmpegArgs(input, {}, { L"-map", L"0:a:0", L"-vn", L"-sn", L"-dn", L"-af", MeasureFilter(target), L"-f", L"null" }, L"-");
    }

    std::vector<std::wstring> NormalizeArgs(const std::wstring& input, const std::wstring& ext, const ProbeInfo& info, const LoudnessTarget& target,
                                            const Loudness& measured, const std::wstring& output)
    {
        return FfmpegArgs(input, {}, Join({ L"-map", L"0:a:0", L"-vn", L"-sn", L"-dn", L"-af", ApplyFilter(target, measured) }, SameFormat(ext, info)),
                          output);
    }

    std::wstring LoudnessNote(double before, double after)
    {
        auto text = std::format(L"{:.1f} \u2192 {:.1f} LUFS", before, after);
        std::replace(text.begin(), text.end(), L'-', L'\u2212');   // a real minus sign
        return text;
    }

    // ---- 17. Visualizer --------------------------------------------------------------------------

    Shape ShapeByName(std::string_view name)
    {
        if (name == "portrait") return Shape::Portrait;
        if (name == "square") return Shape::Square;
        return Shape::Landscape;
    }

    void ShapeSize(Shape shape, int& w, int& h)
    {
        switch (shape)
        {
        case Shape::Portrait: w = 1080, h = 1920; break;
        case Shape::Square: w = 1080, h = 1080; break;
        default: w = 1920, h = 1080; break;
        }
    }

    namespace
    {
        std::vector<std::wstring> VideoTail(const ProbeInfo& info, int w, int h, H264Encoder encoder, int audioInput)
        {
            std::vector<std::wstring> a{ L"-map", L"[v]", L"-map", std::format(L"{}:a:0", audioInput) };
            const auto v = mediatools::EditVideoArgs(encoder, w, h, 30);
            a.insert(a.end(), v.begin(), v.end());
            if (info.media.audioCodec == "aac")
                a.insert(a.end(), { L"-c:a", L"copy" });
            else
                a.insert(a.end(), { L"-c:a", L"aac", L"-b:a", L"192k" });
            // Exactly as long as the sound, whatever the picture source does.
            if (info.DurationUs() > 0)
                a.insert(a.end(), { L"-t", Seconds(info.DurationUs()) });
            a.insert(a.end(), { L"-shortest", L"-movflags", L"+faststart", L"-f", L"mp4" });
            return a;
        }
    }

    VisualStyle VisualStyleByName(std::string_view name)
    {
        if (name == "bars") return VisualStyle::Bars;
        if (name == "circle") return VisualStyle::Circle;
        if (name == "square") return VisualStyle::Square;
        if (name == "vertical") return VisualStyle::Vertical;
        if (name == "image") return VisualStyle::Picture;
        return VisualStyle::Wave;
    }

    const char* VisualStyleName(VisualStyle style)
    {
        switch (style)
        {
        case VisualStyle::Bars: return "bars";
        case VisualStyle::Circle: return "circle";
        case VisualStyle::Square: return "square";
        case VisualStyle::Vertical: return "vertical";
        case VisualStyle::Picture: return "image";
        default: return "waveform";
        }
    }

    namespace
    {
        constexpr const wchar_t* kVisualColour = L"0x7AB8FF";   // a soft blue on the near-black 0x0E0F13

        // showfreqs drawn small, `cols` bins by `rows` levels, then enlarged with hard edges to
        // `pitch` pixels a bin (and `rowPitch` a level; 0 = whole bars), and a transparent gap of
        // `gap` cut between the bins (and levels).
        std::wstring FreqBlocks(const std::wstring& in, int cols, int rows, int pitch, int rowPitch, int gap)
        {
            const int outH = rowPitch > 0 ? rows * rowPitch : rows;
            const std::wstring gridH = rowPitch > 0 ? std::to_wstring(rowPitch) : std::wstring(L"ih+1");
            // The square root scale lifts quiet bins; the log frequency scale gives the bass room.
            return std::format(L"[{}]aformat=channel_layouts=mono,showfreqs=s={}x{}:mode=bar:ascale=sqrt:fscale=log:win_size=4096:colors={}:rate=30,"
                               L"scale={}:{}:flags=neighbor,drawgrid=w={}:h={}:t={}:c=black@0:replace=1",
                               in, cols, rows, kVisualColour, cols * pitch, outH, pitch, gridH, gap);
        }
    }

    std::wstring VisualGraph(VisualStyle style, int w, int h, const std::wstring& in)
    {
        switch (style)
        {
        case VisualStyle::Bars:
            return FreqBlocks(in, std::max(16, w * 5 / 6 / 20), (h / 2) & ~1, 20, 0, 4) + L"[vis]";
        case VisualStyle::Square:
            return FreqBlocks(in, std::max(8, w * 5 / 6 / 40), std::max(6, h * 2 / 3 / 40), 40, 40, 6) + L"[vis]";
        case VisualStyle::Vertical:
            // The bars and their reflection, back to back on the middle line.
            return FreqBlocks(in, std::max(16, w * 5 / 6 / 20), (h / 4) & ~1, 20, 0, 4) + L",split=2[vt][vb];[vb]vflip[vf];[vt][vf]vstack[vis]";
        case VisualStyle::Circle:
        {
            // x = the sound, y = the sound shifted a quarter turn (its Hilbert transform, a 255-tap
            // FIR; the sound is delayed by the filter's 127 samples to stay in step). A tone traces a
            // circle as wide as it is loud; the low-pass keeps the lines smooth.
            const int size = std::min(w, h) & ~1;
            return std::format(L"[{}]aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=mono,lowpass=f=600,asplit=2[cx][cy];"
                               L"hilbert=sample_rate=44100:taps=255[ch];[cy][ch]afir=irnorm=-1[cyh];[cx]adelay=127S[cxd];"
                               L"[cxd][cyh]join=inputs=2:channel_layout=stereo,avectorscope=s={}x{}:mode=lissajous_xy:draw=line:scale=lin:zoom=1.3:"
                               L"rc=122:gc=184:bc=255:ac=255:rf=30:gf=20:bf=12:af=20:rate=30[vis]",
                               in, size, size);
        }
        case VisualStyle::Wave:
            // A centred line, filled from the middle; the square root scale keeps quiet passages moving.
            return std::format(L"[{}]aformat=channel_layouts=mono,showwaves=s={}x{}:mode=cline:rate=30:colors={}:scale=sqrt:draw=full[vis]", in, w,
                               (h / 2) & ~1, kVisualColour);
        default:
            return {};
        }
    }

    std::vector<std::wstring> VisualizerArgs(const std::wstring& audio, const std::wstring& image, const ProbeInfo& info, VisualStyle style,
                                             Shape shape, H264Encoder encoder, const std::wstring& output)
    {
        if (style == VisualStyle::Picture)
        {
            if (!image.empty())
                return StillVideoArgs(audio, image, info, shape, encoder, output);
            style = VisualStyle::Wave;   // no picture to show: the waveform instead
        }
        int w = 0, h = 0;
        ShapeSize(shape, w, h);
        const bool picture = !image.empty();
        // A picture fills the frame (cropped to its shape) and is darkened so the animation stands
        // out; it is read once a second (-loop 1 -framerate 1) and repeated up to 30 fps.
        std::wstring graph = picture ? std::format(L"[0:v]scale={0}:{1}:force_original_aspect_ratio=increase,crop={0}:{1},setsar=1,format=yuv420p,"
                                                   L"drawbox=c=black@0.45:t=fill,fps=30[bg];",
                                                   w, h)
                                     : std::format(L"color=c=0x0E0F13:s={}x{}:r=30[bg];", w, h);
        graph += VisualGraph(style, w, h, picture ? L"1:a:0" : L"0:a:0") + L";[bg][vis]overlay=x=(W-w)/2:y=(H-h)/2:shortest=1,format=yuv420p[v]";
        const auto out = Join({ L"-filter_complex", graph }, VideoTail(info, w, h, encoder, picture ? 1 : 0));
        if (picture)
            return mediatools::MultiInputArgs({ { image, { L"-loop", L"1", L"-framerate", L"1" } }, { audio, {} } }, out, output);
        return FfmpegArgs(audio, {}, out, output);
    }

    std::vector<std::wstring> StillVideoArgs(const std::wstring& audio, const std::wstring& image, const ProbeInfo& info, Shape shape,
                                             H264Encoder encoder, const std::wstring& output)
    {
        int w = 0, h = 0;
        ShapeSize(shape, w, h);
        // The picture is read once a second (-loop 1 -framerate 1) and repeated up to 30 fps: scaling it
        // 30 times a second for an hour of audio would be wasted work.
        const auto graph = std::format(L"[0:v]scale={0}:{1}:force_original_aspect_ratio=decrease:force_divisible_by=2,"
                                       L"pad={0}:{1}:(ow-iw)/2:(oh-ih)/2:color=black,setsar=1,format=yuv420p,fps=30[v]",
                                       w, h);
        return mediatools::MultiInputArgs({ { image, { L"-loop", L"1", L"-framerate", L"1" } }, { audio, {} } },
                                          Join({ L"-filter_complex", graph }, VideoTail(info, w, h, encoder, 1)), output);
    }

    std::vector<std::wstring> CoverArtArgs(const std::wstring& audio, const std::wstring& output)
    {
        return FfmpegArgs(audio, {}, { L"-map", L"0:v:0", L"-an", L"-sn", L"-dn", L"-frames:v", L"1", L"-c:v", L"png", L"-update", L"1", L"-f", L"image2" },
                          output);
    }

    // ---- 18. Trim --------------------------------------------------------------------------------

    std::vector<std::wstring> SilenceDetectArgs(const std::wstring& input, TimeRange range)
    {
        std::vector<std::wstring> in{ L"-ss", Seconds(range.start) };
        if (range.end > range.start)
            in.insert(in.end(), { L"-to", Seconds(range.end) });
        return FfmpegArgs(input, in,
                          { L"-map", L"0:a:0", L"-vn", L"-sn", L"-dn", L"-af",
                            std::format(L"silencedetect=noise={}dB:d={}", Num(kSilenceDb), Num(kSilenceMinUs / 1e6)), L"-f", L"null" },
                          L"-");
    }

    std::vector<TimeRange> ParseSilence(const std::vector<std::string>& log, int64_t offsetUs, int64_t endUs)
    {
        std::vector<TimeRange> out;
        bool open = false;
        int64_t start = 0;
        const auto number = [](std::string_view line, std::string_view key, double& v) {
            const auto at = line.find(key);
            if (at == std::string_view::npos)
                return false;
            auto rest = line.substr(at + key.size());
            while (!rest.empty() && rest.front() == ' ')
                rest.remove_prefix(1);
            const auto r = std::from_chars(rest.data(), rest.data() + rest.size(), v);
            return r.ec == std::errc{};
        };
        for (const std::string_view line : log)
        {
            double v = 0;
            if (number(line, "silence_start:", v))
            {
                open = true;
                start = offsetUs + static_cast<int64_t>(std::llround(std::max(0.0, v) * 1e6));
            }
            else if (number(line, "silence_end:", v) && open)
            {
                open = false;
                out.push_back({ start, offsetUs + static_cast<int64_t>(std::llround(v * 1e6)) });
            }
        }
        if (open)
            out.push_back({ start, endUs });
        for (auto& r : out)
            r.end = std::min(r.end, endUs);
        std::erase_if(out, [](const TimeRange& r) { return r.Length() <= 0; });
        return out;
    }

    TimeRange TrimSilence(TimeRange keep, const std::vector<TimeRange>& silences)
    {
        constexpr int64_t kTouch = 20'000;   // a silence this close to an end counts as at the end
        TimeRange r = keep;
        for (const auto& s : silences)
            if (s.start <= r.start + kTouch && s.end > r.start)
                r.start = std::max(r.start, s.end - kSilencePadUs);
        for (auto it = silences.rbegin(); it != silences.rend(); ++it)
            if (it->end >= r.end - kTouch && it->start < r.end)
                r.end = std::min(r.end, it->start + kSilencePadUs);
        if (r.Length() < mediatools::kMinRangeUs)
            return keep;   // all silence: leave it alone rather than produce nothing
        return r;
    }

    std::vector<std::wstring> TrimAudioArgs(const std::wstring& input, const std::wstring& ext, const ProbeInfo& info, TimeRange range,
                                            const std::wstring& output)
    {
        // Decoded and encoded again, so the cut is sample-exact (a copied MP3 could only cut on its
        // 26 ms frames).
        return FfmpegArgs(input, { L"-ss", Seconds(range.start), L"-to", Seconds(range.end) },
                          Join({ L"-map", L"0:a:0", L"-vn", L"-sn", L"-dn" }, SameFormat(ext, info)), output);
    }

    // ---- 19. Channels ----------------------------------------------------------------------------

    std::wstring PanFilter(const ChannelMix& mix, int channels)
    {
        const double l = std::clamp(mix.left, 0.0, 2.0), r = std::clamp(mix.right, 0.0, 2.0);
        // Surround sources are mixed down to stereo first (ffmpeg's standard downmix), then panned.
        const std::wstring prefix = channels > 2 ? L"aformat=channel_layouts=stereo," : L"";
        std::wstring srcL = L"c0", srcR = channels == 1 ? L"c0" : L"c1";
        if (mix.swap)
            std::swap(srcL, srcR);
        if (mix.mono)
        {
            if (channels == 1)
                return std::format(L"pan=mono|c0={}*c0", Num((l + r) / 2));
            return prefix + std::format(L"pan=mono|c0={}*{}+{}*{}", Num(l / 2), srcL, Num(r / 2), srcR);
        }
        return prefix + std::format(L"pan=stereo|c0={}*{}|c1={}*{}", Num(l), srcL, Num(r), srcR);
    }

    std::vector<std::wstring> ChannelsArgs(const std::wstring& input, const std::wstring& ext, const ProbeInfo& info, const ChannelMix& mix,
                                           const std::wstring& output)
    {
        return FfmpegArgs(input, {}, Join({ L"-map", L"0:a:0", L"-vn", L"-sn", L"-dn", L"-af", PanFilter(mix, info.channels) }, SameFormat(ext, info)),
                          output);
    }

    // ---- 20. Bleep -------------------------------------------------------------------------------

    std::wstring AnyBetween(const std::vector<TimeRange>& ranges, int64_t offsetUs)
    {
        std::wstring e;
        for (const auto& r : ranges)
        {
            if (r.end - offsetUs <= 0)
                continue;
            e += (e.empty() ? L"" : L"+") + std::format(L"between(t,{},{})", Seconds(r.start - offsetUs), Seconds(r.end - offsetUs));
        }
        return e.empty() ? L"0" : e;
    }

    BleepSound BleepSoundByName(std::string_view name)
    {
        if (name == "silence") return BleepSound::Silence;
        if (name == "duck") return BleepSound::Duck;
        return BleepSound::Tone;
    }

    std::wstring BleepGraph(const std::vector<TimeRange>& ranges, BleepSound sound, double duckDb, int sampleRate, int64_t offsetUs)
    {
        const auto any = AnyBetween(ranges, offsetUs);
        if (sound == BleepSound::Silence)
            return std::format(L"[0:a]volume=0:enable='{}'[out]", any);
        if (sound == BleepSound::Duck)
            return std::format(L"[0:a]volume={}dB:enable='{}'[out]", Num(-std::clamp(duckDb, kMinDuckDb, kMaxDuckDb)), any);
        // The sound is muted in the ranges and a 1 kHz tone (about -10 dBFS) is heard only there.
        return std::format(L"[0:a]volume=0:enable='{0}'[m];"
                           L"sine=frequency=1000:sample_rate={1},volume=2.5,volume=0:enable='not({0})'[t];"
                           L"[m][t]amix=inputs=2:duration=first:normalize=0[out]",
                           any, sampleRate > 0 ? sampleRate : 48000);
    }

    std::vector<std::wstring> BleepArgs(const std::wstring& input, const std::wstring& ext, const ProbeInfo& info, const std::vector<TimeRange>& ranges,
                                        BleepSound sound, double duckDb, const std::wstring& output)
    {
        return FfmpegArgs(input, {},
                          Join({ L"-filter_complex", BleepGraph(ranges, sound, duckDb, info.sampleRate), L"-map", L"[out]", L"-vn", L"-sn", L"-dn" },
                               SameFormat(ext, info)),
                          output);
    }
}
