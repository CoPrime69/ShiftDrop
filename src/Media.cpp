#include "Media.h"

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
        // constant-quality mode in ffmpeg, so they get a generous bitrate. Shrinking files is the
        // Compress tool's job, not Convert's.
        std::vector<std::wstring> H264Args(H264Encoder encoder)
        {
            std::vector<std::wstring> a{ L"-c:v", EncoderName(encoder) };
            switch (encoder)
            {
            case H264Encoder::Nvenc:
                a.insert(a.end(), { L"-preset", L"p5", L"-rc", L"vbr", L"-cq", L"23", L"-b:v", L"0", L"-pix_fmt", L"yuv420p" });
                break;
            case H264Encoder::Qsv:
                a.insert(a.end(), { L"-preset", L"medium", L"-global_quality", L"23", L"-pix_fmt", L"nv12" });
                break;
            case H264Encoder::Amf:
                a.insert(a.end(), { L"-quality", L"balanced", L"-rc", L"cqp", L"-qp_i", L"21", L"-qp_p", L"23", L"-pix_fmt", L"nv12" });
                break;
            case H264Encoder::MediaFoundation:
                a.insert(a.end(), { L"-rate_control", L"u_vbr", L"-b:v", L"8M", L"-pix_fmt", L"nv12" });
                break;
            case H264Encoder::OpenH264:
                a.insert(a.end(), { L"-b:v", L"8M", L"-pix_fmt", L"yuv420p" });
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

    std::vector<std::wstring> OutputArgs(const MediaFormatInfo& format, H264Encoder h264, bool transcode)
    {
        const std::string_view id(format.id);
        // The first video and audio stream; "?" makes each optional (a silent clip, an audio file).
        const std::vector<std::wstring> firstAv{ L"-map", L"0:v:0?", L"-map", L"0:a:0?" };
        const std::vector<std::wstring> firstAudio{ L"-map", L"0:a:0", L"-vn" };
        std::vector<std::wstring> a;
        const auto add = [&a](std::initializer_list<const wchar_t*> items) { a.insert(a.end(), items.begin(), items.end()); };
        const auto addAll = [&a](const std::vector<std::wstring>& items) { a.insert(a.end(), items.begin(), items.end()); };

        if (id == "mp4" || id == "mov" || (id == "mkv" && transcode))
        {
            addAll(firstAv);
            add({ L"-vf", kEvenSize });
            addAll(H264Args(h264));
            add({ L"-c:a", L"aac", L"-b:a", L"192k" });
            if (id == "mkv")
                add({ L"-f", L"matroska" });
            else
                add({ L"-movflags", L"+faststart", L"-f", id == "mp4" ? L"mp4" : L"mov" });
        }
        else if (id == "mkv")
            add({ L"-map", L"0", L"-c", L"copy", L"-f", L"matroska" });   // every stream, untouched
        else if (id == "webm")
        {
            addAll(firstAv);
            add({ L"-c:v", L"libvpx-vp9", L"-crf", L"32", L"-b:v", L"0", L"-row-mt", L"1", L"-deadline", L"good", L"-cpu-used", L"4",
                  L"-c:a", L"libopus", L"-b:a", L"128k", L"-f", L"webm" });
        }
        else if (id == "avi")
        {
            addAll(firstAv);
            add({ L"-vf", kEvenSize, L"-c:v", L"mpeg4", L"-q:v", L"3", L"-tag:v", L"XVID", L"-c:a", L"libmp3lame", L"-q:a", L"4", L"-f", L"avi" });
        }
        else if (id == "wmv")
        {
            addAll(firstAv);
            add({ L"-vf", kEvenSize, L"-c:v", L"wmv2", L"-b:v", L"6M", L"-c:a", L"wmav2", L"-b:a", L"192k", L"-f", L"asf" });
        }
        else if (id == "gif")
        {
            // One pass with a palette made for this clip: far better colours than the default palette.
            // GIF has no pixel aspect ratio, so anamorphic video is scaled to square pixels first.
            add({ L"-map", L"0:v:0", L"-an", L"-vf",
                  L"fps=12,scale='min(480,iw*sar)':-2:flags=lanczos,setsar=1,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4",
                  L"-loop", L"0", L"-f", L"gif" });
        }
        else
        {
            addAll(firstAudio);
            if (id == "mp3")
                add({ L"-c:a", L"libmp3lame", L"-q:a", L"2", L"-f", L"mp3" });
            else if (id == "m4a")
                add({ L"-c:a", L"aac", L"-b:a", L"192k", L"-movflags", L"+faststart", L"-f", L"ipod" });
            else if (id == "wav")
                add({ L"-c:a", L"pcm_s16le", L"-f", L"wav" });
            else if (id == "flac")
                add({ L"-c:a", L"flac", L"-f", L"flac" });
            else if (id == "ogg")
                add({ L"-c:a", L"libvorbis", L"-q:a", L"5", L"-f", L"ogg" });
            else if (id == "opus")
                add({ L"-c:a", L"libopus", L"-b:a", L"128k", L"-f", L"opus" });
            else if (id == "aiff")
                add({ L"-c:a", L"pcm_s16be", L"-f", L"aiff" });
            else if (id == "wma")
                add({ L"-c:a", L"wmav2", L"-b:a", L"192k", L"-f", L"asf" });
        }
        return a;
    }

    std::vector<std::wstring> FfmpegArgs(const std::wstring& input, const std::vector<std::wstring>& outputArgs, const std::wstring& output)
    {
        // -protocol_whitelist file: the input can only be a local file, never a URL, so ffmpeg makes
        // no network connections whatever the path says.
        std::vector<std::wstring> a{ L"-hide_banner", L"-nostdin", L"-protocol_whitelist", L"file", L"-i", input };
        a.insert(a.end(), outputArgs.begin(), outputArgs.end());
        a.insert(a.end(), { L"-progress", L"pipe:1", L"-nostats", L"-y", output });
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
