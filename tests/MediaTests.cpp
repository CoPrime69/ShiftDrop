#include "Ffmpeg.h"
#include "Media.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"

#include <shellapi.h>

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
        const auto args = OutputArgs(f, H264Encoder::OpenH264);
        EXPECT_NE(std::find(args.begin(), args.end(), L"-f"), args.end()) << f.id;
    }
}

TEST(Media, OutputArgsPerFormat)
{
    const auto mp4 = OutputArgs(*FindMediaFormat("mp4"), H264Encoder::Nvenc);
    EXPECT_TRUE(Has(mp4, L"-c:v", L"h264_nvenc"));
    EXPECT_TRUE(Has(mp4, L"-c:a", L"aac"));
    EXPECT_TRUE(Has(mp4, L"-movflags", L"+faststart"));
    EXPECT_TRUE(Has(mp4, L"-f", L"mp4"));
    // MKV copies the streams first, and re-encodes only as the fallback.
    const auto& mkv = *FindMediaFormat("mkv");
    EXPECT_TRUE(Has(OutputArgs(mkv, H264Encoder::OpenH264), L"-c", L"copy"));
    EXPECT_TRUE(Has(OutputArgs(mkv, H264Encoder::OpenH264, true), L"-c:v", L"libopenh264"));
    // Audio formats drop the video (cover art included) and take the first audio stream.
    const auto m4a = OutputArgs(*FindMediaFormat("m4a"), H264Encoder::OpenH264);
    EXPECT_TRUE(Has(m4a, L"-map", L"0:a:0"));
    EXPECT_NE(std::find(m4a.begin(), m4a.end(), L"-vn"), m4a.end());
    EXPECT_TRUE(Has(m4a, L"-f", L"ipod"));
}

TEST(Media, CommandLineIsLocalOnlyAndQuoted)
{
    const std::wstring input = LR"(C:\Users\a b\clip "final".mov)";
    const std::wstring output = LR"(C:\Users\a b\~clip.1234abcd.sdpart)";
    const auto args = FfmpegArgs(input, OutputArgs(*FindMediaFormat("webm"), H264Encoder::OpenH264), output);
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
    // Without the engine a media recipe says why it can't run.
    std::wstring why;
    EXPECT_FALSE(runner.Accepts(mp4, L"C:\\v\\clip.mov", &why));
    EXPECT_NE(why.find(L"media engine"), std::wstring::npos);
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

    for (const char* id : { "to-mp4", "to-mkv", "to-webm", "to-gif", "to-mp3" })
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
