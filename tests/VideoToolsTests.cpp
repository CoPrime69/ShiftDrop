#include "Ffmpeg.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "mediaeditor/EditorApi.h"
#include "mediaeditor/EditorLayout.h"
#include "mediaeditor/MediaProbe.h"
#include "mediaeditor/MediaTools.h"
#include "mediaeditor/MediaToolsModule.h"
#include "mediaeditor/Timeline.h"
#include "videotools/VideoEngine.h"
#include "videotools/VideoTools.h"

#include <algorithm>
#include <filesystem>

using namespace sd;
using namespace sd::mediatools;
using namespace sd::videotools;
using namespace sd::test;

namespace
{
    // A video tool's recipe by id, whether it has a slice or is reached from the editor's tool bar.
    Recipe ToolById(const Recipes& recipes, std::string_view id)
    {
        if (const auto* r = recipes.store.Find(id))
            return *r;
        for (const auto tool : SiblingTools(EditorTool::TrimVideo))
            if (const auto j = ToolRecipe(tool); j.value("id", std::string()) == id)
            {
                Recipe r;
                std::string error;
                EXPECT_TRUE(ParseRecipe(j.dump(), recipes.registry, r, error)) << error;
                return r;
            }
        ADD_FAILURE() << "no recipe " << std::string(id);
        return {};
    }

    bool Has(const std::vector<std::wstring>& args, std::wstring_view a, std::wstring_view b)
    {
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == a && args[i + 1] == b)
                return true;
        return false;
    }

    bool Contains(const std::vector<std::wstring>& args, std::wstring_view part)
    {
        return std::any_of(args.begin(), args.end(), [&](const std::wstring& a) { return a.find(part) != std::wstring::npos; });
    }

    // The value after `key`, or empty.
    std::wstring After(const std::vector<std::wstring>& args, std::wstring_view key)
    {
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == key)
                return args[i + 1];
        return {};
    }

    ProbeInfo Clip(int w = 1920, int h = 1080, double fps = 30, const char* video = "h264", const char* audio = "aac", int64_t us = 10'000'000)
    {
        ProbeInfo p;
        p.media.videoCodec = video;
        p.media.audioCodec = audio;
        p.media.width = p.width = w;
        p.media.height = p.height = h;
        p.media.fps = fps;
        p.media.durationUs = us;
        p.channels = *audio ? 2 : 0;
        p.sampleRate = *audio ? 48000 : 0;
        return p;
    }
}

// ---- Timeline maths ------------------------------------------------------------------------------

TEST(Timeline, TimecodesRoundTrip)
{
    EXPECT_EQ(FormatTimecode(0), L"00:00:00.000");
    EXPECT_EQ(FormatTimecode(83'456'000), L"00:01:23.456");
    EXPECT_EQ(FormatTimecode(3'723'004'600), L"01:02:03.005");   // rounded to the millisecond
    int64_t us = 0;
    EXPECT_TRUE(ParseTimecode(L"00:01:23.456", us));
    EXPECT_EQ(us, 83'456'000);
    EXPECT_TRUE(ParseTimecode(L"1:23.5", us));
    EXPECT_EQ(us, 83'500'000);
    EXPECT_TRUE(ParseTimecode(L" 83.25 ", us));
    EXPECT_EQ(us, 83'250'000);
    EXPECT_TRUE(ParseTimecode(L"1:02:03", us));
    EXPECT_EQ(us, 3'723'000'000);
    EXPECT_TRUE(ParseTimecode(L"12", us));
    EXPECT_EQ(us, 12'000'000);
    EXPECT_TRUE(ParseTimecode(L"0,5", us));   // a decimal comma, as typed in many locales
    EXPECT_EQ(us, 500'000);
    for (const wchar_t* bad : { L"", L"abc", L"1:60", L"1::2", L"-3", L"1:2:3:4", L"1.5:20", L"." })
        EXPECT_FALSE(ParseTimecode(bad, us)) << Narrow(bad);
    EXPECT_EQ(FormatRulerLabel(65'000'000, 5'000'000), L"1:05");
    EXPECT_EQ(FormatRulerLabel(3'725'000'000, 60'000'000), L"1:02:05");
    EXPECT_EQ(FormatRulerLabel(1'500'000, 500'000), L"0:01.5");
}

TEST(Timeline, FrameGrid)
{
    EXPECT_EQ(FrameDurationUs(25), 40'000);
    EXPECT_EQ(FrameDurationUs(0), 0);
    EXPECT_EQ(SnapToFrame(1'019'999, 25), 1'000'000);   // inside frame 25
    EXPECT_EQ(SnapToFrame(1'040'000, 25), 1'040'000);
    // 29.97 fps: frame 1800 starts at 60.06 s, not 60 s.
    EXPECT_EQ(SnapToFrame(60'060'100, 30000.0 / 1001), 60'060'000);
    EXPECT_EQ(StepFrames(1'000'000, 25, 1, 10'000'000), 1'040'000);
    EXPECT_EQ(StepFrames(1'000'000, 25, -2, 10'000'000), 920'000);
    EXPECT_EQ(StepFrames(20'000, 25, -5, 10'000'000), 0);
    EXPECT_EQ(LastFrameTime(25, 2'000'000), 1'960'000);
    EXPECT_EQ(StepFrames(1'960'000, 25, 3, 2'000'000), 1'960'000);   // never past the last frame
    EXPECT_EQ(StepFrames(500'000, 0, 1, 2'000'000), 510'000);       // audio: 10 ms steps
    // The preview and Save Frames seek a quarter frame early, so rounding never skips the frame.
    EXPECT_EQ(FrameSeekUs(1'000'000, 25), 990'000);
    EXPECT_EQ(FrameSeekUs(0, 25), 0);
}

TEST(Timeline, RangesAndPoints)
{
    const auto merged = NormalizeRanges({ { 5'000'000, 7'000'000 }, { 1'000'000, 2'000'000 }, { 6'000'000, 8'000'000 }, { 9'000'000, 9'010'000 },
                                          { -5, 500'000 }, { 9'500'000, 12'000'000 } },
                                        10'000'000);
    ASSERT_EQ(merged.size(), 4u);
    EXPECT_EQ(merged[0], (TimeRange{ 0, 500'000 }));
    EXPECT_EQ(merged[1], (TimeRange{ 1'000'000, 2'000'000 }));
    EXPECT_EQ(merged[2], (TimeRange{ 5'000'000, 8'000'000 }));      // overlaps merged
    EXPECT_EQ(merged[3], (TimeRange{ 9'500'000, 10'000'000 }));     // clamped to the clip; the 10 ms one dropped
    EXPECT_EQ(NormalizePoints({ 3'000'000, 1'000'000, 1'010'000, 0, 10'000'000, 5'000'000 }, 10'000'000),
              (std::vector<int64_t>{ 1'000'000, 3'000'000, 5'000'000 }));
    const auto parts = PartsFromPoints({ 4'000'000, 2'000'000 }, 6'000'000);
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[1], (TimeRange{ 2'000'000, 4'000'000 }));
    EXPECT_EQ(parts[2].end, 6'000'000);
    EXPECT_EQ(EqualPartPoints(9'000'000, 3), (std::vector<int64_t>{ 3'000'000, 6'000'000 }));
    EXPECT_TRUE(EqualPartPoints(9'000'000, 1).empty());

    // Dragging an edge stops at the neighbour and keeps the range at least 50 ms long.
    const std::vector<TimeRange> list{ { 1'000'000, 2'000'000 }, { 3'000'000, 4'000'000 } };
    EXPECT_EQ(ClampEdge(list, 1, true, 1'500'000, 10'000'000), 2'000'000);
    EXPECT_EQ(ClampEdge(list, 1, true, 3'990'000, 10'000'000), 3'950'000);
    EXPECT_EQ(ClampEdge(list, 0, false, 5'000'000, 10'000'000), 3'000'000);
    EXPECT_EQ(ClampEdge(list, 1, false, 20'000'000, 10'000'000), 10'000'000);
    TimeRange fresh;
    ASSERT_TRUE(FitNewRange(list, 2'500'000, 2'000'000, 10'000'000, fresh));
    EXPECT_EQ(fresh, (TimeRange{ 2'000'000, 3'000'000 }));          // squeezed into the gap
    EXPECT_FALSE(FitNewRange(list, 1'500'000, 1'000'000, 10'000'000, fresh));   // inside a range
}

TEST(Timeline, ViewZoomsAroundTheCursor)
{
    TimelineView v;
    v.durationUs = 60'000'000;
    v.widthPx = 600;
    v.Fit();
    EXPECT_DOUBLE_EQ(v.usPerPx, 100'000);
    EXPECT_EQ(v.TimeAt(300), 30'000'000);
    v.ZoomAt(4, 150);   // the time under x = 150 (15 s) stays there
    EXPECT_DOUBLE_EQ(v.usPerPx, 25'000);
    EXPECT_EQ(v.TimeAt(150), 15'000'000);
    EXPECT_TRUE(v.Zoomed());
    v.ScrollBy(-10'000);   // cannot scroll before the start
    EXPECT_EQ(v.startUs, 0);
    v.EnsureVisible(50'000'000);
    EXPECT_GT(v.EndUs(), 50'000'000);
    EXPECT_LT(v.startUs, 50'000'000);
    v.ZoomAt(0.001, 0);   // zooming out stops at the whole clip
    EXPECT_FALSE(v.Zoomed());
    EXPECT_EQ(v.startUs, 0);
    v.ZoomAt(1e9, 0);     // and zooming in at half a millisecond per pixel
    EXPECT_DOUBLE_EQ(v.usPerPx, 500);
    EXPECT_EQ(TickStepUs(100'000, 60), 10'000'000);   // 100 px apart
    EXPECT_EQ(TickStepUs(500, 60), 50'000);
    EXPECT_EQ(FilmstripTimes(10'000'000, 5), (std::vector<int64_t>{ 1'000'000, 3'000'000, 5'000'000, 7'000'000, 9'000'000 }));
}

TEST(Timeline, PeakPyramidMatchesAScan)
{
    std::vector<int16_t> samples(100'000);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>((static_cast<int>(i * 7919 % 20001) - 10000) * (i > 60'000 ? 3 : 1));
    PeakPyramid p;
    p.Build(samples, 8000);
    for (const auto& [first, last] : std::vector<std::pair<int64_t, int64_t>>{ { 0, 10 }, { 5, 37 }, { 1000, 9000 }, { 59'000, 61'000 }, { 0, 100'000 }, { 99'990, 100'000 } })
    {
        const auto [lo, hi] = std::minmax_element(samples.begin() + first, samples.begin() + last);
        const auto [plo, phi] = p.Range(first, last);
        // Coarse buckets may reach a bucket beyond the span: never narrower than the true envelope.
        EXPECT_LE(plo, *lo / 32768.f + 1e-6f) << first;
        EXPECT_GE(phi, *hi / 32767.f - 1e-6f) << first;
        if (last - first < 32)
        {
            EXPECT_FLOAT_EQ(plo, *lo / 32768.f);
            EXPECT_FLOAT_EQ(phi, *hi / 32767.f);
        }
    }
    EXPECT_NEAR(p.Peak(), 30000 / 32767.f, 0.01);
    EXPECT_EQ(p.Span(0, 1'000'000), p.Range(0, 8000));
}

TEST(Timeline, CropBoxes)
{
    EXPECT_EQ(CenteredCrop(1920, 1080, AspectByName("9:16", 1920, 1080)), (PixelRect{ 656, 0, 606, 1080 }));
    EXPECT_EQ(CenteredCrop(1920, 1080, AspectByName("1:1", 1920, 1080)), (PixelRect{ 420, 0, 1080, 1080 }));
    EXPECT_EQ(CenteredCrop(1080, 1920, AspectByName("16:9", 1080, 1920)), (PixelRect{ 0, 656, 1080, 606 }));
    EXPECT_EQ(CenteredCrop(1920, 1080, AspectByName("free", 1920, 1080)), (PixelRect{ 0, 0, 1920, 1080 }));
    EXPECT_EQ(CenteredCrop(1080, 1350, AspectByName("4:5", 1080, 1350)), (PixelRect{ 0, 0, 1080, 1350 }));
    // Odd numbers become even, and the box stays inside.
    EXPECT_EQ(ClampCrop({ -5, 7, 3001, 101 }, 1920, 1080), (PixelRect{ 0, 6, 1920, 100 }));
    // Moving the whole box stops at the edges.
    EXPECT_EQ(DragCrop({ 100, 100, 200, 200 }, 8, 5000, -5000, 1920, 1080, {}), (PixelRect{ 1720, 0, 200, 200 }));
    // A free corner drag changes both sides; with 1:1 the box stays square.
    EXPECT_EQ(DragCrop({ 100, 100, 200, 200 }, 2, 100, 40, 1920, 1080, {}), (PixelRect{ 100, 100, 300, 240 }));
    const auto square = DragCrop({ 100, 100, 200, 200 }, 2, 100, 40, 1920, 1080, { 1, 1 });
    EXPECT_EQ(square.w, square.h);
    EXPECT_EQ(square.w, 300);
    // A square grown past the frame's bottom is limited by it, still square.
    const auto limited = DragCrop({ 800, 800, 200, 200 }, 2, 900, 900, 1920, 1080, { 1, 1 });
    EXPECT_EQ(limited.w, limited.h);
    EXPECT_EQ(limited.y + limited.h, 1080);
    // An edge drag with a fixed shape grows the other side around the centre.
    const auto wide = DragCrop({ 400, 400, 320, 180 }, 5, 160, 0, 1920, 1080, { 16, 9 });
    EXPECT_EQ(wide.w, 480);
    EXPECT_EQ(wide.h, 270);
    EXPECT_EQ(wide.x, 400);
    EXPECT_EQ(RectFromCorners(300, 200, 100, 50, 1920, 1080), (PixelRect{ 100, 50, 200, 150 }));
    int w = 0, h = 0;
    CropOutputSize({ 0, 0, 607, 1080 }, 0, 0, w, h);
    EXPECT_EQ(w, 606);
    EXPECT_EQ(h, 1080);
    CropOutputSize({ 0, 0, 1080, 1080 }, 720, 0, w, h);
    EXPECT_EQ(w, 720);
    EXPECT_EQ(h, 720);
    CropOutputSize({ 0, 0, 607, 1080 }, 1080, 1920, w, h);
    EXPECT_EQ(w, 1080);
    EXPECT_EQ(h, 1920);
}

TEST(Timeline, EditorLayoutMapsAndHitTests)
{
    // A 1920x1080 frame letterboxed into a 1000x1000 box.
    const RectF shown = FitInside(1920, 1080, { 0, 0, 1000, 1000 });
    EXPECT_FLOAT_EQ(shown.w, 1000);
    EXPECT_FLOAT_EQ(shown.h, 562.5f);
    EXPECT_FLOAT_EQ(shown.y, 218.75f);
    const PreviewMap map{ shown, 1920, 1080 };
    EXPECT_EQ(map.ToFrameX(500), 960);
    EXPECT_EQ(map.ToFrameY(shown.y + shown.h), 1080);
    const RectF box = map.ToView({ 192, 108, 960, 540 });
    EXPECT_FLOAT_EQ(box.x, 100);
    EXPECT_FLOAT_EQ(box.w, 500);
    EXPECT_NEAR(map.PixelsPerDip(), 1.92, 1e-5);

    const RectF r{ 100, 100, 200, 100 };
    EXPECT_EQ(HitBoxHandle(r, 101, 99, 6), 0);
    EXPECT_EQ(HitBoxHandle(r, 300, 200, 6), 2);
    EXPECT_EQ(HitBoxHandle(r, 200, 103, 6), 4);
    EXPECT_EQ(HitBoxHandle(r, 97, 150, 6), 7);
    EXPECT_EQ(HitBoxHandle(r, 200, 150, 6), 8);
    EXPECT_EQ(HitBoxHandle(r, 400, 150, 6), -1);

    TimelineView view{ 10'000'000, 1000 };
    view.Fit();   // 10 ms per pixel
    const std::vector<TimeRange> ranges{ { 1'000'000, 2'000'000 }, { 2'050'000, 5'000'000 } };
    EXPECT_EQ(HitRange(view, ranges, 150, 4).part, RangeHit::Part::Body);
    // Between two close edges, the nearer one wins.
    const RangeHit edge = HitRange(view, ranges, 204, 6);
    EXPECT_EQ(edge.index, 1);
    EXPECT_EQ(edge.part, RangeHit::Part::Start);
    EXPECT_EQ(HitRange(view, ranges, 600, 4).index, -1);
    EXPECT_EQ(HitMarker(view, { 1'000'000, 3'000'000 }, 298, 5), 1);
    EXPECT_EQ(HitMarker(view, { 1'000'000 }, 500, 5), -1);
    EXPECT_EQ(FilmstripSlots(1000, 96), 11);
}

TEST(Timeline, PerFileChoicesMergeOverSharedOnes)
{
    const nlohmann::json params = {
        { "mode", "exact" },
        { "start", 0 },
        { "perFile", { { Narrow(L"C:\\clips\\b\u00e9.mp4"), { { "start", 2.5 }, { "end", 4 } } } } },
    };
    const auto mine = ParamsForFile(params, L"C:\\clips\\b\u00e9.mp4");
    EXPECT_EQ(mine.value("start", 0.0), 2.5);
    EXPECT_EQ(mine.value("end", 0.0), 4.0);
    EXPECT_EQ(mine.value("mode", ""), "exact");
    EXPECT_FALSE(mine.contains("perFile"));
    const auto other = ParamsForFile(params, L"C:\\clips\\c.mp4");
    EXPECT_EQ(other.value("start", -1.0), 0.0);
    EXPECT_FALSE(other.contains("perFile"));
    EXPECT_EQ(ParamsForFile({ { "factor", 2 } }, L"x.mp4"), (nlohmann::json{ { "factor", 2 } }));
}

// ---- Probing -------------------------------------------------------------------------------------

TEST(VideoTools, ProbeReadsRotationLayoutAndCover)
{
    const auto phone = ParseProbe({
        "  Duration: 00:00:05.00, start: 0.000000, bitrate: 9000 kb/s",
        "  Stream #0:0[0x1](und): Video: hevc (Main) (hvc1 / 0x31637668), yuv420p(tv, bt709), 1920x1080 [SAR 1:1 DAR 16:9], 29.97 fps, 30 tbr (default)",
        "    Side data:",
        "      Display Matrix: rotation of -90.00 degrees",
        "  Stream #0:1[0x2](und): Audio: aac (LC) (mp4a / 0x6134706D), 44100 Hz, stereo, fltp, 191 kb/s (default)",
    });
    EXPECT_EQ(phone.rotation, 270);
    EXPECT_EQ(phone.width, 1080);   // shown portrait
    EXPECT_EQ(phone.height, 1920);
    EXPECT_EQ(phone.channels, 2);
    EXPECT_EQ(phone.sampleRate, 44100);
    EXPECT_DOUBLE_EQ(phone.sar, 1.0);
    const auto old = ParseProbe({ "  Stream #0:0: Video: h264, yuv420p, 720x576 [SAR 16:15 DAR 4:3], 25 fps", "      displaymatrix: rotation of 180.00 degrees",
                                  "  Stream #0:1: Audio: ac3, 48000 Hz, 5.1(side), fltp, 448 kb/s" });
    EXPECT_EQ(old.rotation, 180);
    EXPECT_EQ(old.width, 720);
    EXPECT_NEAR(old.sar, 16.0 / 15, 1e-9);
    EXPECT_EQ(old.channels, 6);
    const auto song = ParseProbe({ "  Stream #0:0: Audio: mp3, 22050 Hz, mono, fltp, 64 kb/s",
                                   "  Stream #0:1: Video: mjpeg (Baseline), yuvj420p, 600x600, 90k tbr (attached pic)" });
    EXPECT_TRUE(song.coverArt);
    EXPECT_FALSE(song.HasVideo());
    EXPECT_EQ(song.channels, 1);
    EXPECT_EQ(ChannelsOfLayout("3 channels"), 3);
    EXPECT_EQ(ChannelsOfLayout("7.1"), 8);
    EXPECT_EQ(ChannelsOfLayout("fltp"), 0);
}

// ---- Command lines -------------------------------------------------------------------------------

TEST(VideoTools, EveryInputIsLocalOnly)
{
    const auto args = MultiInputArgs({ { L"a.mp4", { L"-hwaccel", L"auto" } }, { L"b.mov", {} }, { L"c.mkv", { L"-ss", L"1" } } }, { L"-f", L"mp4" }, L"out");
    // Each -i is preceded, after the previous -i, by its own "-protocol_whitelist file".
    size_t from = 0;
    int inputs = 0;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] != L"-i")
            continue;
        bool whitelisted = false;
        for (size_t k = from; k + 1 < i; ++k)
            whitelisted = whitelisted || (args[k] == L"-protocol_whitelist" && args[k + 1] == L"file");
        EXPECT_TRUE(whitelisted) << "input " << inputs;
        from = i + 2;
        ++inputs;
    }
    EXPECT_EQ(inputs, 3);
    EXPECT_EQ(args.back(), L"out");
    EXPECT_TRUE(Has(args, L"-progress", L"pipe:1"));
}

TEST(VideoTools, TrimFastCopiesExactEncodes)
{
    const auto info = Clip();
    const auto fast = TrimArgs(L"in.mov", L".mov", info, { 2'000'000, 5'500'000 }, true, H264Encoder::Nvenc, L"out");
    EXPECT_TRUE(Has(fast, L"-ss", L"2.000000"));
    EXPECT_TRUE(Has(fast, L"-to", L"5.500000"));
    EXPECT_TRUE(Has(fast, L"-c", L"copy"));
    EXPECT_TRUE(Has(fast, L"-f", L"mov"));            // same container as the source
    EXPECT_FALSE(Has(fast, L"-hwaccel", L"auto"));    // nothing is decoded
    // -ss comes before -i: a seek, not a decode from the start.
    EXPECT_LT(std::find(fast.begin(), fast.end(), L"-ss"), std::find(fast.begin(), fast.end(), L"-i"));
    const auto exact = TrimArgs(L"in.webm", L".webm", info, { 2'000'000, 5'500'000 }, false, H264Encoder::Nvenc, L"out");
    EXPECT_TRUE(Has(exact, L"-c:v", L"h264_nvenc"));
    EXPECT_TRUE(Has(exact, L"-c:a", L"copy"));         // AAC goes into MP4 as it is
    EXPECT_TRUE(Has(exact, L"-f", L"mp4"));            // WebM is encoded into MP4
    EXPECT_TRUE(Has(exact, L"-hwaccel", L"auto"));
    EXPECT_EQ(TrimContainer(L".webm", true).extension, L".webm");
    EXPECT_EQ(TrimContainer(L".wmv", true).muxer, L"matroska");   // copy into a container that takes anything
    EXPECT_EQ(TrimContainer(L".mkv", false).extension, L".mkv");
    // A silent clip has no audio options at all.
    const auto silent = TrimArgs(L"in.mp4", L".mp4", Clip(640, 480, 25, "h264", ""), { 0, 1'000'000 }, false, H264Encoder::OpenH264, L"out");
    EXPECT_NE(std::find(silent.begin(), silent.end(), L"-an"), silent.end());
}

TEST(VideoTools, CropScalesToTheExactSize)
{
    EXPECT_EQ(CropFilter({ 10, 20, 300, 200 }, 300, 200), L"crop=300:200:10:20");
    EXPECT_EQ(CropFilter({ 656, 0, 606, 1080 }, 1080, 1920), L"crop=606:1080:656:0,scale=1080:1920:flags=lanczos,setsar=1");
    const auto args = CropArgs(L"in.mp4", L".mp4", Clip(), { 656, 0, 606, 1080 }, 1080, 1920, H264Encoder::Qsv, L"out");
    EXPECT_TRUE(Contains(args, L"crop=606:1080:656:0,scale=1080:1920"));
    EXPECT_TRUE(Has(args, L"-c:a", L"copy"));   // the original audio, untouched
    EXPECT_TRUE(Has(args, L"-c:v", L"h264_qsv"));
}

TEST(VideoTools, SpeedKeepsPitchAndFrameRate)
{
    EXPECT_EQ(AtempoChain(2), L"atempo=2");
    EXPECT_EQ(AtempoChain(4), L"atempo=2,atempo=2");
    EXPECT_EQ(AtempoChain(0.25), L"atempo=0.5,atempo=0.5");
    EXPECT_EQ(AtempoChain(3), L"atempo=2,atempo=1.5");
    EXPECT_EQ(AtempoChain(0.3), L"atempo=0.5,atempo=0.6");
    EXPECT_EQ(AtempoChain(1.25), L"atempo=1.25");
    EXPECT_EQ(AtempoChain(10), L"atempo=2,atempo=2");   // clamped to 4x
    EXPECT_EQ(SpeedLabel(0.5), L"0.5x");
    const auto args = SpeedArgs(L"in.mp4", L".mp4", Clip(1280, 720, 30000.0 / 1001), 2, H264Encoder::Amf, L"out");
    EXPECT_EQ(After(args, L"-vf"), L"setpts=PTS/2,fps=30000/1001,crop=trunc(iw/2)*2:trunc(ih/2)*2");
    EXPECT_EQ(After(args, L"-af"), L"atempo=2");
    EXPECT_TRUE(Has(args, L"-c:a", L"aac"));   // re-timed audio is encoded
    // A silent video: no audio filter, no audio.
    const auto silent = SpeedArgs(L"in.mp4", L".mp4", Clip(1280, 720, 25, "h264", ""), 0.5, H264Encoder::Amf, L"out");
    EXPECT_TRUE(After(silent, L"-af").empty());
    EXPECT_NE(std::find(silent.begin(), silent.end(), L"-an"), silent.end());
    EXPECT_EQ(FpsExpr(25), L"25");
    EXPECT_EQ(FpsExpr(23.976), L"24000/1001");
    EXPECT_EQ(FpsExpr(59.94), L"60000/1001");
}

TEST(VideoTools, JoinCopiesMatchingClipsAndEncodesTheRest)
{
    const auto a = Clip(), b = Clip();
    EXPECT_TRUE(CanJoinByCopy({ a, b }));
    EXPECT_FALSE(CanJoinByCopy({ a, Clip(1280, 720) }));                 // different size
    EXPECT_FALSE(CanJoinByCopy({ a, Clip(1920, 1080, 25) }));            // different rate
    EXPECT_FALSE(CanJoinByCopy({ a, Clip(1920, 1080, 30, "h264", "") })); // one is silent
    EXPECT_FALSE(CanJoinByCopy({ Clip(1920, 1080, 30, "vp9", "opus"), Clip(1920, 1080, 30, "vp9", "opus") }));   // MP4 out
    auto turned = Clip();
    turned.rotation = 90;
    EXPECT_FALSE(CanJoinByCopy({ a, turned }));

    // Quotes in a path close the quoted string: written as '\''.
    EXPECT_EQ(EscapeConcatPath(R"(C:\clips\it's here.mp4)"), R"('C:\clips\it'\''s here.mp4')");
    EXPECT_EQ(ConcatList({ L"C:\\a.mp4", L"D:\\b c.mov" }), "ffconcat version 1.0\nfile 'C:\\a.mp4'\nfile 'D:\\b c.mov'\n");
    const auto copy = JoinCopyArgs(L"list.ffconcat", a, L"out");
    EXPECT_TRUE(Has(copy, L"-f", L"concat"));
    EXPECT_TRUE(Has(copy, L"-safe", L"0"));
    EXPECT_TRUE(Has(copy, L"-c", L"copy"));
    EXPECT_TRUE(Has(copy, L"-protocol_whitelist", L"file"));

    // The first clip sets the canvas; a silent clip gets silence of its own length.
    const auto first = Clip(1080, 1920, 30, "h264", "aac", 5'000'000);
    const auto quiet = Clip(1920, 1080, 25, "mpeg4", "", 3'000'000);
    const auto enc = JoinEncodeArgs({ L"a.mp4", L"b.avi" }, { first, quiet }, H264Encoder::Nvenc, L"out");
    const auto graph = After(enc, L"-filter_complex");
    EXPECT_NE(graph.find(L"[0:v:0]scale=1080:1920:force_original_aspect_ratio=decrease"), std::wstring::npos) << Narrow(graph);
    EXPECT_NE(graph.find(L"[1:v:0]scale=1080:1920"), std::wstring::npos);
    EXPECT_NE(graph.find(L"pad=1080:1920:(ow-iw)/2:(oh-ih)/2"), std::wstring::npos);
    EXPECT_NE(graph.find(L"anullsrc=channel_layout=stereo:sample_rate=48000,atrim=duration=3.000000[a1]"), std::wstring::npos);
    EXPECT_NE(graph.find(L"[v0][a0][v1][a1]concat=n=2:v=1:a=1[v][a]"), std::wstring::npos);
    EXPECT_TRUE(Has(enc, L"-map", L"[a]"));
    EXPECT_EQ(std::count(enc.begin(), enc.end(), L"-i"), 2);
    // Fill: other shapes are scaled up and cropped to the canvas, no black bars.
    const auto filled = After(JoinEncodeArgs({ L"a.mp4", L"b.avi" }, { first, quiet }, H264Encoder::Nvenc, L"out", true), L"-filter_complex");
    EXPECT_NE(filled.find(L"[1:v:0]scale=1080:1920:force_original_aspect_ratio=increase,crop=1080:1920"), std::wstring::npos) << Narrow(filled);
    EXPECT_EQ(filled.find(L"pad="), std::wstring::npos);
    // Nobody has sound: no audio at all.
    const auto mute = JoinEncodeArgs({ L"a.mp4", L"b.mp4" }, { Clip(640, 480, 30, "h264", ""), Clip(640, 480, 30, "h264", "") }, H264Encoder::OpenH264, L"out");
    EXPECT_NE(After(mute, L"-filter_complex").find(L"concat=n=2:v=1:a=0[v]"), std::wstring::npos);
    EXPECT_FALSE(Has(mute, L"-map", L"[a]"));
}

TEST(VideoTools, FramesAndParts)
{
    const auto png = FrameToPngArgs(L"in.mp4", 1'000'000, 25, L"C:\\out\\100% clip 00-00-01.000.png");
    EXPECT_TRUE(Has(png, L"-ss", L"0.990000"));   // a quarter frame early: this exact frame, never the next
    EXPECT_TRUE(Has(png, L"-frames:v", L"1"));
    EXPECT_TRUE(Has(png, L"-update", L"1"));      // the name is not an image-sequence pattern
    EXPECT_TRUE(Has(png, L"-c:v", L"png"));
    EXPECT_EQ(FramePngName(L"clip", 62'345'000), L"clip 00-01-02.345.png");
    EXPECT_EQ(NormalizeFrameTimes({ 1'010'000, 1'000'000, 99'000'000, 0 }, 25, 2'000'000), (std::vector<int64_t>{ 0, 1'000'000, 1'960'000 }));

    EXPECT_EQ(PartName(L"clip", 2, 3, L".mp4"), L"clip part 2.mp4");
    EXPECT_EQ(PartName(L"clip", 2, 12, L".mp4"), L"clip part 02.mp4");
    const auto copy = SplitCopyArgs(L"in.mp4", L".mp4", { 2'000'000, 4'500'000 }, L"C:\\out", L"50% off");
    EXPECT_TRUE(Has(copy, L"-f", L"segment"));
    EXPECT_TRUE(Has(copy, L"-segment_times", L"2.000000,4.500000"));
    EXPECT_TRUE(Has(copy, L"-segment_format", L"mp4"));
    EXPECT_TRUE(Has(copy, L"-c", L"copy"));
    EXPECT_EQ(copy.back(), L"C:\\out\\50%% off part %01d.mp4");   // the user's % doubled, ours a field
    const auto part = SplitPartArgs(L"in.mp4", L".mp4", Clip(), { 2'000'000, 4'500'000 }, H264Encoder::Nvenc, L"out");
    EXPECT_TRUE(Has(part, L"-ss", L"2.000000"));
    EXPECT_TRUE(Has(part, L"-c:v", L"h264_nvenc"));
}

TEST(VideoTools, RedactBlursOrCoversEachBoxInItsTime)
{
    EXPECT_EQ(BetweenExpr({ 1'500'000, 3'000'000 }), L"between(t,1.500000,3.000000)");
    const auto graph = RedactGraph({ { { 101, 51, 200, 100 }, { 1'000'000, 2'000'000 }, true }, { { 0, 0, 64, 64 }, { 0, 5'000'000 }, false } }, 1920, 1080);
    EXPECT_NE(graph.find(L"[0:v:0]split=2[r0a][r0b];[r0b]crop=200:100:100:50,gblur=sigma="), std::wstring::npos) << Narrow(graph);
    EXPECT_NE(graph.find(L"[r0a][r0c]overlay=x=100:y=50:enable='between(t,1.000000,2.000000)'[r0]"), std::wstring::npos);
    EXPECT_NE(graph.find(L"[r0]drawbox=x=0:y=0:w=64:h=64:color=black@1:t=fill:enable='between(t,0.000000,5.000000)'[r1]"), std::wstring::npos);
    EXPECT_TRUE(graph.ends_with(L"[r1]crop=trunc(iw/2)*2:trunc(ih/2)*2[vout]"));
    const auto args = RedactArgs(L"in.mov", L".mov", Clip(), { { { 0, 0, 64, 64 }, { 0, 5'000'000 }, false } }, H264Encoder::OpenH264, L"out");
    EXPECT_TRUE(Has(args, L"-map", L"[vout]"));
    EXPECT_TRUE(Has(args, L"-c:a", L"copy"));
    EXPECT_TRUE(Has(args, L"-f", L"mov"));
}

TEST(VideoTools, ContainersAndSoundtracks)
{
    EXPECT_EQ(EncodeContainer(L".mov").muxer, L"mov");
    EXPECT_EQ(EncodeContainer(L".avi").muxer, L"mp4");
    EXPECT_EQ(CopyContainer(L".m4v").extension, L".mp4");
    EXPECT_EQ(CopyContainer(L".mts").muxer, L"mpegts");
    EXPECT_TRUE(ContainerTakesAudio({ L"mp4", L".mp4" }, "aac"));
    EXPECT_FALSE(ContainerTakesAudio({ L"mp4", L".mp4" }, "pcm_s16le"));
    EXPECT_TRUE(ContainerTakesAudio({ L"mov", L".mov" }, "pcm_s16le"));
    EXPECT_TRUE(ContainerTakesAudio({ L"matroska", L".mkv" }, "flac"));
    auto pcm = Clip();
    pcm.media.audioCodec = "pcm_s16le";
    EXPECT_TRUE(Has(SoundtrackArgs({ L"mp4", L".mp4" }, pcm), L"-c:a", L"aac"));
    EXPECT_TRUE(Has(EditVideoArgs(H264Encoder::OpenH264, 1920, 1080, 30), L"-b:v", L"7464960"));   // 0.12 bits per pixel per frame
    EXPECT_TRUE(Has(EditVideoArgs(H264Encoder::Nvenc, 1920, 1080, 30), L"-cq", L"20"));
    EXPECT_TRUE(DecodeArgs(H264Encoder::MediaFoundation).empty());
}

TEST(VideoTools, EditorFrameExtraction)
{
    auto info = Clip(1920, 1080);
    int w = 0, h = 0;
    FitFrame(info, 320, 320, w, h);
    EXPECT_EQ(w, 320);
    EXPECT_EQ(h, 180);
    info.width = 720, info.height = 576, info.sar = 16.0 / 15;   // anamorphic PAL: shown 768 wide
    FitFrame(info, 2000, 2000, w, h);
    EXPECT_EQ(w, 768);
    EXPECT_EQ(h, 576);
    const auto fast = FrameArgs(L"in.mp4", 5'000'000, 25, 160, 90, true, L"out");
    EXPECT_TRUE(Has(fast, L"-skip_frame", L"nokey"));
    EXPECT_TRUE(Has(fast, L"-f", L"rawvideo"));
    EXPECT_TRUE(Has(fast, L"-pix_fmt", L"bgra"));
    EXPECT_TRUE(Has(fast, L"-vf", L"scale=160:90:flags=bilinear"));
    EXPECT_TRUE(Has(fast, L"-fps_mode", L"passthrough"));   // a keyframe before the seek point still comes out
    const auto exact = FrameArgs(L"in.mp4", 5'000'000, 25, 160, 90, false, L"out");
    EXPECT_TRUE(Has(exact, L"-ss", L"4.990000"));
    EXPECT_EQ(WaveformRate(60'000'000), 8000);
    EXPECT_EQ(WaveformRate(3600LL * 1'000'000), 2200);
    EXPECT_EQ(WaveformRate(100LL * 3600 * 1'000'000), 200);
}

// ---- The module ----------------------------------------------------------------------------------

TEST(VideoTools, SlicesOnTheToolsWheelForVideosOnly)
{
    TempDir dir;
    Recipes recipes(dir);
    const PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr, nullptr);
    int forVideo = 0;
    for (const auto& r : recipes.store.All())
    {
        if (r.ring != "tools")
            continue;
        const bool video = runner.Applies(r, L"C:\\v\\clip.mov");
        forVideo += video;
        if (r.id.starts_with("video-"))
        {
            EXPECT_TRUE(video) << r.id;
            EXPECT_FALSE(runner.Applies(r, L"C:\\v\\song.mp3")) << r.id;
            EXPECT_FALSE(runner.Applies(r, L"C:\\v\\photo.jpg")) << r.id;
            EXPECT_EQ(runner.Lane(r), JobLane::Media) << r.id;
            EXPECT_NE(r.icon, 0) << r.id;
        }
    }
    // Two rings hold sixteen slices; past that the wheel pages. Every video tool has a slice.
    EXPECT_LE(forVideo, 16);
    for (const char* id : { "video-trim", "video-crop", "video-speed", "video-join", "video-frames", "video-split", "video-redact" })
        ASSERT_NE(recipes.store.Find(id), nullptr) << id;
    // Every tool is in the editor's tool bar, with a recipe of its own for history and output names.
    EXPECT_EQ(SiblingTools(EditorTool::JoinVideos).size(), 7u);
    for (const auto tool : SiblingTools(EditorTool::JoinVideos))
    {
        const auto j = ToolRecipe(tool);
        ASSERT_TRUE(j.is_object()) << ToolAction(tool);
        EXPECT_EQ(j["steps"][0]["action"], ToolAction(tool));
        EXPECT_FALSE(j.contains("wheel"));
        EXPECT_TRUE(IsVideoTool(tool));
        Recipe parsed;
        std::string error;
        EXPECT_TRUE(ParseRecipe(j.dump(), recipes.registry, parsed, error)) << error;
        EditorTool back{};
        EXPECT_TRUE(ToolForAction(ToolAction(tool), back));
        EXPECT_EQ(back, tool);
    }
    EXPECT_FALSE(ToolRecipe(EditorTool::TrimAudio).is_object());
    const auto* engine = runner.EngineFor(recipes.Get("video-trim"));
    ASSERT_NE(engine, nullptr);
    EXPECT_TRUE(engine->NeedsEditor(recipes.Get("video-trim")));
    Recipe twice = ToolById(recipes, "video-speed");
    twice.steps[0].params["ask"] = false;
    EXPECT_FALSE(engine->NeedsEditor(twice));   // a one-click preset
    EXPECT_TRUE(engine->IsBatch(recipes.Get("video-join")));
    EXPECT_FALSE(engine->IsBatch(recipes.Get("video-split")));
    std::wstring why;
    EXPECT_FALSE(runner.Accepts(recipes.Get("video-trim"), L"C:\\v\\clip.mov", &why));
    EXPECT_NE(why.find(L"media engine"), std::wstring::npos);
    EXPECT_TRUE(IsPerFileTool(EditorTool::TrimVideo));
    EXPECT_FALSE(IsPerFileTool(EditorTool::JoinVideos));
    // Without a window linked in (tests) opening the editor does nothing and says so.
    bool called = false;
    EXPECT_FALSE(OpenEditor(nullptr, EditorRequest{}, [&](nlohmann::json, std::vector<std::wstring>) { called = true; }));
    EXPECT_FALSE(called);
}

namespace
{
    // Stands in for the editor window: the user switches to Speed in the tool bar, picks 3x, confirms.
    void FakeEditorPicksSpeed(HWND, EditorRequest request, EditorDone done)
    {
        EXPECT_EQ(request.tool, EditorTool::TrimVideo);
        done(nlohmann::json{ { "tool", "video.speed" }, { "factor", 3 } }, request.files);
    }

    void FakeEditorKeepsTrim(HWND, EditorRequest request, EditorDone done)
    {
        done(nlohmann::json{ { "tool", "video.trim" }, { "start", 1 } }, request.files);
    }
}

TEST(VideoTools, ToolBarSwitchRunsTheChosenTool)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto engine = MediaToolsModule().makeEngine(EngineServices{ nullptr, nullptr });
    Recipe ran;
    std::vector<std::wstring> files;
    const auto done = [&](Recipe r, std::vector<std::wstring> f) {
        ran = std::move(r);
        files = std::move(f);
    };
    SetEditorOpener(&FakeEditorPicksSpeed);
    engine->OpenEditor(nullptr, recipes.Get("video-trim"), { L"C:\\v\\a.mp4" }, done);
    EXPECT_EQ(ran.id, "video-speed");
    EXPECT_EQ(ran.steps[0].action, "video.speed");
    EXPECT_EQ(ran.suffix, L"speed");
    EXPECT_EQ(ran.description.substr(0, 18), L"Change Video Speed");
    EXPECT_EQ(static_cast<int>(ran.icon), 0xEC49);
    EXPECT_EQ(ran.steps[0].params.value("factor", 0.0), 3.0);
    EXPECT_FALSE(ran.steps[0].params.value("ask", true));   // the editor was the asking
    EXPECT_FALSE(ran.steps[0].params.contains("tool"));
    EXPECT_EQ(files, std::vector<std::wstring>{ L"C:\\v\\a.mp4" });
    // Confirming the tool it opened with keeps the recipe as it was.
    SetEditorOpener(&FakeEditorKeepsTrim);
    engine->OpenEditor(nullptr, recipes.Get("video-trim"), { L"C:\\v\\a.mp4" }, done);
    SetEditorOpener(nullptr);
    EXPECT_EQ(ran.id, "video-trim");
    EXPECT_EQ(ran.suffix, L"trimmed");
    EXPECT_EQ(ran.steps[0].params.value("start", 0.0), 1.0);
}

// ---- With the real ffmpeg.exe (SHIFTDROP_FFMPEG) --------------------------------------------------

namespace
{
    std::shared_ptr<const FfmpegEngine> Ffmpeg()
    {
        static const auto engine = std::make_shared<const FfmpegEngine>(FfmpegEngine::Locate());
        return engine;
    }

    bool Make(const std::vector<std::wstring>& args)
    {
        std::stop_source never;
        std::vector<std::wstring> a{ L"-hide_banner", L"-nostdin", L"-y" };
        a.insert(a.end(), args.begin(), args.end());
        return Ffmpeg()->Run(a, nullptr, never.get_token()).ok;
    }

    // 4 s of 320x240 test pattern at 25 fps with a tone, H.264 (OpenH264) + AAC in MP4.
    bool MakeH264(const std::wstring& path, const wchar_t* size = L"320x240", double seconds = 4)
    {
        const auto d = std::format(L"{}", seconds);
        return Make({ L"-f", L"lavfi", L"-i", std::format(L"testsrc=s={}:r=25:d={}", size, d), L"-f", L"lavfi", L"-i", std::format(L"sine=f=440:d={}", d),
                      L"-c:v", L"libopenh264", L"-b:v", L"2M", L"-g", L"25", L"-c:a", L"aac", L"-shortest", L"-f", L"mp4", path });
    }

    // Frame n is grey level 8n: tells exactly which frame was taken.
    bool MakeNumbered(const std::wstring& path)
    {
        return Make({ L"-f", L"lavfi", L"-i", L"color=c=black:s=64x64:r=10:d=3,geq=lum='N*8':cb=128:cr=128", L"-c:v", L"mpeg4", L"-q:v", L"1",
                      L"-f", L"avi", path });
    }

    struct Fixture
    {
        TempDir dir;
        Recipes recipes{ dir };
        std::unique_ptr<HistoryStore> history;
        std::unique_ptr<IEngine> engine;
        Fixture()
        {
            std::wstring error;
            history = HistoryStore::Open(dir / L"history.db", error);
            engine = MediaToolsModule().makeEngine(EngineServices{ history.get(), Ffmpeg() });
        }
        ItemResult Run(std::string_view id, nlohmann::json params, const std::wstring& source, double* lastProgress = nullptr)
        {
            Recipe r = ToolById(recipes, id);
            for (auto& [k, v] : params.items())
                r.steps[0].params[k] = v;
            const auto run = history->BeginRun(r, 1);
            double last = 0;
            auto out = engine->Run(r, run, source, std::stop_token{}, [&](double p) { last = std::max(last, p); });
            history->FinishRun(run);
            if (lastProgress)
                *lastProgress = last;
            return out;
        }
    };

    double Seconds(const ProbeInfo& p) { return static_cast<double>(p.DurationUs()) / 1e6; }
}

TEST(VideoToolsEngine, TrimExactAndFast)
{
    if (!Ffmpeg()->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    Fixture f;
    const auto clip = f.dir / L"my clip.mp4";
    ASSERT_TRUE(MakeH264(clip));
    double progress = 0;
    const auto exact = f.Run("video-trim", { { "start", 1.0 }, { "end", 3.0 }, { "mode", "exact" } }, clip, &progress);
    ASSERT_TRUE(exact.ok) << Narrow(exact.error);
    EXPECT_EQ(std::filesystem::path(exact.output).filename(), L"my clip (trimmed).mp4");
    const auto e = Probe(*Ffmpeg(), exact.output);
    EXPECT_NEAR(Seconds(e), 2.0, 0.1);
    EXPECT_TRUE(e.HasAudio());
    EXPECT_GT(progress, 0.5);
    const auto fast = f.Run("video-trim", { { "start", 1.0 }, { "end", 3.0 }, { "mode", "fast" } }, clip);
    ASSERT_TRUE(fast.ok) << Narrow(fast.error);
    EXPECT_EQ(std::filesystem::path(fast.output).filename(), L"my clip (trimmed 2).mp4");   // never overwrites
    const auto c = Probe(*Ffmpeg(), fast.output);
    EXPECT_EQ(c.media.videoCodec, "h264");
    EXPECT_GE(Seconds(c), 1.9);   // from the keyframe at 1 s (one every 25 frames)
    EXPECT_LE(Seconds(c), 2.2);
    // Keeping everything is refused, and the refusal is in History.
    const auto none = f.Run("video-trim", { { "start", 0.0 }, { "end", 0.0 } }, clip);
    EXPECT_FALSE(none.ok);
    EXPECT_EQ(CountFiles(f.dir, L".sdpart"), 0u);
}

TEST(VideoToolsEngine, CropSpeedAndRedact)
{
    if (!Ffmpeg()->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    Fixture f;
    const auto clip = f.dir / L"clip.mp4";
    ASSERT_TRUE(MakeH264(clip));
    const auto crop = f.Run("video-crop", { { "box", { 10, 20, 100, 100 } }, { "width", 200 }, { "height", 200 } }, clip);
    ASSERT_TRUE(crop.ok) << Narrow(crop.error);
    const auto c = Probe(*Ffmpeg(), crop.output);
    EXPECT_EQ(c.width, 200);
    EXPECT_EQ(c.height, 200);
    EXPECT_TRUE(c.HasAudio());
    EXPECT_NEAR(Seconds(c), 4.0, 0.15);
    // A one-click centred crop from the aspect alone.
    const auto square = f.Run("video-crop", { { "aspect", "1:1" }, { "ask", false } }, clip);
    ASSERT_TRUE(square.ok) << Narrow(square.error);
    EXPECT_EQ(Probe(*Ffmpeg(), square.output).width, 240);

    const auto fast = f.Run("video-speed", { { "factor", 2 }, { "ask", false } }, clip);
    ASSERT_TRUE(fast.ok) << Narrow(fast.error);
    EXPECT_EQ(std::filesystem::path(fast.output).filename(), L"clip (2x).mp4");
    const auto s = Probe(*Ffmpeg(), fast.output);
    EXPECT_NEAR(Seconds(s), 2.0, 0.15);
    EXPECT_NEAR(s.media.fps, 25, 0.5);   // the source's rate, not 50
    EXPECT_TRUE(s.HasAudio());
    // Slowing a silent clip down works too.
    const auto silent = f.dir / L"silent.avi";
    ASSERT_TRUE(Make({ L"-f", L"lavfi", L"-i", L"testsrc=s=160x120:r=25:d=2", L"-c:v", L"mpeg4", L"-f", L"avi", silent }));
    const auto slow = f.Run("video-speed", { { "factor", 0.5 } }, silent);
    ASSERT_TRUE(slow.ok) << Narrow(slow.error);
    EXPECT_NEAR(Seconds(Probe(*Ffmpeg(), slow.output)), 4.0, 0.2);
    EXPECT_FALSE(Probe(*Ffmpeg(), slow.output).HasAudio());

    // Redact: a solid box is black inside its time range and the picture outside it is untouched.
    nlohmann::json boxes = nlohmann::json::array();
    boxes.push_back({ { "x", 0 }, { "y", 0 }, { "w", 80 }, { "h", 80 }, { "start", 1.0 }, { "end", 2.0 }, { "style", "solid" } });
    boxes.push_back({ { "x", 160 }, { "y", 120 }, { "w", 80 }, { "h", 60 }, { "start", 0.0 }, { "end", 4.0 }, { "style", "blur" } });
    const auto redacted = f.Run("video-redact", { { "boxes", boxes } }, clip);
    ASSERT_TRUE(redacted.ok) << Narrow(redacted.error);
    TempFolder temp;
    const auto info = Probe(*Ffmpeg(), redacted.output);
    Frame inside, outside;
    ASSERT_TRUE(ExtractFrame(*Ffmpeg(), redacted.output, info, 1'500'000, 320, 240, false, temp, {}, inside));
    ASSERT_TRUE(ExtractFrame(*Ffmpeg(), redacted.output, info, 3'000'000, 320, 240, false, temp, {}, outside));
    ASSERT_EQ(inside.w, 320);
    const auto luma = [](uint32_t p) { return ((p >> 16 & 0xFF) + (p >> 8 & 0xFF) + (p & 0xFF)) / 3; };
    EXPECT_LT(luma(inside.bgra[40 * 320 + 40]), 24u);
    EXPECT_GT(luma(outside.bgra[40 * 320 + 40]), 24u);   // testsrc is bright there
    EXPECT_EQ(CountFiles(f.dir, L".sdpart"), 0u);
}

TEST(VideoToolsEngine, JoinCopiesOrEncodes)
{
    if (!Ffmpeg()->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    Fixture f;
    const auto a = f.dir / L"a.mp4", b = f.dir / L"b's clip.mp4";   // a quote in the name: the concat list escapes it
    ASSERT_TRUE(MakeH264(a, L"320x240", 2));
    ASSERT_TRUE(MakeH264(b, L"320x240", 3));
    auto join = f.recipes.Get("video-join");
    double progress = 0;
    const auto copied = f.engine->RunBatch(join, 0, { a, b }, {}, [&](double p) { progress = std::max(progress, p); });
    ASSERT_TRUE(copied.ok) << Narrow(copied.error);
    EXPECT_EQ(std::filesystem::path(copied.output).filename(), L"a (joined).mp4");
    const auto j = Probe(*Ffmpeg(), copied.output);
    EXPECT_NEAR(Seconds(j), 5.0, 0.2);
    EXPECT_EQ(j.media.videoCodec, "h264");
    EXPECT_GT(progress, 0.5);
    // A different size and no sound: encoded onto the first clip's canvas, with silence for it.
    const auto c = f.dir / L"c.avi";
    ASSERT_TRUE(Make({ L"-f", L"lavfi", L"-i", L"testsrc=s=200x300:r=30:d=2", L"-c:v", L"mpeg4", L"-f", L"avi", c }));
    const auto encoded = f.engine->RunBatch(join, 0, { a, c, b }, {}, nullptr);
    ASSERT_TRUE(encoded.ok) << Narrow(encoded.error);
    const auto e = Probe(*Ffmpeg(), encoded.output);
    EXPECT_EQ(e.width, 320);
    EXPECT_EQ(e.height, 240);
    EXPECT_NEAR(Seconds(e), 7.0, 0.3);
    EXPECT_TRUE(e.HasAudio());
    EXPECT_FALSE(f.engine->RunBatch(join, 0, { a }, {}, nullptr).ok);   // one clip is not a join
}

TEST(VideoToolsEngine, SavesExactFramesAndSplits)
{
    if (!Ffmpeg()->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    Fixture f;
    const auto numbered = f.dir / L"numbered.avi";
    ASSERT_TRUE(MakeNumbered(numbered));
    const auto frames = f.Run("video-frames", { { "frames", { 1.2, 0.0, 2.5 } } }, numbered);
    ASSERT_TRUE(frames.ok) << Narrow(frames.error);
    EXPECT_EQ(std::filesystem::path(frames.output).filename(), L"numbered (frames)");
    std::vector<std::wstring> names;
    for (const auto& e : std::filesystem::directory_iterator(frames.output))
        names.push_back(e.path().filename().wstring());
    std::sort(names.begin(), names.end());
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[1], L"numbered 00-00-01.200.png");
    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(frames.output + L"\\" + names[1], w, h));
    EXPECT_EQ(w, 64u);   // full size
    // Frame 12 (1.2 s at 10 fps) is grey 96: exactly that frame, not its neighbour (88 or 104).
    TempFolder temp;
    const auto info = Probe(*Ffmpeg(), numbered);
    Frame frame;
    ASSERT_TRUE(ExtractFrame(*Ffmpeg(), numbered, info, 1'200'000, 64, 64, false, temp, {}, frame));
    const int grey = static_cast<int>(frame.bgra[32 * 64 + 32] >> 8 & 0xFF);
    EXPECT_NEAR(grey, 96 * 255 / 219 - 16 * 255 / 219, 4) << "the preview shows frame 12";

    const auto clip = f.dir / L"long.mp4";
    ASSERT_TRUE(MakeH264(clip));
    const auto exact = f.Run("video-split", { { "parts", 2 }, { "mode", "exact" } }, clip);
    ASSERT_TRUE(exact.ok) << Narrow(exact.error);
    EXPECT_EQ(std::filesystem::path(exact.output).filename(), L"long (parts)");
    const auto part2 = exact.output + L"\\long part 2.mp4";
    ASSERT_NE(GetFileAttributesW(part2.c_str()), INVALID_FILE_ATTRIBUTES);
    EXPECT_NEAR(Seconds(Probe(*Ffmpeg(), part2)), 2.0, 0.1);
    const auto fast = f.Run("video-split", { { "points", { 1.0, 3.0 } }, { "mode", "fast" } }, clip);
    ASSERT_TRUE(fast.ok) << Narrow(fast.error);
    size_t parts = 0;
    for (const auto& e : std::filesystem::directory_iterator(fast.output))
        parts += e.path().extension() == L".mp4";
    EXPECT_EQ(parts, 3u);
    EXPECT_EQ(CountFiles(f.dir, L".sdpart"), 0u);
}

TEST(VideoToolsEngine, EditorExtractsFramesAndWaveform)
{
    if (!Ffmpeg()->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    TempDir dir;
    const auto clip = dir / L"clip.mp4";
    ASSERT_TRUE(MakeH264(clip, L"320x240", 2));
    const auto info = Probe(*Ffmpeg(), clip);
    TempFolder temp;
    ASSERT_TRUE(temp.Ok());
    Frame thumb;
    ASSERT_TRUE(ExtractFrame(*Ffmpeg(), clip, info, 1'000'000, 96, 96, true, temp, {}, thumb));
    EXPECT_EQ(thumb.w, 96);
    EXPECT_EQ(thumb.h, 72);
    std::vector<int16_t> pcm;
    ASSERT_TRUE(ExtractMonoPcm(*Ffmpeg(), clip, 8000, temp, {}, pcm));
    EXPECT_NEAR(static_cast<double>(pcm.size()), 16000, 800);
    PeakPyramid peaks;
    peaks.Build(std::move(pcm), 8000);
    EXPECT_GT(peaks.Peak(), 0.05f);   // the tone is there
    // The temp folder goes away with everything in it.
    std::wstring gonePath;
    {
        TempFolder gone;
        ASSERT_TRUE(gone.Ok());
        gonePath = gone.Path();
        test::WriteText(gone.File(L"x.raw"), "x");
    }
    EXPECT_EQ(GetFileAttributesW(gonePath.c_str()), INVALID_FILE_ATTRIBUTES);
}
