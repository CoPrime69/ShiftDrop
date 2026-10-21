#include "Media.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <charconv>
#include <cstring>

namespace sd
{
    namespace
    {
        // Whole ".ext" tokens of a run like ".mp4.m4v".
        bool HasToken(std::wstring_view all, const std::wstring& ext)
        {
            if (ext.empty())
                return false;
            size_t pos = 0;
            while ((pos = all.find(ext, pos)) != std::wstring_view::npos)
            {
                const size_t end = pos + ext.size();
                if (end == all.size() || all[end] == L'.')
                    return true;
                pos = end;
            }
            return false;
        }

        constexpr std::wstring_view kVideoExtensions = L".mp4.m4v.mov.qt.mkv.webm.avi.wmv.asf.flv.mpg.mpeg.mpe.vob.ts.mts.m2ts.3gp.3g2.ogv.mxf.divx";
        constexpr std::wstring_view kAudioExtensions = L".mp3.m4a.m4b.aac.wav.wave.flac.ogg.oga.opus.aif.aiff.aifc.wma.alac.ac3.amr.ape.wv.mka.caf";

        // H.264 for MP4/MOV, with each encoder's own idea of "good quality". The GPU encoders run in
        // constant-quality mode (size follows content); Media Foundation and OpenH264 have no reliable
        // constant-quality mode in ffmpeg, so they get a bitrate instead (`fallbackBps`). `cq` is the
        // quality level on the usual H.264 scale: 23 = visually lossless for most video, 28 = clearly
        // smaller with little visible loss. Presets lean to speed (p4, fast): GPU encoders are already
        // good at these settings and conversions should feel instant.
        std::vector<std::wstring> H264Args(H264Encoder encoder, int cq, int64_t fallbackBps)
        {
            const auto q = std::to_wstring(cq);
            const auto bps = std::to_wstring(fallbackBps);
            std::vector<std::wstring> a{ L"-c:v", EncoderName(encoder) };
            switch (encoder)
            {
            case H264Encoder::Nvenc:
                a.insert(a.end(), { L"-preset", L"p4", L"-rc", L"vbr", L"-cq", q, L"-b:v", L"0", L"-pix_fmt", L"yuv420p" });
                break;
            case H264Encoder::Qsv:
                a.insert(a.end(), { L"-preset", L"fast", L"-global_quality", q, L"-pix_fmt", L"nv12" });
                break;
            case H264Encoder::Amf:
                a.insert(a.end(), { L"-quality", L"speed", L"-rc", L"cqp", L"-qp_i", std::to_wstring(cq - 2), L"-qp_p", q, L"-pix_fmt", L"nv12" });
                break;
            case H264Encoder::MediaFoundation:
                a.insert(a.end(), { L"-rate_control", L"u_vbr", L"-b:v", bps, L"-pix_fmt", L"nv12" });
                break;
            case H264Encoder::OpenH264:
                a.insert(a.end(), { L"-rc_mode", L"bitrate", L"-b:v", bps, L"-pix_fmt", L"yuv420p" });
                break;
            }
            return a;
        }

        std::vector<std::wstring> H264Args(H264Encoder encoder) { return H264Args(encoder, 23, 8'000'000); }

        // H.264 aiming at an average bitrate, for a size target. maxrate caps the peaks so the file
        // can't overshoot much; the runner checks the size and retries lower if it does.
        std::vector<std::wstring> H264BitrateArgs(H264Encoder encoder, int64_t bps)
        {
            const auto b = std::to_wstring(bps);
            const auto buf = std::to_wstring(bps * 2);
            std::vector<std::wstring> a{ L"-c:v", EncoderName(encoder) };
            switch (encoder)
            {
            case H264Encoder::Nvenc:
                a.insert(a.end(), { L"-preset", L"p4", L"-rc", L"vbr", L"-multipass", L"qres", L"-b:v", b, L"-maxrate", b, L"-bufsize", buf, L"-pix_fmt", L"yuv420p" });
                break;
            case H264Encoder::Qsv:
                a.insert(a.end(), { L"-preset", L"fast", L"-b:v", b, L"-maxrate", b, L"-bufsize", buf, L"-pix_fmt", L"nv12" });
                break;
            case H264Encoder::Amf:
                a.insert(a.end(), { L"-quality", L"balanced", L"-rc", L"vbr_peak", L"-b:v", b, L"-maxrate", b, L"-bufsize", buf, L"-pix_fmt", L"nv12" });
                break;
            case H264Encoder::MediaFoundation:
                a.insert(a.end(), { L"-rate_control", L"cbr", L"-b:v", b, L"-pix_fmt", L"nv12" });
                break;
            case H264Encoder::OpenH264:
                a.insert(a.end(), { L"-rc_mode", L"bitrate", L"-b:v", b, L"-maxrate", b, L"-pix_fmt", L"yuv420p" });
                break;
            }
            return a;
        }

        // 4:2:0 video needs even dimensions; odd phone-camera crops would otherwise fail to encode.
        // Cropping the odd pixel (rather than scaling) leaves the pixel aspect ratio untouched.
        constexpr const wchar_t* kEvenSize = L"crop=trunc(iw/2)*2:trunc(ih/2)*2";
    }

    MediaKind MediaKindOfExtension(const std::wstring& ext)
    {
        if (HasToken(kVideoExtensions, ext))
            return MediaKind::Video;
        if (HasToken(kAudioExtensions, ext))
            return MediaKind::Audio;
        return MediaKind::None;
    }

    const wchar_t* EncoderName(H264Encoder encoder)
    {
        switch (encoder)
        {
        case H264Encoder::Nvenc: return L"h264_nvenc";
        case H264Encoder::Qsv: return L"h264_qsv";
        case H264Encoder::Amf: return L"h264_amf";
        case H264Encoder::MediaFoundation: return L"h264_mf";
        case H264Encoder::OpenH264: return L"libopenh264";
        }
        return L"libopenh264";
    }

    std::vector<std::wstring> H264QualityArgs(H264Encoder encoder, int cq, int64_t fallbackBps)
    {
        return H264Args(encoder, cq, fallbackBps);
    }

    const std::vector<MediaFormatInfo>& MediaFormats()
    {
        // Wheel order: video formats, then audio. MP3 is offered for both (audio from a video).
        static const std::vector<MediaFormatInfo> formats = {
            { "mp4", L"MP4", L".mp4", L".mp4.m4v", L"H.264 video, AAC audio", true, false, false },
            { "mov", L"MOV", L".mov", L".mov.qt", L"QuickTime, H.264 and AAC", true, false, false },
            { "mkv", L"MKV", L".mkv", L".mkv", L"Matroska, streams kept as they are", true, false, true },
            { "webm", L"WebM", L".webm", L".webm", L"VP9 video, Opus audio", true, false, false },
            { "avi", L"AVI", L".avi", L".avi.divx", L"MPEG-4 video, MP3 audio", true, false, false },
            { "wmv", L"WMV", L".wmv", L".wmv.asf", L"Windows Media Video", true, false, false },
            { "gif", L"GIF", L".gif", L".gif", L"Animated GIF, 12 fps, up to 480 px", true, false, false },
            { "mp3", L"MP3", L".mp3", L".mp3", L"MP3, VBR about 190 kbps", true, true, false },
            { "m4a", L"M4A", L".m4a", L".m4a.m4b", L"AAC, 192 kbps", false, true, false },
            { "wav", L"WAV", L".wav", L".wav.wave", L"WAV, 16-bit PCM", false, true, false },
            { "flac", L"FLAC", L".flac", L".flac", L"FLAC, lossless", false, true, false },
            { "ogg", L"OGG", L".ogg", L".ogg.oga", L"Ogg Vorbis, quality 5", false, true, false },
            { "opus", L"Opus", L".opus", L".opus", L"Opus, 128 kbps", false, true, false },
            { "aiff", L"AIFF", L".aiff", L".aif.aiff.aifc", L"AIFF, 16-bit PCM", false, true, false },
            { "wma", L"WMA", L".wma", L".wma", L"Windows Media Audio, 192 kbps", false, true, false },
        };
        return formats;
    }

    const MediaFormatInfo* FindMediaFormat(std::string_view id)
    {
        for (const auto& f : MediaFormats())
            if (id == f.id)
                return &f;
        return nullptr;
    }

    bool IsAlreadyMediaFormat(const MediaFormatInfo& format, const std::wstring& ext)
    {
        return HasToken(format.aliases, ext);
    }

    bool MediaFormatAccepts(const MediaFormatInfo& format, MediaKind kind)
    {
        return (kind == MediaKind::Video && format.fromVideo) || (kind == MediaKind::Audio && format.fromAudio);
    }

    namespace
    {
        bool OneOf(std::string_view codec, std::initializer_list<std::string_view> codecs)
        {
            return !codec.empty() && std::find(codecs.begin(), codecs.end(), codec) != codecs.end();
        }

        // Video codecs each container takes as they are, picked so the result plays where people
        // expect that format to play (an MP4 from MPEG-4 Part 2 would, but not on most phones).
        bool ContainerTakesVideo(std::string_view id, std::string_view codec)
        {
            if (id == "mp4") return OneOf(codec, { "h264", "hevc" });
            if (id == "mov") return OneOf(codec, { "h264", "hevc", "prores" });
            if (id == "webm") return OneOf(codec, { "vp8", "vp9", "av1" });
            if (id == "avi") return OneOf(codec, { "mpeg4" });
            if (id == "wmv") return OneOf(codec, { "wmv1", "wmv2", "wmv3", "vc1" });
            return false;
        }

        bool ContainerTakesAudio(std::string_view id, std::string_view codec)
        {
            if (id == "mp4") return OneOf(codec, { "aac", "mp3" });
            if (id == "mov") return OneOf(codec, { "aac", "mp3", "alac", "pcm_s16le", "pcm_s16be", "pcm_s24le" });
            if (id == "webm") return OneOf(codec, { "opus", "vorbis" });
            if (id == "avi") return OneOf(codec, { "mp3", "ac3" });
            if (id == "wmv" || id == "wma") return OneOf(codec, { "wmav2" });
            if (id == "mp3") return OneOf(codec, { "mp3" });
            if (id == "m4a") return OneOf(codec, { "aac", "alac" });
            if (id == "wav") return OneOf(codec, { "pcm_s16le" });
            if (id == "flac") return OneOf(codec, { "flac" });
            if (id == "ogg") return OneOf(codec, { "vorbis" });
            if (id == "opus") return OneOf(codec, { "opus" });
            if (id == "aiff") return OneOf(codec, { "pcm_s16be" });
            return false;
        }

        // A number at `at` in `line`, e.g. the 1920 of "1920x1080".
        template <typename T>
        bool NumberAt(std::string_view line, size_t at, T& value, size_t* end = nullptr)
        {
            const char* first = line.data() + at;
            const auto r = std::from_chars(first, line.data() + line.size(), value);
            if (r.ec != std::errc{} || r.ptr == first)
                return false;
            if (end)
                *end = static_cast<size_t>(r.ptr - line.data());
            return true;
        }

        // The codec name after "Video: " or "Audio: ": "h264" from "Video: h264 (High) (avc1 / ...), ...".
        std::string CodecAfter(std::string_view line, std::string_view key)
        {
            const auto at = line.find(key);
            if (at == std::string_view::npos)
                return {};
            auto rest = line.substr(at + key.size());
            const auto stop = rest.find_first_of(" ,");
            return std::string(rest.substr(0, stop));
        }
    }

    MediaInfo ParseMediaInfo(const std::vector<std::string>& log)
    {
        MediaInfo info;
        for (const std::string_view line : log)
        {
            if (info.durationUs < 0)
                info.durationUs = ParseDurationLine(line);
            const auto has = [&line](std::string_view what) { return line.find(what) != std::string_view::npos; };
            if (!has("Stream #0:"))
                continue;
            if (has("Video: ") && info.videoCodec.empty() && !has("(attached pic)"))
            {
                // Cover art in a song is a one-frame "video" marked (attached pic): not a video stream.
                info.videoCodec = CodecAfter(line, "Video: ");
                // The first "WxH" with both sides numbers (the codec tag can contain an 'x').
                for (size_t x = line.find('x'); x != std::string_view::npos; x = line.find('x', x + 1))
                {
                    size_t start = x;
                    while (start > 0 && line[start - 1] >= '0' && line[start - 1] <= '9')
                        --start;
                    int w = 0, h = 0;
                    if (start < x && x - start >= 2 && NumberAt(line, start, w) && NumberAt(line, x + 1, h) && w > 0 && h > 0)
                    {
                        info.width = w;
                        info.height = h;
                        break;
                    }
                }
                if (const auto f = line.find(" fps"); f != std::string_view::npos)
                {
                    size_t start = f;
                    while (start > 0 && (std::isdigit(static_cast<unsigned char>(line[start - 1])) || line[start - 1] == '.'))
                        --start;
                    double fps = 0;
                    if (start < f && NumberAt(line, start, fps))
                        info.fps = fps;
                }
            }
            else if (has("Audio: ") && info.audioCodec.empty())
            {
                info.audioCodec = CodecAfter(line, "Audio: ");
            }
        }
        return info;
    }

    MediaPlan PlanConversion(const MediaFormatInfo& format, const MediaInfo& info, H264Encoder h264)
    {
        MediaPlan plan;
        plan.h264 = h264;
        const std::string_view id(format.id);
        if (format.remux)
        {
            plan.copyAll = true;   // MKV holds almost anything: copy every stream as it is
            return plan;
        }
        if (id == "gif")
            return plan;
        const bool audioOnly = !format.fromVideo;
        plan.copyVideo = !audioOnly && ContainerTakesVideo(id, info.videoCodec);
        plan.hevc = info.videoCodec == "hevc";
        plan.copyAudio = ContainerTakesAudio(id, info.audioCodec);
        // A GPU encoder usually comes with that GPU's decoder; decoding there too saves the CPU a lot
        // on 4K phone video. ffmpeg falls back to software decoding by itself if it can't.
        plan.hwDecode = !audioOnly && !plan.copyVideo && !info.videoCodec.empty() && (id == "mp4" || id == "mov") &&
                        h264 != H264Encoder::OpenH264 && h264 != H264Encoder::MediaFoundation;
        return plan;
    }

    MediaPlan TranscodePlan(const MediaPlan& plan)
    {
        MediaPlan full;
        full.h264 = plan.h264;
        full.hwDecode = plan.hwDecode;
        full.videoBps = plan.videoBps;
        return full;
    }

    int64_t CapVideoBps(int64_t sourceBytes, int64_t durationUs, double share, bool hasAudio)
    {
        if (sourceBytes <= 0 || durationUs <= 0)
            return 0;
        constexpr int64_t kAudioBps = 192'000;   // the most any Convert spends on sound
        constexpr int64_t kFloorBps = 32'000;    // an encoder needs something to work with
        const double budget = static_cast<double>(sourceBytes) * 8.0 * 1'000'000.0 / static_cast<double>(durationUs) * share;
        const double audio = hasAudio ? std::min<double>(kAudioBps, budget / 4) : 0;
        return std::max<int64_t>(kFloorBps, static_cast<int64_t>(budget - audio));
    }

    std::vector<std::wstring> InputArgs(const MediaPlan& plan)
    {
        if (plan.hwDecode)
            return { L"-hwaccel", L"auto" };
        return {};
    }

    std::vector<std::wstring> OutputArgs(const MediaFormatInfo& format, const MediaPlan& plan)
    {
        const std::string_view id(format.id);
        std::vector<std::wstring> a;
        const auto add = [&a](std::initializer_list<const wchar_t*> items) { a.insert(a.end(), items.begin(), items.end()); };
        const auto addAll = [&a](const std::vector<std::wstring>& items) { a.insert(a.end(), items.begin(), items.end()); };

        if (plan.copyAll)
        {
            add({ L"-map", L"0", L"-c", L"copy", L"-f", L"matroska" });   // every stream, untouched
            return a;
        }
        if (id == "gif")
        {
            // One pass with a palette made for this clip: far better colours than the default palette.
            // GIF has no pixel aspect ratio, so anamorphic video is scaled to square pixels first.
            add({ L"-map", L"0:v:0", L"-an", L"-vf",
                  L"fps=12,scale='min(480,iw*sar)':-2:flags=lanczos,setsar=1,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4",
                  L"-loop", L"0", L"-f", L"gif" });
            return a;
        }

        if (format.fromVideo && id != "mp3")
        {
            // The first video and audio stream; "?" makes each optional (a silent clip).
            add({ L"-map", L"0:v:0?", L"-map", L"0:a:0?" });
            if (plan.copyVideo)
            {
                add({ L"-c:v", L"copy" });
                // HEVC in MP4/MOV needs the hvc1 tag, or Apple devices and some players refuse it.
                if (plan.hevc && (id == "mp4" || id == "mov"))
                    add({ L"-tag:v", L"hvc1" });
            }
            else if (id == "mp4" || id == "mov" || id == "mkv")
            {
                add({ L"-vf", kEvenSize });
                addAll(plan.videoBps > 0 ? H264BitrateArgs(plan.h264, plan.videoBps) : H264Args(plan.h264));
            }
            else if (plan.videoBps > 0)
            {
                // Held to a bitrate (see CapVideoBps). VP9 keeps its quality setting under the cap.
                const auto b = std::to_wstring(id == "wmv" ? std::min<int64_t>(plan.videoBps, 6'000'000) : plan.videoBps);
                if (id == "webm")
                    add({ L"-c:v", L"libvpx-vp9", L"-crf", L"32", L"-b:v", b.c_str(), L"-row-mt", L"1", L"-deadline", L"good", L"-cpu-used", L"5" });
                else if (id == "avi")
                    add({ L"-vf", kEvenSize, L"-c:v", L"mpeg4", L"-b:v", b.c_str(), L"-tag:v", L"XVID" });
                else if (id == "wmv")
                    add({ L"-vf", kEvenSize, L"-c:v", L"wmv2", L"-b:v", b.c_str() });
            }
            else if (id == "webm")
                add({ L"-c:v", L"libvpx-vp9", L"-crf", L"32", L"-b:v", L"0", L"-row-mt", L"1", L"-deadline", L"good", L"-cpu-used", L"5" });
            else if (id == "avi")
                add({ L"-vf", kEvenSize, L"-c:v", L"mpeg4", L"-q:v", L"3", L"-tag:v", L"XVID" });
            else if (id == "wmv")
                add({ L"-vf", kEvenSize, L"-c:v", L"wmv2", L"-b:v", L"6M" });
        }
        else
        {
            // Audio formats take the first audio stream and drop the video (cover art included).
            add({ L"-map", L"0:a:0", L"-vn" });
        }

        if (plan.copyAudio)
            add({ L"-c:a", L"copy" });
        else if (id == "mp4" || id == "mov" || id == "mkv")
            add({ L"-c:a", L"aac", L"-b:a", L"192k" });
        else if (id == "webm" || id == "opus")
            add({ L"-c:a", L"libopus", L"-b:a", L"128k" });
        else if (id == "avi")
            add({ L"-c:a", L"libmp3lame", L"-q:a", L"4" });
        else if (id == "wmv" || id == "wma")
            add({ L"-c:a", L"wmav2", L"-b:a", L"192k" });
        else if (id == "mp3")
            add({ L"-c:a", L"libmp3lame", L"-q:a", L"2" });
        else if (id == "m4a")
            add({ L"-c:a", L"aac", L"-b:a", L"192k" });
        else if (id == "wav")
            add({ L"-c:a", L"pcm_s16le" });
        else if (id == "flac")
            add({ L"-c:a", L"flac" });
        else if (id == "ogg")
            add({ L"-c:a", L"libvorbis", L"-q:a", L"5" });
        else if (id == "aiff")
            add({ L"-c:a", L"pcm_s16be" });

        // The muxer, named because the staging file's extension (".sdpart") says nothing.
        if (id == "mp4" || id == "mov")
            add({ L"-movflags", L"+faststart", L"-f", id == "mp4" ? L"mp4" : L"mov" });
        else if (id == "m4a")
            add({ L"-movflags", L"+faststart", L"-f", L"ipod" });
        else if (id == "mkv")
            add({ L"-f", L"matroska" });
        else if (id == "wmv" || id == "wma")
            add({ L"-f", L"asf" });
        else
            a.insert(a.end(), { L"-f", std::wstring(id.begin(), id.end()) });   // webm, avi, mp3, wav, flac, ogg, opus, aiff
        return a;
    }

    std::vector<std::wstring> FfmpegArgs(const std::wstring& input, const std::vector<std::wstring>& inputArgs,
                                         const std::vector<std::wstring>& outputArgs, const std::wstring& output)
    {
        // -protocol_whitelist file: the input can only be a local file, never a URL, so ffmpeg makes
        // no network connections whatever the path says.
        std::vector<std::wstring> a{ L"-hide_banner", L"-nostdin", L"-protocol_whitelist", L"file" };
        a.insert(a.end(), inputArgs.begin(), inputArgs.end());
        a.insert(a.end(), { L"-i", input });
        a.insert(a.end(), outputArgs.begin(), outputArgs.end());
        a.insert(a.end(), { L"-progress", L"pipe:1", L"-nostats", L"-y", output });
        return a;
    }

    std::vector<std::wstring> ProbeArgs(const std::wstring& input)
    {
        // No output: ffmpeg prints the input's streams and stops ("At least one output file must be
        // specified", exit code 1, which is expected here).
        return { L"-hide_banner", L"-nostdin", L"-protocol_whitelist", L"file", L"-i", input };
    }

    bool PlanSizeFit(int64_t targetBytes, const MediaInfo& info, SizeFit& fit)
    {
        fit = {};
        if (targetBytes <= 0 || info.durationUs <= 0)
            return false;
        const double seconds = static_cast<double>(info.durationUs) / 1e6;
        // 4% for the container (MP4 boxes, interleaving) and the encoder's own drift.
        const double total = static_cast<double>(targetBytes) * 8.0 * 0.96 / seconds;
        if (info.videoCodec.empty())
        {
            if (total < 24'000)
                return false;   // below this AAC is barely speech
            fit.audioBps = static_cast<int64_t>(std::min(total, 256'000.0));
            return true;
        }
        fit.audioBps = info.audioCodec.empty() ? 0 : total >= 1'500'000 ? 128'000 : total >= 500'000 ? 96'000 : 64'000;
        const double video = total - static_cast<double>(fit.audioBps);
        if (video < 100'000)
            return false;   // a picture this starved is not worth making
        fit.videoBps = static_cast<int64_t>(video);
        // The tallest standard height whose pixel count the bitrate can carry at ~0.08 bits per pixel
        // per frame (a usual H.264 working point), never taller than the source.
        const double fps = info.fps > 0 ? std::min(info.fps, 60.0) : 30.0;
        const double pixels = video / (fps * 0.08);
        const double aspect = info.width > 0 && info.height > 0 ? static_cast<double>(info.width) / info.height : 16.0 / 9.0;
        const double maxHeight = std::sqrt(pixels / aspect);
        for (const int h : { 2160, 1440, 1080, 720, 540, 480, 360, 240 })
        {
            if (h <= maxHeight || h == 240)
            {
                fit.height = info.height > 0 && h >= info.height ? 0 : h;   // 0 = keep the source's size
                break;
            }
        }
        return true;
    }

    std::vector<std::wstring> CompressArgs(const MediaInfo& info, H264Encoder h264, const SizeFit* fit, int maxHeight)
    {
        std::vector<std::wstring> a;
        const auto add = [&a](std::initializer_list<const wchar_t*> items) { a.insert(a.end(), items.begin(), items.end()); };
        if (info.videoCodec.empty())
        {
            // Audio: AAC in M4A.
            const int64_t bps = fit ? fit->audioBps : 96'000;
            a.insert(a.end(), { L"-map", L"0:a:0", L"-vn", L"-c:a", L"aac", L"-b:a", std::to_wstring(bps), L"-movflags", L"+faststart", L"-f", L"ipod" });
            return a;
        }
        add({ L"-map", L"0:v:0", L"-map", L"0:a:0?" });
        int height = fit ? fit->height : 0;
        if (maxHeight > 0 && info.height > maxHeight && (height == 0 || height > maxHeight))
            height = maxHeight;
        if (height > 0)
            a.insert(a.end(), { L"-vf", std::format(L"scale=-2:{}:flags=bicubic", height) });
        else
            a.insert(a.end(), { L"-vf", kEvenSize });
        if (fit)
        {
            const auto v = H264BitrateArgs(h264, fit->videoBps);
            a.insert(a.end(), v.begin(), v.end());
        }
        else
        {
            // Preset: quality 28, the bitrate fallback sized from the output's pixels.
            const int h = height > 0 ? height : info.height > 0 ? info.height : 1080;
            const int w = info.height > 0 ? info.width * h / info.height : h * 16 / 9;
            const double fps = info.fps > 0 ? std::min(info.fps, 60.0) : 30.0;
            const auto v = H264Args(h264, 28, static_cast<int64_t>(static_cast<double>(w) * h * fps * 0.06));
            a.insert(a.end(), v.begin(), v.end());
        }
        const int64_t audio = fit ? fit->audioBps : 128'000;
        if (audio > 0)
            a.insert(a.end(), { L"-c:a", L"aac", L"-b:a", std::to_wstring(audio) });
        add({ L"-movflags", L"+faststart", L"-f", L"mp4" });
        return a;
    }

    std::wstring QuoteArg(const std::wstring& arg)
    {
        if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
            return arg;
        // Backslashes are literal except before a quote, where each must be doubled (and the quote
        // escaped); a run of backslashes before the closing quote is doubled too.
        std::wstring out = L"\"";
        size_t slashes = 0;
        for (const wchar_t c : arg)
        {
            if (c == L'\\')
            {
                ++slashes;
                continue;
            }
            if (c == L'"')
                out.append(slashes * 2 + 1, L'\\');
            else
                out.append(slashes, L'\\');
            slashes = 0;
            out += c;
        }
        out.append(slashes * 2, L'\\');
        out += L'"';
        return out;
    }

    int64_t ParseDurationLine(std::string_view line)
    {
        const auto at = line.find("Duration: ");
        if (at == std::string_view::npos)
            return -1;
        line.remove_prefix(at + 10);
        int h = 0, m = 0;
        double s = 0;
        // hh:mm:ss.cc
        const char* p = line.data();
        const char* end = line.data() + line.size();
        auto r = std::from_chars(p, end, h);
        if (r.ec != std::errc{} || r.ptr == end || *r.ptr != ':')
            return -1;
        r = std::from_chars(r.ptr + 1, end, m);
        if (r.ec != std::errc{} || r.ptr == end || *r.ptr != ':')
            return -1;
        r = std::from_chars(r.ptr + 1, end, s);
        if (r.ec != std::errc{})
            return -1;
        return static_cast<int64_t>((h * 3600.0 + m * 60.0 + s) * 1e6);
    }

    int64_t ParseProgressLine(std::string_view line)
    {
        constexpr std::string_view key = "out_time_us=";
        if (!line.starts_with(key))
            return -1;
        line.remove_prefix(key.size());
        int64_t us = -1;
        const auto r = std::from_chars(line.data(), line.data() + line.size(), us);
        return r.ec == std::errc{} && us >= 0 ? us : -1;
    }
}
