#include "Ffmpeg.h"
#include "JobQueue.h"
#include "Media.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

using namespace sd;
using namespace sd::test;

namespace
{
    bool Has(const std::vector<std::wstring>& args, std::wstring_view a, std::wstring_view b)
    {
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == a && args[i + 1] == b)
                return true;
        return false;
    }

    // Splits a command line the way the child process will (CommandLineToArgvW).
    std::vector<std::wstring> Split(const std::wstring& commandLine)
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(commandLine.c_str(), &argc);
        std::vector<std::wstring> out(argv, argv + argc);
        LocalFree(argv);
        return out;
    }

    // The real engine when CI (or a developer) points SHIFTDROP_FFMPEG at ffmpeg.exe; the tests that
    // need it are skipped otherwise.
    std::shared_ptr<const FfmpegEngine> Engine()
    {
        static const auto engine = std::make_shared<const FfmpegEngine>(FfmpegEngine::Locate());
        return engine;
    }
}

TEST(Media, KindsByExtension)
{
    EXPECT_EQ(MediaKindOfExtension(L".mp4"), MediaKind::Video);
    EXPECT_EQ(MediaKindOfExtension(L".mkv"), MediaKind::Video);
    EXPECT_EQ(MediaKindOfExtension(L".m2ts"), MediaKind::Video);
    EXPECT_EQ(MediaKindOfExtension(L".mp3"), MediaKind::Audio);
    EXPECT_EQ(MediaKindOfExtension(L".flac"), MediaKind::Audio);
    EXPECT_EQ(MediaKindOfExtension(L".gif"), MediaKind::None);   // stays an image
    EXPECT_EQ(MediaKindOfExtension(L".png"), MediaKind::None);
    EXPECT_EQ(MediaKindOfExtension(L".m4"), MediaKind::None);    // only whole extensions match
    EXPECT_EQ(MediaKindOfExtension(L".mp"), MediaKind::None);
    EXPECT_EQ(MediaKindOfExtension(L""), MediaKind::None);
}

TEST(Media, FormatsAndAliases)
{
    const auto* mp4 = FindMediaFormat("mp4");
    ASSERT_NE(mp4, nullptr);
    EXPECT_TRUE(IsAlreadyMediaFormat(*mp4, L".m4v"));
    EXPECT_FALSE(IsAlreadyMediaFormat(*mp4, L".mov"));
    EXPECT_TRUE(MediaFormatAccepts(*mp4, MediaKind::Video));
    EXPECT_FALSE(MediaFormatAccepts(*mp4, MediaKind::Audio));
    const auto* mp3 = FindMediaFormat("mp3");
    ASSERT_NE(mp3, nullptr);
    EXPECT_TRUE(MediaFormatAccepts(*mp3, MediaKind::Video));   // the soundtrack of a video
    EXPECT_TRUE(MediaFormatAccepts(*mp3, MediaKind::Audio));
    EXPECT_EQ(FindMediaFormat("nope"), nullptr);
    // Every format names its muxer: the staging file's ".sdpart" extension says nothing to ffmpeg.
    for (const auto& f : MediaFormats())
    {
        const auto args = OutputArgs(f, MediaPlan{});
        EXPECT_NE(std::find(args.begin(), args.end(), L"-f"), args.end()) << f.id;
    }
}

TEST(Media, OutputArgsPerFormat)
{
    MediaPlan nvenc;
    nvenc.h264 = H264Encoder::Nvenc;
    const auto mp4 = OutputArgs(*FindMediaFormat("mp4"), nvenc);
    EXPECT_TRUE(Has(mp4, L"-c:v", L"h264_nvenc"));
    EXPECT_TRUE(Has(mp4, L"-c:a", L"aac"));
    EXPECT_TRUE(Has(mp4, L"-movflags", L"+faststart"));
    EXPECT_TRUE(Has(mp4, L"-f", L"mp4"));
    // MKV copies the streams first, and re-encodes only as the fallback.
    const auto& mkv = *FindMediaFormat("mkv");
    const auto mkvPlan = PlanConversion(mkv, MediaInfo{}, H264Encoder::OpenH264);
    EXPECT_TRUE(Has(OutputArgs(mkv, mkvPlan), L"-c", L"copy"));
    EXPECT_TRUE(Has(OutputArgs(mkv, TranscodePlan(mkvPlan)), L"-c:v", L"libopenh264"));
    // OpenH264 gets a slice per core (one slice is one core), at a quality or a bitrate; the GPU
    // encoders thread themselves.
    const auto cores = std::to_wstring(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
    const auto openh264 = H264QualityArgs(H264Encoder::OpenH264, 23, 8'000'000);
    EXPECT_TRUE(Has(openh264, L"-slices", cores));
    EXPECT_TRUE(Has(openh264, L"-threads", cores));
    MediaPlan slicedAtABitrate;
    slicedAtABitrate.h264 = H264Encoder::OpenH264;
    slicedAtABitrate.videoBps = 2'000'000;
    EXPECT_TRUE(Has(OutputArgs(*FindMediaFormat("mp4"), slicedAtABitrate), L"-slices", cores));
    EXPECT_EQ(std::find(mp4.begin(), mp4.end(), L"-slices"), mp4.end());
    // VP9 in realtime mode at speed 6 (six times faster than "good" at 5, same quality).
    const auto webm = OutputArgs(*FindMediaFormat("webm"), MediaPlan{});
    EXPECT_TRUE(Has(webm, L"-c:v", L"libvpx-vp9"));
    EXPECT_TRUE(Has(webm, L"-deadline", L"realtime"));
    EXPECT_TRUE(Has(webm, L"-cpu-used", L"6"));
    EXPECT_TRUE(Has(webm, L"-crf", L"32"));
    // Audio formats drop the video (cover art included) and take the first audio stream.
    const auto m4a = OutputArgs(*FindMediaFormat("m4a"), MediaPlan{});
    EXPECT_TRUE(Has(m4a, L"-map", L"0:a:0"));
    EXPECT_NE(std::find(m4a.begin(), m4a.end(), L"-vn"), m4a.end());
    EXPECT_TRUE(Has(m4a, L"-f", L"ipod"));
    // Held to a bitrate (a Convert that came out bigger than its source).
    MediaPlan capped;
    capped.videoBps = 900'000;
    EXPECT_TRUE(Has(OutputArgs(*FindMediaFormat("mp4"), capped), L"-b:v", L"900000"));
    EXPECT_TRUE(Has(OutputArgs(*FindMediaFormat("mp4"), capped), L"-maxrate", L"900000"));
    EXPECT_TRUE(Has(OutputArgs(*FindMediaFormat("webm"), capped), L"-b:v", L"900000"));
    EXPECT_TRUE(Has(OutputArgs(*FindMediaFormat("avi"), capped), L"-b:v", L"900000"));
    EXPECT_FALSE(Has(OutputArgs(*FindMediaFormat("avi"), capped), L"-q:v", L"3"));
}

TEST(Media, ConvertCapFollowsTheSource)
{
    // 10 MB over 10 s = 8 Mbit/s; 90% of it, less 192k for the sound.
    EXPECT_EQ(CapVideoBps(10'000'000, 10'000'000, 0.9, true), 7'200'000 - 192'000);
    EXPECT_EQ(CapVideoBps(10'000'000, 10'000'000, 0.9, false), 7'200'000);
    // A tiny source: the sound gets at most a quarter.
    EXPECT_EQ(CapVideoBps(100'000, 10'000'000, 1.0, true), 60'000);
    EXPECT_EQ(CapVideoBps(1'000, 10'000'000, 1.0, false), 32'000);   // the floor
    EXPECT_EQ(CapVideoBps(10'000'000, -1, 0.9, true), 0);            // length unknown
}

TEST(Media, ProbeLogIsParsed)
{
    const std::vector<std::string> log = {
        "Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'IMG_0042.MOV':",
        "  Duration: 00:00:12.50, start: 0.000000, bitrate: 15876 kb/s",
        "  Stream #0:0[0x1](und): Video: hevc (Main) (hvc1 / 0x31637668), yuv420p(tv, bt709), 1920x1080, 15600 kb/s, 29.97 fps, 30 tbr, 600 tbn (default)",
        "  Stream #0:1[0x2](und): Audio: aac (LC) (mp4a / 0x6134706D), 44100 Hz, stereo, fltp, 191 kb/s (default)",
    };
    const auto info = ParseMediaInfo(log);
    EXPECT_EQ(info.durationUs, 12'500'000);
    EXPECT_EQ(info.videoCodec, "hevc");
    EXPECT_EQ(info.audioCodec, "aac");
    EXPECT_EQ(info.width, 1920);
    EXPECT_EQ(info.height, 1080);
    EXPECT_NEAR(info.fps, 29.97, 0.001);

    // A song with cover art has no video stream.
    const auto song = ParseMediaInfo({ "  Duration: 00:03:00.00, start: 0.0",
                                       "  Stream #0:0: Audio: mp3, 44100 Hz, stereo, fltp, 320 kb/s",
                                       "  Stream #0:1: Video: mjpeg (Baseline), yuvj420p, 600x600, 90k tbr (attached pic)" });
    EXPECT_TRUE(song.videoCodec.empty());
    EXPECT_EQ(song.audioCodec, "mp3");
}

TEST(Media, CopiesStreamsTheContainerPlays)
{
    MediaInfo phone;
    phone.videoCodec = "hevc";
    phone.audioCodec = "aac";
    // iPhone HEVC MOV -> MP4: both streams copied, tagged hvc1, nothing encoded.
    const auto mp4 = PlanConversion(*FindMediaFormat("mp4"), phone, H264Encoder::Nvenc);
    EXPECT_TRUE(mp4.copyVideo);
    EXPECT_TRUE(mp4.copyAudio);
    const auto args = OutputArgs(*FindMediaFormat("mp4"), mp4);
    EXPECT_TRUE(Has(args, L"-c:v", L"copy"));
    EXPECT_TRUE(Has(args, L"-tag:v", L"hvc1"));
    EXPECT_TRUE(Has(args, L"-c:a", L"copy"));
    EXPECT_EQ(std::find(args.begin(), args.end(), L"-vf"), args.end());   // no filter on a copied stream
    // To WebM: HEVC isn't allowed, AAC isn't either: both encoded.
    const auto webm = PlanConversion(*FindMediaFormat("webm"), phone, H264Encoder::Nvenc);
    EXPECT_FALSE(webm.copyVideo);
    EXPECT_FALSE(webm.copyAudio);
    // To M4A: the AAC soundtrack is copied.
    MediaInfo song;
    song.audioCodec = "aac";
    EXPECT_TRUE(PlanConversion(*FindMediaFormat("m4a"), song, H264Encoder::OpenH264).copyAudio);
    // An MPEG-4 Part 2 AVI to MP4 is encoded to H.264 (phones don't play Part 2), on the GPU.
    MediaInfo xvid;
    xvid.videoCodec = "mpeg4";
    xvid.audioCodec = "mp3";
    const auto fromAvi = PlanConversion(*FindMediaFormat("mp4"), xvid, H264Encoder::Qsv);
    EXPECT_FALSE(fromAvi.copyVideo);
    EXPECT_TRUE(fromAvi.copyAudio);
    EXPECT_TRUE(fromAvi.hwDecode);
    EXPECT_TRUE(Has(InputArgs(fromAvi), L"-hwaccel", L"auto"));
    EXPECT_FALSE(TranscodePlan(fromAvi).copyAudio);
}

TEST(Media, SizeFitPicksBitratesAndHeight)
{
    MediaInfo clip;
    clip.durationUs = 60'000'000;   // one minute of 1080p30
    clip.videoCodec = "h264";
    clip.audioCodec = "aac";
    clip.width = 1920;
    clip.height = 1080;
    clip.fps = 30;
    SizeFit fit;
    ASSERT_TRUE(PlanSizeFit(10LL * 1024 * 1024, clip, fit));
    // 10 MB over 60 s is ~1.34 Mbit/s in all: 96k audio, the rest video, at 540p.
    EXPECT_EQ(fit.audioBps, 96'000);
    EXPECT_NEAR(static_cast<double>(fit.videoBps), 1'246'000, 10'000);
    EXPECT_EQ(fit.height, 540);
    // The bitrates and the container margin stay under the target.
    EXPECT_LT((fit.videoBps + fit.audioBps) * 60 / 8, 10LL * 1024 * 1024);
    // A generous target keeps the source's height.
    ASSERT_TRUE(PlanSizeFit(500LL * 1024 * 1024, clip, fit));
    EXPECT_EQ(fit.height, 0);
    // A target far too small for an hour of video is refused rather than made unwatchable.
    clip.durationUs = 3600LL * 1'000'000;
    EXPECT_FALSE(PlanSizeFit(5LL * 1024 * 1024, clip, fit));
    // Audio only: one bitrate, capped at 256k.
    MediaInfo song;
    song.durationUs = 180'000'000;
    song.audioCodec = "flac";
    ASSERT_TRUE(PlanSizeFit(3LL * 1024 * 1024, song, fit));
    EXPECT_EQ(fit.videoBps, 0);
    EXPECT_GT(fit.audioBps, 100'000);
    EXPECT_LE(fit.audioBps, 256'000);
    const auto args = CompressArgs(song, H264Encoder::OpenH264, &fit, 0);
    EXPECT_TRUE(Has(args, L"-f", L"ipod"));
}

TEST(Media, CommandLineIsLocalOnlyAndQuoted)
{
    const std::wstring input = LR"(C:\Users\a b\clip "final".mov)";
    const std::wstring output = LR"(C:\Users\a b\~clip.1234abcd.sdpart)";
    const auto args = FfmpegArgs(input, {}, OutputArgs(*FindMediaFormat("webm"), MediaPlan{}), output);
    EXPECT_TRUE(Has(args, L"-protocol_whitelist", L"file"));   // no network protocols, whatever the path says
    EXPECT_TRUE(Has(args, L"-progress", L"pipe:1"));
    EXPECT_TRUE(Has(args, L"-i", input));
    EXPECT_EQ(args.back(), output);

    std::wstring commandLine = L"ffmpeg.exe";
    for (const auto& a : args)
        commandLine += L' ' + QuoteArg(a);
    auto back = Split(commandLine);
    back.erase(back.begin());
    EXPECT_EQ(back, args);

    // The awkward cases of CommandLineToArgvW: trailing backslashes, quotes after backslashes, empty.
    for (const std::wstring tricky : { L"", L"a\\", L"a b\\", L"a\\\"b", L"\\\\server\\share\\x y\\", L"tab\there" })
    {
        const auto parsed = Split(L"x " + QuoteArg(tricky));
        ASSERT_EQ(parsed.size(), 2u) << Narrow(tricky);
        EXPECT_EQ(parsed[1], tricky);
    }
}

TEST(Media, ParsesDurationAndProgress)
{
    EXPECT_EQ(ParseDurationLine("  Duration: 00:01:23.45, start: 0.000000, bitrate: 1411 kb/s"), 83'450'000);
    EXPECT_EQ(ParseDurationLine("  Duration: 01:00:00.00, start: 0.0"), 3'600'000'000);
    EXPECT_EQ(ParseDurationLine("  Duration: N/A, bitrate: N/A"), -1);
    EXPECT_EQ(ParseDurationLine("Stream #0:0: Video: h264"), -1);
    EXPECT_EQ(ParseProgressLine("out_time_us=12345678"), 12'345'678);
    EXPECT_EQ(ParseProgressLine("out_time_us=N/A"), -1);
    EXPECT_EQ(ParseProgressLine("out_time_ms=12345678"), -1);
    EXPECT_EQ(ParseProgressLine("progress=end"), -1);
}

TEST(Media, RecipesApplyByKind)
{
    TempDir dir;
    Recipes recipes(dir);
    const PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr, nullptr);
    const auto& mp4 = recipes.Get("to-mp4");
    const auto& mp3 = recipes.Get("to-mp3");
    const auto& png = recipes.Get("to-png");
    EXPECT_TRUE(mp4.IsMedia());
    EXPECT_EQ(mp4.OutputExtensionFor(L".mov"), L".mp4");
    EXPECT_TRUE(runner.Applies(mp4, L"C:\\v\\clip.mov"));
    EXPECT_FALSE(runner.Applies(mp4, L"C:\\v\\song.flac"));
    EXPECT_FALSE(runner.Applies(mp4, L"C:\\v\\photo.png"));
    EXPECT_TRUE(runner.Applies(mp3, L"C:\\v\\song.flac"));
    EXPECT_TRUE(runner.Applies(mp3, L"C:\\v\\clip.mov"));
    EXPECT_FALSE(runner.Applies(png, L"C:\\v\\clip.mov"));
    EXPECT_TRUE(runner.Applies(png, L"C:\\v\\photo.jpg"));
    EXPECT_TRUE(runner.IsNoOp(mp4, L"C:\\v\\clip.m4v"));
    // Video runs one at a time (one ffmpeg already uses every core); sound runs several files at once.
    EXPECT_EQ(runner.Lane(mp4), JobLane::Media);
    EXPECT_EQ(runner.Lane(recipes.Get("to-gif")), JobLane::Media);
    EXPECT_EQ(runner.Lane(mp3), JobLane::Audio);
    EXPECT_EQ(runner.Lane(recipes.Get("to-m4a")), JobLane::Audio);
    EXPECT_EQ(runner.Lane(png), JobLane::Pool);
    // Without the engine a media recipe says why it can't run.
    std::wstring why;
    EXPECT_FALSE(runner.Accepts(mp4, L"C:\\v\\clip.mov", &why));
    EXPECT_NE(why.find(L"media engine"), std::wstring::npos);
}

TEST(Media, PoolLeavesACoreFree)
{
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threads = JobQueue::PoolThreads();
    EXPECT_GE(threads, 1u);
    EXPECT_LE(threads, 8u);
    EXPECT_EQ(threads, std::clamp(cores > 1 ? cores - 1 : 1u, 1u, 8u));
}

TEST(Media, MediaStepMustBeAlone)
{
    ActionRegistry registry;
    Recipe r;
    std::string error;
    EXPECT_FALSE(ParseRecipe(R"({ "id": "x", "label": "X", "steps": [
        { "action": "media.convert", "params": { "format": "mp4" } }, { "action": "image.encode" } ] })", registry, r, error));
    EXPECT_NE(error.find("only step"), std::string::npos) << error;
    EXPECT_FALSE(ParseRecipe(R"({ "id": "x", "label": "X", "steps": [ { "action": "media.convert", "params": { "format": "mpeg9" } } ] })",
                             registry, r, error));
}

// ---- With the real ffmpeg.exe (SHIFTDROP_FFMPEG) ---------------------------------------------------

namespace
{
    // A 2-second 321x241 test pattern with a tone (odd size on purpose), written by ffmpeg itself.
    bool MakeClip(const FfmpegEngine& engine, const std::wstring& path)
    {
        std::stop_source never;
        return engine.Run({ L"-hide_banner", L"-nostdin", L"-f", L"lavfi", L"-i", L"testsrc=s=321x241:r=25:d=2", L"-f", L"lavfi",
                            L"-i", L"sine=f=440:d=2", L"-c:v", L"mpeg4", L"-c:a", L"pcm_s16le", L"-shortest", L"-y", L"-f", L"avi", path },
                          nullptr, never.get_token()).ok;
    }
}

TEST(MediaEngine, ConvertsThroughThePipeline)
{
    const auto engine = Engine();
    if (!engine->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    TempDir dir;
    Recipes recipes(dir);
    std::wstring error;
    auto history = HistoryStore::Open(dir / L"history.db", error);
    const PipelineRunner runner(std::make_shared<const ImageCodecs>(), history.get(), engine);
    const auto source = dir / L"my clip.avi";
    ASSERT_TRUE(MakeClip(*engine, source));

    for (const char* id : { "to-mp4", "to-mkv", "to-webm", "to-gif", "to-mp3", "to-mov" })
    {
        const auto& recipe = recipes.Get(id);
        const auto runId = history->BeginRun(recipe, 1);
        double last = 0.0;
        const auto r = runner.Run(recipe, runId, source, std::stop_token{}, [&](double p) { last = std::max(last, p); });
        history->FinishRun(runId);
        ASSERT_TRUE(r.ok) << id << ": " << Narrow(r.error);
        EXPECT_EQ(std::filesystem::path(r.output).filename(), L"my clip (converted)" + recipe.OutputExtensionFor(L".avi")) << id;
        EXPECT_GT(r.size, 0u) << id;
        EXPECT_GT(last, 0.5) << id << ": progress was reported";
        EXPECT_EQ(history->RecentRuns(1).at(0).status, status::Done) << id;
    }
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
}

TEST(MediaEngine, ConvertNeverComesOutBigger)
{
    const auto engine = Engine();
    if (!engine->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    TempDir dir;
    Recipes recipes(dir);
    const PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr, engine);
    // Heavily compressed already: a quality-based MP4 would be bigger than this.
    const auto source = dir / L"tiny.avi";
    std::stop_source never;
    ASSERT_TRUE(engine->Run({ L"-hide_banner", L"-nostdin", L"-f", L"lavfi", L"-i", L"testsrc=s=640x360:r=25:d=4", L"-c:v", L"mpeg4",
                              L"-q:v", L"31", L"-y", L"-f", L"avi", source }, nullptr, never.get_token()).ok);
    const auto sourceBytes = std::filesystem::file_size(source);
    // MP4 aims at a quality and retries when too big; WebM is held to the source's bitrate from the
    // first run. (Not WMV here: its encoder can't go below this source's q31 floor, so it errors out.)
    for (const char* id : { "to-mp4", "to-webm" })
    {
        const auto r = runner.Run(recipes.Get(id), 0, source, std::stop_token{}, nullptr);
        ASSERT_TRUE(r.ok) << id << ": " << Narrow(r.error);
        EXPECT_LE(r.size, sourceBytes) << id;
    }
}

TEST(MediaEngine, CancelStopsFfmpegAndLeavesNothing)
{
    const auto engine = Engine();
    if (!engine->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    TempDir dir;
    Recipes recipes(dir);
    const PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr, engine);
    // Long enough that it is still running when the stop comes.
    const auto source = dir / L"long.avi";
    std::stop_source never;
    ASSERT_TRUE(engine->Run({ L"-hide_banner", L"-nostdin", L"-f", L"lavfi", L"-i", L"testsrc=s=640x360:r=30:d=60", L"-c:v", L"mpeg4",
                              L"-q:v", L"31", L"-y", L"-f", L"avi", source }, nullptr, never.get_token()).ok);

    std::stop_source stop;
    std::atomic<bool> started{ false };
    std::jthread canceller([&](std::stop_token own) {   // `own` ends the wait if Run returns first
        while (!started && !own.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        stop.request_stop();
    });
    const auto r = runner.Run(recipes.Get("to-webm"), 0, source, stop.get_token(), [&](double) { started = true; });
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.skipped);
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
    EXPECT_EQ(CountFiles(dir, L".webm"), 0u);
}

TEST(MediaEngine, CompressKeepsUnderTheTarget)
{
    const auto engine = Engine();
    if (!engine->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    TempDir dir;
    Recipes recipes(dir);
    const PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr, engine);
    // A big, noisy source (near-lossless MPEG-4 of random noise), so there is something to squeeze.
    const auto source = dir / L"big.avi";
    std::stop_source never;
    ASSERT_TRUE(engine->Run({ L"-hide_banner", L"-nostdin", L"-f", L"lavfi", L"-i", L"nullsrc=s=640x360:r=25:d=6,geq=random(1)*255:128:128",
                              L"-f", L"lavfi", L"-i", L"sine=d=6", L"-c:v", L"mpeg4", L"-q:v", L"2", L"-c:a", L"pcm_s16le", L"-shortest",
                              L"-y", L"-f", L"avi", source }, nullptr, never.get_token()).ok);
    const auto sourceBytes = std::filesystem::file_size(source);

    const auto half = runner.Run(recipes.Get("media-half"), 0, source, std::stop_token{});
    ASSERT_TRUE(half.ok) << Narrow(half.error);
    EXPECT_EQ(std::filesystem::path(half.output).filename(), L"big (compressed).mp4");
    EXPECT_LE(std::filesystem::file_size(half.output), sourceBytes / 2);

    const auto preset = runner.Run(recipes.Get("compress-media"), 0, source, std::stop_token{});
    ASSERT_TRUE(preset.ok) << Narrow(preset.error);
    EXPECT_LT(std::filesystem::file_size(preset.output), sourceBytes);
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
}
