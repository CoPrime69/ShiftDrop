// The bundled ffmpeg.exe is built with only what ShiftDrop uses (scripts/ffmpeg-components.txt,
// scripts/build-ffmpeg.sh). These tests run every ffmpeg argument builder in the app, collect each
// encoder, muxer, input format, filter and hardware decoder they name, and check that the components
// file lists it: a new codec or filter in the code fails here until the build includes it.
// (scripts/check-ffmpeg.ps1 then checks that the exe itself has everything the file lists.)
#include "Media.h"
#include "Utf.h"
#include "audiotools/AudioTools.h"
#include "mediaeditor/MediaTools.h"
#include "metadata/MediaMetadata.h"
#include "videotools/VideoTools.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

using namespace sd;
using namespace sd::mediatools;

namespace
{
    std::filesystem::path ComponentsFile()
    {
        wchar_t env[MAX_PATH]{};
        if (const DWORD n = GetEnvironmentVariableW(L"SHIFTDROP_FFMPEG_COMPONENTS", env, MAX_PATH); n > 0 && n < MAX_PATH)
            return env;
#ifdef SHIFTDROP_FFMPEG_COMPONENTS
        return std::filesystem::path(Widen(SHIFTDROP_FFMPEG_COMPONENTS));
#else
        return {};
#endif
    }

    // Section -> names (as ffmpeg prints them); a name with * is a pattern.
    using Components = std::map<std::string, std::vector<std::string>>;

    Components Load()
    {
        Components c;
        std::ifstream in(ComponentsFile());
        std::string line, section;
        while (std::getline(in, line))
        {
            line = line.substr(0, line.find('#'));
            if (line.starts_with('['))
            {
                section = line.substr(1, line.find(']') - 1);
                c[section];
                continue;
            }
            std::istringstream words(line);
            for (std::string w; words >> w;)
                c[section].push_back(w);
        }
        return c;
    }

    bool Listed(const Components& c, const std::string& section, const std::string& name)
    {
        const auto it = c.find(section);
        if (it == c.end())
            return false;
        for (const auto& n : it->second)
        {
            if (n == name)
                return true;
            if (n.find('*') != std::string::npos)
            {
                std::string re;
                for (const char ch : n)
                    re += ch == '*' ? std::string(".*") : std::string(1, ch);
                if (std::regex_match(name, std::regex(re)))
                    return true;
            }
        }
        return false;
    }

    // What a set of command lines asks of ffmpeg.
    struct Uses
    {
        std::set<std::string> encoders, muxers, demuxers, filters, hwaccels;
    };

    // The filter names in a filtergraph: "[0:v]scale=1:2,pad=...[v];[v][1]overlay" -> scale, pad,
    // overlay. Quotes and backslashes protect commas inside expressions, as in ffmpeg.
    void AddFilters(const std::wstring& graph, std::set<std::string>& out)
    {
        std::wstring current;
        bool quoted = false;
        const auto flush = [&] {
            std::wstring f = current;
            current.clear();
            while (!f.empty() && f.front() == L'[')   // input labels
                f.erase(0, f.find(L']') + 1);
            const auto end = f.find_first_of(L"=[@");
            f = f.substr(0, end);
            if (!f.empty())
                out.insert(Narrow(f));
        };
        for (size_t i = 0; i < graph.size(); ++i)
        {
            const wchar_t ch = graph[i];
            if (ch == L'\\' && i + 1 < graph.size())
            {
                current += graph[++i];
                continue;
            }
            if (ch == L'\'')
                quoted = !quoted;
            if (!quoted && (ch == L',' || ch == L';'))
            {
                flush();
                continue;
            }
            current += ch;
        }
        flush();
    }

    void Collect(const std::vector<std::wstring>& a, Uses& u)
    {
        size_t lastInput = 0;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i] == L"-i")
                lastInput = i;
        for (size_t i = 0; i + 1 < a.size(); ++i)
        {
            const auto& opt = a[i];
            const auto value = Narrow(a[i + 1]);
            if (opt == L"-c" || opt == L"-c:v" || opt == L"-c:a" || opt == L"-codec:v" || opt == L"-codec:a" || opt == L"-vcodec" || opt == L"-acodec")
            {
                if (value != "copy")
                    u.encoders.insert(value);
            }
            else if (opt == L"-f")
                (i < lastInput ? u.demuxers : u.muxers).insert(value);
            else if (opt == L"-segment_format")
                u.muxers.insert(value);
            else if (opt == L"-vf" || opt == L"-af" || opt == L"-filter_complex" || opt == L"-filter:v" || opt == L"-filter:a")
                AddFilters(a[i + 1], u.filters);
            else if (opt == L"-hwaccel")
                u.hwaccels.insert(value);
        }
    }

    MediaInfo Video(std::string v, std::string a)
    {
        MediaInfo m;
        m.durationUs = 10'000'000;
        m.videoCodec = std::move(v);
        m.audioCodec = std::move(a);
        m.width = 1920;
        m.height = 1080;
        m.fps = 29.97;
        return m;
    }

    ProbeInfo Probe(std::string v, std::string a, int channels = 2)
    {
        ProbeInfo p;
        p.media = Video(std::move(v), std::move(a));
        if (p.media.videoCodec.empty())
            p.media.width = p.media.height = 0;
        p.width = p.media.width;
        p.height = p.media.height;
        p.channels = p.media.audioCodec.empty() ? 0 : channels;
        p.sampleRate = 44100;
        return p;
    }

    constexpr H264Encoder kEncoders[] = { H264Encoder::Nvenc, H264Encoder::Qsv, H264Encoder::Amf, H264Encoder::MediaFoundation,
                                          H264Encoder::OpenH264 };

    // Every argument builder in the app, with inputs that take each of its branches.
    Uses Everything()
    {
        Uses u;
        const auto add = [&u](const std::vector<std::wstring>& a) { Collect(a, u); };
        const std::wstring in = L"C:\\in.sdin", out = L"C:\\out.sdpart";

        // H.264 probe at startup (Ffmpeg.cpp): generated frames into the null muxer, per encoder.
        for (const auto e : kEncoders)
            add({ L"-f", L"lavfi", L"-i", L"color=c=black:s=320x240:r=30:d=0.3", L"-c:v", EncoderName(e), L"-f", L"null", L"-" });

        // Convert (Media.cpp): every format from video and audio sources, copied, re-encoded and
        // held to a bitrate.
        const MediaInfo sources[] = { Video("h264", "aac"), Video("hevc", "ac3"), Video("vp9", "opus"), Video("mpeg4", "mp3"),
                                      Video("prores", "pcm_s24le"), Video("", "mp3"), Video("", "flac"), Video("", "wmav2"),
                                      Video("wmv3", "wmav2"), Video("h264", "") };
        for (const auto& f : MediaFormats())
            for (const auto& info : sources)
                for (const auto e : kEncoders)
                {
                    if (!MediaFormatAccepts(f, info.videoCodec.empty() ? MediaKind::Audio : MediaKind::Video))
                        continue;
                    auto plan = PlanConversion(f, info, e);
                    add(FfmpegArgs(in, InputArgs(plan), OutputArgs(f, plan), out));
                    auto full = TranscodePlan(plan);
                    add(FfmpegArgs(in, InputArgs(full), OutputArgs(f, full), out));
                    full.videoBps = CapVideoBps(50'000'000, info.durationUs, 0.8, !info.audioCodec.empty());
                    add(FfmpegArgs(in, InputArgs(full), OutputArgs(f, full), out));
                }
        add(ProbeArgs(in));

        // Compress and Fit to... (Media.cpp).
        for (const auto& info : sources)
            for (const auto e : kEncoders)
            {
                add(FfmpegArgs(in, {}, CompressArgs(info, e, nullptr, 0), out));
                add(FfmpegArgs(in, {}, CompressArgs(info, e, nullptr, 720), out));
                SizeFit fit;
                if (PlanSizeFit(10'000'000, info, fit))
                    add(FfmpegArgs(in, {}, CompressArgs(info, e, &fit, 0), out));
            }

        // Video tools.
        const std::wstring videoExts[] = { L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".avi", L".ts", L".mts", L".3gp", L".wmv", L".flv" };
        const ProbeInfo clips[] = { Probe("h264", "aac"), Probe("hevc", "opus"), Probe("vp9", ""), Probe("mpeg4", "mp3", 6) };
        for (const auto& ext : videoExts)
            for (const auto& clip : clips)
                for (const auto e : kEncoders)
                {
                    const TimeRange range{ 1'000'000, 4'000'000 };
                    add(videotools::TrimArgs(in, ext, clip, range, true, e, out));
                    add(videotools::TrimArgs(in, ext, clip, range, false, e, out));
                    for (const bool remove : { false, true })
                        for (const bool fade : { false, true })
                            add(videotools::TrimArgs(in, ext, clip, range, true, e, out, { remove, fade }));
                    add(videotools::CropArgs(in, ext, clip, { 10, 10, 640, 360 }, 1280, 720, e, out));
                    add(videotools::CropArgs(in, ext, clip, { 10, 10, 640, 360 }, 640, 360, e, out));
                    add(videotools::SpeedArgs(in, ext, clip, 0.25, e, out));
                    add(videotools::SpeedArgs(in, ext, clip, 4, e, out));
                    add(videotools::SpeedArgs(in, ext, clip, 0.25, e, out, false));
                    add(videotools::SpeedArgs(in, ext, clip, 4, e, out, false));
                    add(videotools::FrameToPngArgs(in, 1'500'000, clip.media.fps, out));
                    add(videotools::FrameSequenceArgs(in, { videotools::FramePick::Every, 30, 1 }, L"C:\\frames"));
                    add(videotools::FrameSequenceArgs(in, { videotools::FramePick::PerSecond, 30, 2.5 }, L"C:\\frames"));
                    add(videotools::SplitCopyArgs(in, ext, { 2'000'000, 5'000'000 }, L"C:\\parts", L"clip"));
                    add(videotools::SplitPartArgs(in, ext, clip, range, e, out));
                    add(videotools::RedactArgs(in, ext, clip, { { { 0, 0, 100, 100 }, range, true }, { { 50, 50, 80, 80 }, range, false } }, e, out));
                }
        for (const auto e : kEncoders)
            for (const bool fill : { false, true })
            {
                add(videotools::JoinCopyArgs(L"C:\\list.txt", clips[0], out));
                add(videotools::JoinEncodeArgs({ L"a.mp4", L"b.avi", L"c.mkv" }, { clips[0], clips[2], clips[3] }, e, out, fill));
                add(videotools::JoinEncodeArgs({ L"a.mp4", L"b.webm" }, { clips[2], clips[2] }, e, out, fill));
            }

        // Audio tools, for every audio format a tool can write.
        const std::wstring audioExts[] = { L".mp3", L".m4a", L".aac", L".wav", L".flac", L".ogg", L".opus", L".aiff", L".wma", L".ac3", L".amr", L".mka" };
        const ProbeInfo songs[] = { Probe("", "mp3"), Probe("", "aac", 1), Probe("", "flac", 6), Probe("h264", "aac") };
        const audiotools::LoudnessTarget targets[] = { audiotools::TargetFor(-14), audiotools::TargetFor(-16), audiotools::TargetFor(-23) };
        const audiotools::Loudness measured{ -20, -3, 6, -30, -16, -1, 6, -26, 0.2 };
        for (const auto& ext : audioExts)
            for (const auto& song : songs)
            {
                for (const auto& t : targets)
                {
                    add(audiotools::MeasureArgs(in, t));
                    add(audiotools::NormalizeArgs(in, ext, song, t, measured, out));
                }
                add(audiotools::SilenceDetectArgs(in, { 0, 5'000'000 }));
                add(audiotools::TrimAudioArgs(in, ext, song, { 1'000'000, 3'000'000 }, out));
                for (const bool mono : { false, true })
                    for (const bool swap : { false, true })
                        add(audiotools::ChannelsArgs(in, ext, song, { mono, swap, 0.8, 1.2 }, out));
                for (const auto sound : { audiotools::BleepSound::Tone, audiotools::BleepSound::Silence, audiotools::BleepSound::Duck })
                    add(audiotools::BleepArgs(in, ext, song, { { 1'000'000, 2'000'000 }, { 3'000'000, 3'500'000 } }, sound, audiotools::kDuckDb, out));
            }
        for (const auto& song : songs)
            for (const auto e : kEncoders)
                for (const auto shape : { audiotools::Shape::Landscape, audiotools::Shape::Portrait, audiotools::Shape::Square })
                {
                    for (const auto style : { audiotools::VisualStyle::Bars, audiotools::VisualStyle::Wave, audiotools::VisualStyle::Circle,
                                              audiotools::VisualStyle::Square, audiotools::VisualStyle::Vertical, audiotools::VisualStyle::Picture })
                        for (const wchar_t* image : { L"", L"C:\\background.jpg" })
                            add(audiotools::VisualizerArgs(in, image, song, style, shape, e, out));
                    add(audiotools::StillVideoArgs(in, L"C:\\cover.png", song, shape, e, out));
                }
        add(audiotools::CoverArtArgs(in, out));

        // The editor: filmstrip frames, the waveform, and playback through each tool's filter.
        add(FrameArgs(in, 2'000'000, 29.97, 160, 90, true, out));
        add(FrameArgs(in, 2'000'000, 29.97, 160, 90, false, out));
        add(MonoPcmArgs(in, WaveformRate(60'000'000), out));
        add(PlaybackPcmArgs(in, { 0, 5'000'000 }, {}, false, 48000, out));
        add(PlaybackPcmArgs(in, { 0, 5'000'000 }, audiotools::PanFilter({ true, false, 1, 1 }, 2), false, 48000, out));
        add(PlaybackPcmArgs(in, { 0, 5'000'000 }, videotools::AtempoChain(3), false, 48000, out));
        add(PlaybackPcmArgs(in, { 0, 5'000'000 }, videotools::SpeedAudioFilter(3, false, 48000), false, 48000, out));
        for (const auto sound : { audiotools::BleepSound::Tone, audiotools::BleepSound::Silence, audiotools::BleepSound::Duck })
            add(PlaybackPcmArgs(in, { 0, 5'000'000 }, audiotools::BleepGraph({ { 1'000'000, 2'000'000 } }, sound, audiotools::kDuckDb, 48000), true, 48000,
                                out));

        // Remove metadata / Share-safe: a stream copy into the source's own container.
        for (const wchar_t* ext : { L".mp4", L".m4v", L".mov", L".m4a", L".3gp", L".mkv", L".mka", L".webm", L".avi", L".mp3", L".flac",
                                    L".ogg", L".oga", L".opus", L".wav", L".aif", L".aiff", L".wmv", L".wma", L".asf", L".ts", L".mts",
                                    L".m2ts", L".mpg", L".mpeg" })
        {
            EXPECT_FALSE(metadata::MuxerFor(ext).empty()) << Narrow(ext);
            add(FfmpegArgs(in, {}, metadata::StripMediaArgs(ext), out));
        }
        return u;
    }
}

TEST(FfmpegComponents, ListIsThere)
{
    const auto c = Load();
    ASSERT_FALSE(c.empty()) << "cannot read " << ComponentsFile().string();
    for (const char* section : { "encoders", "decoders", "muxers", "demuxers", "filters", "protocols", "indevs", "hwaccels" })
        EXPECT_FALSE(c.contains(section) ? c.at(section).empty() : true) << section;
}

TEST(FfmpegComponents, EveryArgumentBuilderIsCovered)
{
    const auto c = Load();
    ASSERT_FALSE(c.empty()) << "cannot read " << ComponentsFile().string();
    const auto u = Everything();
    // Sanity: the builders were really walked.
    EXPECT_TRUE(u.encoders.contains("libvpx-vp9"));
    EXPECT_TRUE(u.muxers.contains("ipod"));
    EXPECT_TRUE(u.filters.contains("loudnorm"));
    EXPECT_TRUE(u.demuxers.contains("concat"));

    for (const auto& e : u.encoders)
        EXPECT_TRUE(Listed(c, "encoders", e)) << "encoder " << e << " is used but not in scripts/ffmpeg-components.txt";
    for (const auto& m : u.muxers)
        EXPECT_TRUE(Listed(c, "muxers", m)) << "muxer " << m << " is used but not in scripts/ffmpeg-components.txt";
    for (const auto& d : u.demuxers)
        EXPECT_TRUE(Listed(c, "demuxers", d) || Listed(c, "indevs", d)) << "input format " << d << " is used but not in scripts/ffmpeg-components.txt";
    for (const auto& f : u.filters)
        EXPECT_TRUE(Listed(c, "filters", f)) << "filter " << f << " is used but not in scripts/ffmpeg-components.txt";
    // -f lavfi frames reach the encoder through the wrapped_avframe decoder, and the null muxer
    // takes wrapped_avframe (video) and pcm_s16le (audio) when no codec is named.
    if (u.demuxers.contains("lavfi"))
        EXPECT_TRUE(Listed(c, "decoders", "wrapped_avframe"));
    if (u.muxers.contains("null"))
        EXPECT_TRUE(Listed(c, "encoders", "wrapped_avframe") && Listed(c, "encoders", "pcm_s16le"));
    for (const auto& h : u.hwaccels)
        EXPECT_EQ(h, "auto") << "-hwaccel " << h;
    // -hwaccel auto picks among the hardware decoders built in: DXVA2 and D3D11 on every GPU.
    EXPECT_TRUE(Listed(c, "hwaccels", "h264_dxva2") && Listed(c, "hwaccels", "h264_d3d11va") && Listed(c, "hwaccels", "hevc_d3d11va"));
    // Local files only, the -progress pipe, the sandbox's inherited handles.
    for (const char* p : { "file", "pipe", "fd" })
        EXPECT_TRUE(Listed(c, "protocols", p)) << p;
}

TEST(FfmpegComponents, TestClipsCanBeMade)
{
    // The tests make their clips with generated inputs (-f lavfi -i "testsrc=...") and filters
    // (-af/-vf): those must be in the build too, or the media tests fail on the bundled exe.
    const auto c = Load();
    ASSERT_FALSE(c.empty());
    const auto testsDir = ComponentsFile().parent_path().parent_path() / "tests";
    ASSERT_TRUE(std::filesystem::is_directory(testsDir)) << testsDir.string();
    const std::wregex graph(LR"re(L"(?:-i|-af|-vf|-filter_complex)", (?:std::format\()?L"([^"]*)")re");
    std::set<std::string> filters;
    for (const auto& e : std::filesystem::directory_iterator(testsDir))
    {
        if (e.path().extension() != ".cpp")
            continue;
        std::ifstream in(e.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        const std::wstring wide(text.begin(), text.end());   // the graphs are ASCII
        for (std::wsregex_iterator it(wide.begin(), wide.end(), graph), end; it != end; ++it)
        {
            std::wstring g = (*it)[1].str();
            // Only filtergraphs: an -i value is a graph when it names a source ("sine=..."), not a file.
            if (g.find(L'=') == std::wstring::npos || g.starts_with(L"fd:"))
                continue;
            // C++ escapes in the source: "\\," is an escaped comma in the graph.
            for (size_t at; (at = g.find(L"\\\\")) != std::wstring::npos;)
                g.replace(at, 2, L"\\");
            AddFilters(g, filters);
        }
    }
    EXPECT_TRUE(filters.contains("sine")) << "no test graphs found in " << testsDir.string();
    for (const auto& f : filters)
        if (f != "subtitles")   // the sandbox test only checks that such a graph is refused
            EXPECT_TRUE(Listed(c, "filters", f)) << "filter " << f << " is used by a test but not in scripts/ffmpeg-components.txt";
}

TEST(FfmpegComponents, EveryDroppableFileDecodes)
{
    // Each extension the wheel takes as video or audio (Media.cpp) needs its demuxer.
    const auto c = Load();
    ASSERT_FALSE(c.empty());
    const std::map<std::wstring, std::vector<std::string>> demuxers = {
        { L".mp4", { "mov" } }, { L".m4v", { "mov" } }, { L".mov", { "mov" } }, { L".qt", { "mov" } }, { L".3gp", { "mov" } }, { L".3g2", { "mov" } },
        { L".m4a", { "mov" } }, { L".m4b", { "mov" } }, { L".alac", { "mov", "caf" } }, { L".mkv", { "matroska" } }, { L".webm", { "matroska" } },
        { L".mka", { "matroska" } }, { L".avi", { "avi" } }, { L".divx", { "avi" } }, { L".wmv", { "asf" } }, { L".asf", { "asf" } },
        { L".wma", { "asf" } }, { L".flv", { "flv" } }, { L".mpg", { "mpeg", "mpegvideo" } }, { L".mpeg", { "mpeg" } }, { L".mpe", { "mpeg" } },
        { L".vob", { "mpeg" } }, { L".ts", { "mpegts" } }, { L".mts", { "mpegts" } }, { L".m2ts", { "mpegts" } }, { L".ogv", { "ogg" } },
        { L".ogg", { "ogg" } }, { L".oga", { "ogg" } }, { L".opus", { "ogg" } }, { L".mxf", { "mxf" } }, { L".mp3", { "mp3" } },
        { L".aac", { "aac" } }, { L".wav", { "wav" } }, { L".wave", { "wav" } }, { L".flac", { "flac" } }, { L".aif", { "aiff" } },
        { L".aiff", { "aiff" } }, { L".aifc", { "aiff" } }, { L".ac3", { "ac3" } }, { L".amr", { "amr" } }, { L".ape", { "ape" } },
        { L".wv", { "wv" } }, { L".caf", { "caf" } },
    };
    for (const auto& [ext, names] : demuxers)
    {
        EXPECT_NE(MediaKindOfExtension(ext), MediaKind::None) << Narrow(ext);
        for (const auto& d : names)
            EXPECT_TRUE(Listed(c, "demuxers", d)) << Narrow(ext) << " needs the " << d << " demuxer";
    }
    // And the codecs those files usually hold.
    for (const char* d : { "h264", "hevc", "vp8", "vp9", "libdav1d", "mpeg2video", "mpeg4", "msmpeg4", "prores", "dnxhd", "mjpeg", "wmv3", "vc1",
                           "dvvideo", "theora", "flv", "aac", "mp3float", "ac3", "eac3", "dca", "truehd", "flac", "alac", "opus", "vorbis",
                           "wmav2", "wmapro", "wmalossless", "amrnb", "amrwb", "ape", "wavpack", "pcm_s16le", "pcm_s24le", "pcm_f32le",
                           "pcm_bluray", "pcm_dvd", "adpcm_ima_wav", "png", "bmp", "webp", "tiff", "gif" })
        EXPECT_TRUE(Listed(c, "decoders", d)) << "decoder " << d;
}
