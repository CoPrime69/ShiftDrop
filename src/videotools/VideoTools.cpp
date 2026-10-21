#include "videotools/VideoTools.h"
#include "Utf.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace sd::videotools
{
    using mediatools::Seconds;

    namespace
    {
        // 4:2:0 H.264 needs even sides: drop the odd row or column rather than scale.
        constexpr const wchar_t* kEven = L"crop=trunc(iw/2)*2:trunc(ih/2)*2";

        double KeptFps(double fps) { return fps > 1 ? std::min(fps, 60.0) : 0.0; }

        void AddMuxer(std::vector<std::wstring>& a, const Container& c)
        {
            if (c.muxer == L"mp4" || c.muxer == L"mov")
                a.insert(a.end(), { L"-movflags", L"+faststart" });
            a.insert(a.end(), { L"-f", c.muxer });
        }
    }

    std::vector<std::wstring> EncodeOutput(const Container& container, const ProbeInfo& info, H264Encoder encoder,
                                           const std::wstring& videoFilter, int outW, int outH, double fps, const std::wstring& audioFilter)
    {
        std::vector<std::wstring> a{ L"-map", L"0:v:0" };
        if (info.HasAudio())
            a.insert(a.end(), { L"-map", L"0:a?" });
        a.insert(a.end(), { L"-sn", L"-dn", L"-vf", videoFilter.empty() ? std::wstring(kEven) : videoFilter + L"," + kEven });
        const auto v = mediatools::EditVideoArgs(encoder, outW, outH, fps);
        a.insert(a.end(), v.begin(), v.end());
        if (!info.HasAudio())
            a.push_back(L"-an");
        else if (!audioFilter.empty())
            a.insert(a.end(), { L"-af", audioFilter, L"-c:a", L"aac", L"-b:a", L"192k" });
        else
        {
            const auto s = mediatools::SoundtrackArgs(container, info);
            a.insert(a.end(), s.begin(), s.end());
        }
        AddMuxer(a, container);
        return a;
    }

    // ---- 9. Trim ---------------------------------------------------------------------------------

    Container TrimContainer(const std::wstring& ext, bool fast)
    {
        return fast ? mediatools::CopyContainer(ext) : mediatools::EncodeContainer(ext);
    }

    bool TrimCopies(bool fast, const TrimChoice& choice) { return fast && !choice.remove && !choice.fade; }

    std::vector<TimeRange> TrimParts(TimeRange selection, int64_t durationUs, bool remove)
    {
        if (!remove)
            return { selection };
        std::vector<TimeRange> parts;
        if (selection.start >= mediatools::kMinRangeUs)
            parts.push_back({ 0, selection.start });
        if (durationUs - selection.end >= mediatools::kMinRangeUs)
            parts.push_back({ selection.end, durationUs });
        return parts;
    }

    int64_t TrimmedLength(TimeRange selection, int64_t durationUs, bool remove)
    {
        int64_t total = 0;
        for (const auto& p : TrimParts(selection, durationUs, remove))
            total += p.Length();
        return total;
    }

    std::wstring FadeFilter(bool audio, int64_t lengthUs, bool fadeIn, bool fadeOut)
    {
        // A fade never takes more than a third of its part, so a short part still shows.
        const int64_t d = std::min(kFadeUs, std::max<int64_t>(0, lengthUs) / 3);
        if (d <= 0)
            return {};
        const wchar_t* name = audio ? L"afade" : L"fade";
        std::wstring f;
        if (fadeIn)
            f = std::format(L"{}=t=in:st=0:d={}", name, Seconds(d));
        if (fadeOut)
            f += (f.empty() ? L"" : L",") + std::format(L"{}=t=out:st={}:d={}", name, Seconds(lengthUs - d), Seconds(d));
        return f;
    }

    std::vector<std::wstring> TrimArgs(const std::wstring& input, const std::wstring& ext, const ProbeInfo& info, TimeRange range, bool fast,
                                       H264Encoder encoder, const std::wstring& output, const TrimChoice& choice)
    {
        const bool copy = TrimCopies(fast, choice);
        const auto container = TrimContainer(ext, copy);
        const int64_t duration = info.DurationUs() > 0 ? info.DurationUs() : range.end;
        // -ss/-to on the input: ffmpeg seeks instead of decoding up to the start. With copied streams
        // it lands on the keyframe at or before the start; when decoding it is frame-exact.
        const auto seek = [&](TimeRange part) {
            std::vector<std::wstring> in;
            if (!copy)
                in = mediatools::DecodeArgs(encoder);
            in.insert(in.end(), { L"-ss", Seconds(part.start), L"-to", Seconds(part.end) });
            return in;
        };
        if (copy)
        {
            std::vector<std::wstring> out{ L"-map", L"0:v:0?", L"-map", L"0:a?", L"-c", L"copy", L"-sn", L"-dn", L"-avoid_negative_ts", L"make_zero" };
            if (info.media.videoCodec == "hevc" && (container.muxer == L"mp4" || container.muxer == L"mov"))
                out.insert(out.end(), { L"-tag:v", L"hvc1" });
            AddMuxer(out, container);
            return FfmpegArgs(input, seek(range), out, output);
        }
        auto parts = TrimParts(range, duration, choice.remove);
        if (parts.empty())
            parts = { range };
        // A cut is wherever a part does not begin at the start or end at the end of the video.
        const auto fades = [&](const TimeRange& part, bool audio) {
            return choice.fade ? FadeFilter(audio, part.Length(), part.start > 0, part.end < duration) : std::wstring();
        };
        if (parts.size() == 1)
        {
            const auto& part = parts.front();
            return FfmpegArgs(input, seek(part),
                              EncodeOutput(container, info, encoder, fades(part, false), info.width, info.height, info.media.fps,
                                           info.HasAudio() ? fades(part, true) : std::wstring()),
                              output);
        }
        // The parts before and after a removed one: each read through its own seek (so nothing before
        // a part is decoded), faded at the cut if asked, and joined by the concat filter.
        const bool audio = info.HasAudio();
        std::vector<mediatools::Input> inputs;
        std::wstring graph, chain;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            inputs.push_back({ input, seek(parts[i]) });
            const auto v = fades(parts[i], false);
            graph += std::format(L"[{0}:v:0]{1}[v{0}];", i, v.empty() ? L"null" : v);
            chain += std::format(L"[v{}]", i);
            if (audio)
            {
                const auto a = fades(parts[i], true);
                graph += std::format(L"[{0}:a:0]{1}[a{0}];", i, a.empty() ? L"anull" : a);
                chain += std::format(L"[a{}]", i);
            }
        }
        graph += chain + std::format(L"concat=n={}:v=1:a={}[joined]", parts.size(), audio ? 1 : 0) + (audio ? L"[a]" : L"") + L";[joined]" + kEven + L"[v]";
        std::vector<std::wstring> out{ L"-filter_complex", graph, L"-map", L"[v]" };
        if (audio)
            out.insert(out.end(), { L"-map", L"[a]" });
        out.insert(out.end(), { L"-sn", L"-dn" });
        const auto v = mediatools::EditVideoArgs(encoder, info.width, info.height, info.media.fps);
        out.insert(out.end(), v.begin(), v.end());
        if (audio)
            out.insert(out.end(), { L"-c:a", L"aac", L"-b:a", L"192k" });
        else
            out.push_back(L"-an");
        AddMuxer(out, container);
        return mediatools::MultiInputArgs(inputs, out, output);
    }

    // ---- 10. Crop --------------------------------------------------------------------------------

    std::wstring CropFilter(const PixelRect& box, int outW, int outH)
    {
        std::wstring f = std::format(L"crop={}:{}:{}:{}", box.w, box.h, box.x, box.y);
        if (outW > 0 && outH > 0 && (outW != box.w || outH != box.h))
            f += std::format(L",scale={}:{}:flags=lanczos,setsar=1", outW, outH);
        return f;
    }

    std::vector<std::wstring> CropArgs(const std::wstring& input, const std::wstring& ext, const ProbeInfo& info, const PixelRect& box, int outW,
                                       int outH, H264Encoder encoder, const std::wstring& output)
    {
        const auto container = mediatools::EncodeContainer(ext);
        return FfmpegArgs(input, mediatools::DecodeArgs(encoder),
                          EncodeOutput(container, info, encoder, CropFilter(box, outW, outH), outW, outH, info.media.fps), output);
    }

    // ---- 11. Speed -------------------------------------------------------------------------------

    std::wstring AtempoChain(double factor)
    {
        factor = std::clamp(factor, kMinSpeed, kMaxSpeed);
        std::vector<double> steps;
        while (factor > 2.0 + 1e-9)
        {
            steps.push_back(2.0);
            factor /= 2.0;
        }
        while (factor < 0.5 - 1e-9)
        {
            steps.push_back(0.5);
            factor /= 0.5;
        }
        steps.push_back(factor);
        std::wstring chain;
        for (const double s : steps)
            chain += (chain.empty() ? L"" : L",") + std::format(L"atempo={}", Num(s));
        return chain;
    }

    std::wstring SpeedLabel(double factor) { return Num(factor) + L"x"; }

    std::wstring SpeedAudioFilter(double factor, bool keepPitch, int sampleRate)
    {
        factor = std::clamp(factor, kMinSpeed, kMaxSpeed);
        if (keepPitch || sampleRate <= 0)
            return AtempoChain(factor);
        // Read the samples as if recorded at a faster (or slower) rate, then resample back.
        return std::format(L"asetrate={},aresample={}", std::llround(sampleRate * factor), sampleRate);
    }

    std::vector<std::wstring> SpeedArgs(const std::wstring& input, const std::wstring& ext, const ProbeInfo& info, double factor,
                                        H264Encoder encoder, const std::wstring& output, bool keepPitch)
    {
        factor = std::clamp(factor, kMinSpeed, kMaxSpeed);
        const auto container = mediatools::EncodeContainer(ext);
        // New timestamps, then back to the source's frame rate: a 2x clip drops every other frame
        // instead of playing at 60 fps, a 0.5x clip repeats frames.
        std::wstring filter = std::format(L"setpts=PTS/{}", Num(factor));
        const double fps = KeptFps(info.media.fps);
        if (fps > 0)
            filter += std::format(L",fps={}", FpsExpr(fps));
        return FfmpegArgs(input, mediatools::DecodeArgs(encoder),
                          EncodeOutput(container, info, encoder, filter, info.width, info.height, info.media.fps,
                                       info.HasAudio() ? SpeedAudioFilter(factor, keepPitch, info.sampleRate) : std::wstring()),
                          output);
    }

    // ---- 12. Join --------------------------------------------------------------------------------

    bool CanJoinByCopy(const std::vector<ProbeInfo>& clips)
    {
        if (clips.empty())
            return false;
        const auto& a = clips.front();
        // MP4 out: the codecs must be ones MP4 holds.
        if (a.media.videoCodec != "h264" && a.media.videoCodec != "hevc")
            return false;
        if (a.HasAudio() && a.media.audioCodec != "aac" && a.media.audioCodec != "mp3")
            return false;
        for (const auto& b : clips)
        {
            if (b.media.videoCodec != a.media.videoCodec || b.media.width != a.media.width || b.media.height != a.media.height ||
                b.rotation != a.rotation || std::abs(b.media.fps - a.media.fps) > 0.01 || b.media.audioCodec != a.media.audioCodec ||
                b.channels != a.channels || b.sampleRate != a.sampleRate || b.media.durationUs <= 0)
                return false;
        }
        return true;
    }

    std::string EscapeConcatPath(const std::string& path)
    {
        // Inside '...' everything is literal except the quote itself, which closes the string: write
        // it as '\'' (close, an escaped quote, reopen).
        std::string out = "'";
        for (const char c : path)
        {
            if (c == '\'')
                out += "'\\''";
            else
                out += c;
        }
        return out + "'";
    }

    std::string ConcatList(const std::vector<std::wstring>& paths)
    {
        std::string list = "ffconcat version 1.0\n";
        for (const auto& p : paths)
            list += "file " + EscapeConcatPath(Narrow(p)) + "\n";
        return list;
    }

    std::vector<std::wstring> JoinCopyArgs(const std::wstring& listFile, const ProbeInfo& first, const std::wstring& output)
    {
        // -safe 0: the list names absolute paths. The demuxer opens them through the same file-only
        // protocol whitelist as the list itself.
        std::vector<std::wstring> out{ L"-map", L"0:v:0", L"-map", L"0:a?", L"-c", L"copy", L"-sn", L"-dn" };
        if (first.media.videoCodec == "hevc")
            out.insert(out.end(), { L"-tag:v", L"hvc1" });
        out.insert(out.end(), { L"-movflags", L"+faststart", L"-f", L"mp4" });
        return FfmpegArgs(listFile, { L"-f", L"concat", L"-safe", L"0" }, out, output);
    }

    void JoinCanvas(const ProbeInfo& first, int& w, int& h, double& fps)
    {
        w = std::max(16, (first.width > 0 ? first.width : 1280) & ~1);
        h = std::max(16, (first.height > 0 ? first.height : 720) & ~1);
        fps = first.media.fps > 1 ? std::min(first.media.fps, 60.0) : 30.0;
    }

    std::vector<std::wstring> JoinEncodeArgs(const std::vector<std::wstring>& inputs, const std::vector<ProbeInfo>& clips, H264Encoder encoder,
                                             const std::wstring& output, bool fill)
    {
        int w = 0, h = 0;
        double fps = 30;
        JoinCanvas(clips.front(), w, h, fps);
        const bool anyAudio = std::any_of(clips.begin(), clips.end(), [](const ProbeInfo& c) { return c.HasAudio(); });
        std::wstring graph, chain;
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            // Fit inside the canvas, centred on black, or fill it and crop the overflow from the middle;
            // the same rate and pixel format for every clip.
            graph += fill ? std::format(L"[{0}:v:0]scale={1}:{2}:force_original_aspect_ratio=increase,"
                                        L"crop={1}:{2},setsar=1,fps={3},format=yuv420p[v{0}];",
                                        i, w, h, FpsExpr(fps))
                          : std::format(L"[{0}:v:0]scale={1}:{2}:force_original_aspect_ratio=decrease:force_divisible_by=2,"
                                        L"pad={1}:{2}:(ow-iw)/2:(oh-ih)/2:color=black,setsar=1,fps={3},format=yuv420p[v{0}];",
                                        i, w, h, FpsExpr(fps));
            chain += std::format(L"[v{}]", i);
            if (anyAudio)
            {
                if (clips[i].HasAudio())
                    graph += std::format(L"[{0}:a:0]aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo[a{0}];", i);
                else   // a silent clip gets silence as long as itself
                    graph += std::format(L"anullsrc=channel_layout=stereo:sample_rate=48000,atrim=duration={1}[a{0}];", i,
                                         Seconds(std::max<int64_t>(clips[i].DurationUs(), 1000)));
                chain += std::format(L"[a{}]", i);
            }
        }
        graph += chain + std::format(L"concat=n={}:v=1:a={}[v]", inputs.size(), anyAudio ? 1 : 0) + (anyAudio ? L"[a]" : L"");
        std::vector<std::wstring> out{ L"-filter_complex", graph, L"-map", L"[v]" };
        if (anyAudio)
            out.insert(out.end(), { L"-map", L"[a]" });
        const auto v = mediatools::EditVideoArgs(encoder, w, h, fps);
        out.insert(out.end(), v.begin(), v.end());
        if (anyAudio)
            out.insert(out.end(), { L"-c:a", L"aac", L"-b:a", L"192k" });
        out.insert(out.end(), { L"-movflags", L"+faststart", L"-f", L"mp4" });
        std::vector<mediatools::Input> in;
        for (const auto& p : inputs)
            in.push_back({ p, mediatools::DecodeArgs(encoder) });
        return mediatools::MultiInputArgs(in, out, output);
    }

    // ---- 13. Frames ------------------------------------------------------------------------------

    std::vector<std::wstring> FrameToPngArgs(const std::wstring& input, int64_t us, double fps, const std::wstring& output)
    {
        // -update 1: the name is a plain file name, not an image-sequence pattern (a '%' in it is fine).
        return FfmpegArgs(input, { L"-ss", Seconds(mediatools::FrameSeekUs(us, fps)) },
                          { L"-map", L"0:v:0", L"-an", L"-sn", L"-dn", L"-frames:v", L"1", L"-c:v", L"png", L"-update", L"1", L"-f", L"image2" }, output);
    }

    std::wstring FrameFileName(const std::wstring& stem, int64_t us, const std::wstring& extension)
    {
        const int64_t ms = (std::max<int64_t>(0, us) + 500) / 1000;
        return std::format(L"{} {:02}-{:02}-{:02}.{:03}{}", stem, ms / 3'600'000, ms / 60'000 % 60, ms / 1000 % 60, ms % 1000, extension);
    }

    std::vector<int64_t> NormalizeFrameTimes(std::vector<int64_t> times, double fps, int64_t durationUs)
    {
        const int64_t last = mediatools::LastFrameTime(fps, durationUs);
        for (auto& t : times)
            t = std::min(mediatools::SnapToFrame(std::max<int64_t>(0, t), fps), last);
        std::sort(times.begin(), times.end());
        // Two marks on the same millisecond would be the same file name: keep one.
        times.erase(std::unique(times.begin(), times.end(), [](int64_t a, int64_t b) { return (a + 500) / 1000 == (b + 500) / 1000; }), times.end());
        return times;
    }

    FramePick FramePickByName(std::string_view name)
    {
        if (name == "every") return FramePick::Every;
        if (name == "rate") return FramePick::PerSecond;
        return FramePick::Marks;
    }

    std::wstring FrameSequenceFilter(const FrameSequence& seq)
    {
        if (seq.pick == FramePick::PerSecond)
            return std::format(L"fps={}", Num(std::clamp(seq.perSecond, kMinPerSecond, kMaxPerSecond)));
        return std::format(L"select='not(mod(n,{}))'", std::clamp(seq.every, 1, kMaxEvery));
    }

    std::vector<std::wstring> FrameSequenceArgs(const std::wstring& input, const FrameSequence& seq, const std::wstring& folder)
    {
        // passthrough: a frame select drops is not repeated to fill the gap. The image2 muxer reads
        // the name as a pattern, so a '%' in the folder's path is doubled.
        return FfmpegArgs(input, {},
                          { L"-map", L"0:v:0", L"-an", L"-sn", L"-dn", L"-vf", FrameSequenceFilter(seq), L"-fps_mode", L"passthrough", L"-c:v", L"png",
                            L"-start_number", L"0", L"-f", L"image2" },
                          mediatools::EscapePercent(folder) + L"\\%06d.png");
    }

    int64_t FrameSequenceCount(const FrameSequence& seq, double fps, int64_t durationUs)
    {
        const double seconds = static_cast<double>(std::max<int64_t>(0, durationUs)) / 1e6;
        if (seq.pick == FramePick::PerSecond)
            // The fps filter rounds to the nearest output frame: 3.7 s at 3 a second is 11.
            return std::max<int64_t>(1, std::llround(seconds * std::clamp(seq.perSecond, kMinPerSecond, kMaxPerSecond)));
        if (fps <= 0)
            return 0;
        const int64_t frames = std::max<int64_t>(1, std::llround(seconds * fps));
        const int64_t every = std::clamp(seq.every, 1, kMaxEvery);
        return (frames + every - 1) / every;   // frames 0, N, 2N...
    }

    int64_t FrameSequenceTime(const FrameSequence& seq, double fps, int64_t index)
    {
        if (seq.pick == FramePick::PerSecond)
            return std::llround(static_cast<double>(index) * 1e6 / std::clamp(seq.perSecond, kMinPerSecond, kMaxPerSecond));
        if (fps <= 0)
            return 0;
        return std::llround(static_cast<double>(index) * std::clamp(seq.every, 1, kMaxEvery) * 1e6 / fps);
    }

    uint64_t FrameFileBytes(int width, int height, std::string_view format)
    {
        // Bytes per pixel of a typical camera or screen-recording frame: lossless PNG about 1.5, JPG
        // at quality 92 about 0.22, WebP at 85 about 0.08.
        const double perPixel = format == "jpg" ? 0.22 : format == "webp" ? 0.08 : 1.5;
        return static_cast<uint64_t>(static_cast<double>(std::max(width, 0)) * std::max(height, 0) * perPixel);
    }

    int FrameQuality(std::string_view format) { return format == "webp" ? 85 : 92; }

    // ---- 14. Split -------------------------------------------------------------------------------

    namespace
    {
        int PartDigits(int count) { return count >= 1000 ? 4 : count >= 100 ? 3 : count >= 10 ? 2 : 1; }
    }

    std::wstring PartName(const std::wstring& stem, int number, int count, const std::wstring& extension)
    {
        return std::format(L"{} part {:0{}}{}", stem, number, PartDigits(count), extension);
    }

    std::vector<std::wstring> SplitCopyArgs(const std::wstring& input, const std::wstring& ext, const std::vector<int64_t>& points,
                                            const std::wstring& folder, const std::wstring& stem)
    {
        const auto container = mediatools::CopyContainer(ext);
        std::wstring times;
        for (const int64_t p : points)
            times += (times.empty() ? L"" : L",") + Seconds(p);
        const int count = static_cast<int>(points.size()) + 1;
        // The segment muxer reads the name as a pattern: '%' in the folder and the stem must be doubled.
        const auto pattern = std::format(L"{}\\{} part %0{}d{}", mediatools::EscapePercent(folder), mediatools::EscapePercent(stem), PartDigits(count),
                                         container.extension);
        std::vector<std::wstring> out{ L"-map", L"0:v:0?", L"-map", L"0:a?", L"-c", L"copy", L"-sn", L"-dn", L"-f", L"segment",
                                       L"-segment_times", times, L"-segment_start_number", L"1", L"-reset_timestamps", L"1",
                                       L"-segment_format", container.muxer };
        if (container.muxer == L"mp4" || container.muxer == L"mov")
            out.insert(out.end(), { L"-segment_format_options", L"movflags=+faststart" });
        return FfmpegArgs(input, {}, out, pattern);
    }

    std::vector<std::wstring> SplitPartArgs(const std::wstring& input, const std::wstring& ext, const ProbeInfo& info, TimeRange part,
                                            H264Encoder encoder, const std::wstring& output)
    {
        return TrimArgs(input, ext, info, part, false, encoder, output);
    }

    // ---- 15. Redact ------------------------------------------------------------------------------

    std::wstring BetweenExpr(TimeRange range)
    {
        return std::format(L"between(t,{},{})", Seconds(range.start), Seconds(range.end));
    }

    std::wstring RedactGraph(const std::vector<RedactBox>& boxes, int frameW, int frameH)
    {
        std::wstring graph;
        std::wstring current = L"0:v:0";
        int n = 0;
        for (const auto& b : boxes)
        {
            const auto r = mediatools::ClampCrop(b.box, frameW, frameH);
            const auto when = std::format(L"enable='{}'", BetweenExpr(b.range));
            const auto next = std::format(L"r{}", n);
            if (b.blur)
            {
                // Blur a copy of just that area and lay it back on top while the range lasts. The
                // blur is strong for the box's size so faces and text are unreadable, not just soft.
                const double sigma = std::clamp(std::min(r.w, r.h) / 3.0, 12.0, 400.0);
                graph += std::format(L"[{0}]split=2[{1}a][{1}b];[{1}b]crop={2}:{3}:{4}:{5},gblur=sigma={6}:steps=3[{1}c];"
                                     L"[{1}a][{1}c]overlay=x={4}:y={5}:{7}[{1}];",
                                     current, next, r.w, r.h, r.x, r.y, Num(sigma), when);
            }
            else
                graph += std::format(L"[{}]drawbox=x={}:y={}:w={}:h={}:color=black@1:t=fill:{}[{}];", current, r.x, r.y, r.w, r.h, when, next);
            current = next;
            ++n;
        }
        graph += std::format(L"[{}]{}[vout]", current, kEven);
        return graph;
    }

    std::vector<std::wstring> RedactArgs(const std::wstring& input, const std::wstring& ext, const ProbeInfo& info, const std::vector<RedactBox>& boxes,
                                         H264Encoder encoder, const std::wstring& output)
    {
        const auto container = mediatools::EncodeContainer(ext);
        std::vector<std::wstring> out{ L"-filter_complex", RedactGraph(boxes, info.width, info.height), L"-map", L"[vout]" };
        if (info.HasAudio())
            out.insert(out.end(), { L"-map", L"0:a?" });
        out.insert(out.end(), { L"-sn", L"-dn" });
        const auto v = mediatools::EditVideoArgs(encoder, info.width, info.height, info.media.fps);
        out.insert(out.end(), v.begin(), v.end());
        const auto s = mediatools::SoundtrackArgs(container, info);
        out.insert(out.end(), s.begin(), s.end());
        AddMuxer(out, container);
        return FfmpegArgs(input, mediatools::DecodeArgs(encoder), out, output);
    }
}
