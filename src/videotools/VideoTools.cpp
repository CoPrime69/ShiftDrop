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

    std::vector<std::wstring> TrimArgs(const std::wstring& input, const std::wstring& ext, const ProbeInfo& info, TimeRange range, bool fast,
                                       H264Encoder encoder, const std::wstring& output)
    {
        const auto container = TrimContainer(ext, fast);
        std::vector<std::wstring> in;
        if (!fast)
            in = mediatools::DecodeArgs(encoder);
        // -ss/-to on the input: ffmpeg seeks instead of decoding up to the start. With copied streams
        // it lands on the keyframe at or before the start; when decoding it is frame-exact.
        in.insert(in.end(), { L"-ss", Seconds(range.start), L"-to", Seconds(range.end) });
        if (fast)
        {
            std::vector<std::wstring> out{ L"-map", L"0:v:0?", L"-map", L"0:a?", L"-c", L"copy", L"-sn", L"-dn", L"-avoid_negative_ts", L"make_zero" };
            if (info.media.videoCodec == "hevc" && (container.muxer == L"mp4" || container.muxer == L"mov"))
                out.insert(out.end(), { L"-tag:v", L"hvc1" });
            AddMuxer(out, container);
            return FfmpegArgs(input, in, out, output);
        }
        return FfmpegArgs(input, in, EncodeOutput(container, info, encoder, {}, info.width, info.height, info.media.fps), output);
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

    std::vector<std::wstring> SpeedArgs(const std::wstring& input, const std::wstring& ext, const ProbeInfo& info, double factor,
                                        H264Encoder encoder, const std::wstring& output)
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
                                       info.HasAudio() ? AtempoChain(factor) : std::wstring()),
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

    std::wstring FramePngName(const std::wstring& stem, int64_t us)
    {
        const int64_t ms = (std::max<int64_t>(0, us) + 500) / 1000;
        return std::format(L"{} {:02}-{:02}-{:02}.{:03}.png", stem, ms / 3'600'000, ms / 60'000 % 60, ms / 1000 % 60, ms % 1000);
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

    // ---- 14. Split -------------------------------------------------------------------------------

    std::wstring PartName(const std::wstring& stem, int number, int count, const std::wstring& extension)
    {
        const int digits = count >= 100 ? 3 : count >= 10 ? 2 : 1;
        return std::format(L"{} part {:0{}}{}", stem, number, digits, extension);
    }

    std::vector<std::wstring> SplitCopyArgs(const std::wstring& input, const std::wstring& ext, const std::vector<int64_t>& points,
                                            const std::wstring& folder, const std::wstring& stem)
    {
        const auto container = mediatools::CopyContainer(ext);
        std::wstring times;
        for (const int64_t p : points)
            times += (times.empty() ? L"" : L",") + Seconds(p);
        const int count = static_cast<int>(points.size()) + 1;
        const int digits = count >= 100 ? 3 : count >= 10 ? 2 : 1;
        // The segment muxer reads the name as a pattern: '%' in the stem must be doubled.
        const auto pattern = std::format(L"{}\\{} part %0{}d{}", folder, mediatools::EscapePercent(stem), digits, container.extension);
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
