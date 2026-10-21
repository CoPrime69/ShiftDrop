#include "Ask.h"
#include "Ffmpeg.h"
#include "MediaTestUtil.h"
#include "PathUtil.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "audiotools/AudioEngine.h"
#include "audiotools/AudioTools.h"
#include "mediaeditor/MediaProbe.h"
#include "mediaeditor/MediaTools.h"
#include "mediaeditor/MediaToolsModule.h"

#include <algorithm>
#include <cmath>

using namespace sd;
using namespace sd::mediatools;
using namespace sd::audiotools;
using namespace sd::test;

namespace
{
    ProbeInfo Song(const char* codec = "mp3", int channels = 2, int rate = 44100, int64_t us = 180'000'000)
    {
        ProbeInfo p;
        p.media.audioCodec = codec;
        p.media.durationUs = us;
        p.channels = channels;
        p.sampleRate = rate;
        return p;
    }

    // What loudnorm prints at the end of a run (ffmpeg 8).
    const std::vector<std::string> kLoudnormLog = {
        "  Stream #0:0: Audio: pcm_s16le, 44100 Hz, stereo, s16, 1411 kb/s",
        "[Parsed_loudnorm_0 @ 000001d2c5f0e880] ",
        "{",
        "\t\"input_i\" : \"-27.61\",",
        "\t\"input_tp\" : \"-4.47\",",
        "\t\"input_lra\" : \"18.06\",",
        "\t\"input_thresh\" : \"-39.20\",",
        "\t\"output_i\" : \"-16.58\",",
        "\t\"output_tp\" : \"-1.50\",",
        "\t\"output_lra\" : \"14.78\",",
        "\t\"output_thresh\" : \"-27.71\",",
        "\t\"normalization_type\" : \"dynamic\",",
        "\t\"target_offset\" : \"0.58\"",
        "}",
        "[out#0/null @ 000001d2c5f0d540] video:0KiB audio:0KiB",
    };
}

// ---- Normalize -----------------------------------------------------------------------------------

TEST(AudioTools, ParsesLoudnormJson)
{
    const auto l = ParseLoudnorm(kLoudnormLog);
    ASSERT_TRUE(l.has_value());
    EXPECT_DOUBLE_EQ(l->inputI, -27.61);
    EXPECT_DOUBLE_EQ(l->inputTP, -4.47);
    EXPECT_DOUBLE_EQ(l->inputLRA, 18.06);
    EXPECT_DOUBLE_EQ(l->inputThresh, -39.20);
    EXPECT_DOUBLE_EQ(l->outputI, -16.58);
    EXPECT_DOUBLE_EQ(l->targetOffset, 0.58);
    EXPECT_FALSE(l->silent);
    // A silent file measures -inf.
    const auto silent = ParseLoudnorm({ "{", "\t\"input_i\" : \"-inf\",", "\t\"input_tp\" : \"-inf\",", "\t\"target_offset\" : \"inf\"", "}" });
    ASSERT_TRUE(silent.has_value());
    EXPECT_TRUE(silent->silent);
    EXPECT_FALSE(ParseLoudnorm({ "Stream #0:0: Audio: mp3", "nothing here" }).has_value());
    // Two blocks (pass 2 prints its own): the last one counts.
    auto twice = kLoudnormLog;
    twice.insert(twice.end(), { "{", "\t\"input_i\" : \"-16.10\",", "\t\"output_i\" : \"-16.02\",", "\t\"target_offset\" : \"0.00\"", "}" });
    EXPECT_DOUBLE_EQ(ParseLoudnorm(twice)->outputI, -16.02);
}

TEST(AudioTools, ParsesEbur128Summary)
{
    // What ebur128 prints at the end of a run (ffmpeg 8), after a log line of its own.
    const std::vector<std::string> summary = {
        "[Parsed_ebur128_0 @ 00007fffff373dc0] Summary:",
        "",
        "  Integrated loudness:",
        "    I:         -10.9 LUFS",
        "    Threshold: -20.9 LUFS",
        "",
        "  Loudness range:",
        "    LRA:         0.5 LU",
        "    Threshold: -30.9 LUFS",
        "    LRA low:   -11.1 LUFS",
        "    LRA high:  -10.6 LUFS",
        "",
        "  True peak:",
        "    Peak:       -4.4 dBFS",
        "[out#0/null @ 00007fffff36d040] video:0KiB audio:41344KiB subtitle:0KiB",
    };
    const auto l = ParseEbur128(summary);
    ASSERT_TRUE(l.has_value());
    EXPECT_DOUBLE_EQ(l->inputI, -10.9);
    EXPECT_DOUBLE_EQ(l->inputThresh, -20.9);   // the integrated gate, not the range's
    EXPECT_DOUBLE_EQ(l->inputLRA, 0.5);
    EXPECT_DOUBLE_EQ(l->inputTP, -4.4);
    EXPECT_FALSE(l->silent);
    // Silence: ebur128's floor and a peak of -inf.
    const auto silent = ParseEbur128({ "Summary:", "  Integrated loudness:", "    I:         -70.0 LUFS", "    Threshold:   0.0 LUFS",
                                       "  True peak:", "    Peak:        -inf dBFS" });
    ASSERT_TRUE(silent.has_value());
    EXPECT_TRUE(silent->silent);
    EXPECT_FALSE(ParseEbur128({ "Stream #0:0: Audio: mp3", "nothing here" }).has_value());
    // Without a summary line the numbers of a frame log don't count.
    EXPECT_FALSE(ParseEbur128({ "    I:         -10.9 LUFS" }).has_value());
}

TEST(AudioTools, NormalizeIsTwoPassAndLinear)
{
    EXPECT_DOUBLE_EQ(TargetFor(-14).integrated, -14);
    EXPECT_DOUBLE_EQ(TargetFor(-14).truePeak, -1);
    EXPECT_DOUBLE_EQ(TargetFor(-23).range, 20);
    const auto t = TargetFor(-16);
    // Pass 1 is ebur128 at the file's own rate, not loudnorm (which resamples to 192 kHz).
    EXPECT_EQ(MeasureFilter(t), L"ebur128=peak=true:framelog=quiet");
    const auto measured = *ParseLoudnorm(kLoudnormLog);
    // +11.6 dB would put the -4.47 dBTP peaks over -1: loudnorm limits them. The measured LRA
    // (18.06) is above the target range: widened so the dynamics are left alone.
    EXPECT_FALSE(IsLinear(t, measured));
    EXPECT_EQ(ApplyFilter(t, measured),
              L"loudnorm=I=-16:TP=-1:LRA=18.2:measured_I=-27.61:measured_TP=-4.47:measured_LRA=18.06:measured_thresh=-39.2:offset=0.58:linear=true:print_format=json");
    // When one gain keeps the peaks under -1 dBTP it is a plain volume change (loudnorm's linear mode).
    Loudness loud;
    loud.inputI = -9.3;
    loud.inputTP = 0.4;
    EXPECT_TRUE(IsLinear(t, loud));
    EXPECT_EQ(ApplyFilter(t, loud), L"volume=-6.7dB");
    Loudness quiet;
    quiet.inputI = -23.25;
    quiet.inputTP = -9.5;
    EXPECT_TRUE(IsLinear(t, quiet));
    EXPECT_EQ(ApplyFilter(t, quiet), L"volume=7.25dB");
    quiet.inputTP = -7.9;   // +7.25 dB would reach -0.65 dBTP
    EXPECT_FALSE(IsLinear(t, quiet));
    const auto pass1 = MeasureArgs(L"in.flac", t);
    EXPECT_TRUE(Has(pass1, L"-af", L"ebur128=peak=true:framelog=quiet"));
    EXPECT_TRUE(Has(pass1, L"-f", L"null"));
    EXPECT_EQ(pass1.back(), L"-");
    const auto pass2 = NormalizeArgs(L"in.mp3", L".mp3", Song(), t, measured, L"out");
    EXPECT_TRUE(Has(pass2, L"-c:a", L"libmp3lame"));   // an MP3 stays an MP3
    EXPECT_TRUE(Has(pass2, L"-ar", L"44100"));         // at its own rate (loudnorm works at 192 kHz)
    EXPECT_TRUE(Has(pass2, L"-f", L"mp3"));
    EXPECT_FALSE(Has(pass2, L"-map", L"0:a:0") && Has(pass2, L"-map", L"0:a:0") && std::count(pass2.begin(), pass2.end(), L"-map") > 1);
    EXPECT_EQ(LoudnessNote(-23.44, -16.02), L"\u221223.4 \u2192 \u221216.0 LUFS");
}

TEST(AudioTools, OutputKeepsTheSourceFormat)
{
    EXPECT_EQ(std::string_view(AudioOutputFormat(L".flac").id), "flac");
    EXPECT_EQ(std::string_view(AudioOutputFormat(L".m4b").id), "m4a");
    EXPECT_EQ(std::string_view(AudioOutputFormat(L".aif").id), "aiff");
    EXPECT_EQ(std::string_view(AudioOutputFormat(L".ape").id), "m4a");   // not one of ours: AAC
    const auto opus = AudioCodecArgs(*FindMediaFormat("opus"), 44100);
    EXPECT_TRUE(Has(opus, L"-ar", L"48000"));   // Opus only takes 48 kHz
    const auto wav = AudioCodecArgs(*FindMediaFormat("wav"), 22050);
    EXPECT_TRUE(Has(wav, L"-c:a", L"pcm_s16le"));
    EXPECT_TRUE(Has(wav, L"-ar", L"22050"));
    EXPECT_EQ(std::find(wav.begin(), wav.end(), L"-map"), wav.end());   // the tool maps its own filter output
}

// ---- Visualizer ----------------------------------------------------------------------------------

TEST(AudioTools, VisualizerShapesAndInputs)
{
    int w = 0, h = 0;
    ShapeSize(ShapeByName("portrait"), w, h);
    EXPECT_EQ(w, 1080);
    EXPECT_EQ(h, 1920);
    ShapeSize(ShapeByName("square"), w, h);
    EXPECT_EQ(w, h);
    ShapeSize(ShapeByName("anything"), w, h);
    EXPECT_EQ(w, 1920);
    const auto waves = VisualizerArgs(L"song.mp3", {}, Song("mp3", 2, 44100, 30'000'000), VisualStyle::Wave, Shape::Landscape, H264Encoder::Nvenc, L"out");
    const auto graph = After(waves, L"-filter_complex");
    EXPECT_NE(graph.find(L"showwaves=s=1920x540:mode=cline:rate=30"), std::wstring::npos) << Narrow(graph);
    EXPECT_NE(graph.find(L"color=c=0x0E0F13:s=1920x1080:r=30"), std::wstring::npos);
    EXPECT_TRUE(Has(waves, L"-t", L"30.000000"));      // exactly as long as the song
    EXPECT_TRUE(Has(waves, L"-c:a", L"aac"));          // MP3 into MP4 is encoded to AAC
    EXPECT_TRUE(Has(waves, L"-c:v", L"h264_nvenc"));
    EXPECT_TRUE(Has(waves, L"-f", L"mp4"));
    const auto still = StillVideoArgs(L"song.m4a", L"C:\\cover.jpg", Song("aac"), Shape::Square, H264Encoder::OpenH264, L"out");
    EXPECT_TRUE(Has(still, L"-loop", L"1"));
    EXPECT_TRUE(Has(still, L"-c:a", L"copy"));         // AAC is copied as it is
    EXPECT_TRUE(Has(still, L"-map", L"1:a:0"));
    EXPECT_NE(After(still, L"-filter_complex").find(L"pad=1080:1080"), std::wstring::npos);
    EXPECT_EQ(std::count(still.begin(), still.end(), L"file"), 2);   // both inputs whitelisted to local files
    EXPECT_TRUE(Has(CoverArtArgs(L"song.mp3", L"cover.png"), L"-map", L"0:v:0"));
}

TEST(AudioTools, VisualizerStylesAndBackgrounds)
{
    for (const auto* name : { "bars", "waveform", "circle", "square", "vertical", "image" })
        EXPECT_STREQ(VisualStyleName(VisualStyleByName(name)), name);
    EXPECT_EQ(VisualStyleByName("anything"), VisualStyle::Wave);
    // Bars: 80 bins of showfreqs, enlarged without smoothing to 20 px each, a 4 px gap between them.
    EXPECT_EQ(VisualGraph(VisualStyle::Bars, 1920, 1080, L"0:a:0"),
              L"[0:a:0]aformat=channel_layouts=mono,showfreqs=s=80x540:mode=bar:ascale=sqrt:fscale=log:win_size=4096:colors=0x7AB8FF:rate=30,"
              L"scale=1600:540:flags=neighbor,drawgrid=w=20:h=ih+1:t=4:c=black@0:replace=1[vis]");
    // Square: 40 x 18 blocks of 40 px with a 6 px gap both ways.
    EXPECT_EQ(VisualGraph(VisualStyle::Square, 1920, 1080, L"0:a:0"),
              L"[0:a:0]aformat=channel_layouts=mono,showfreqs=s=40x18:mode=bar:ascale=sqrt:fscale=log:win_size=4096:colors=0x7AB8FF:rate=30,"
              L"scale=1600:720:flags=neighbor,drawgrid=w=40:h=40:t=6:c=black@0:replace=1[vis]");
    // Vertical: bars a quarter of the height, and their reflection under them.
    const auto vertical = VisualGraph(VisualStyle::Vertical, 1080, 1920, L"1:a:0");
    EXPECT_NE(vertical.find(L"[1:a:0]aformat=channel_layouts=mono,showfreqs=s=45x480:"), std::wstring::npos) << Narrow(vertical);
    EXPECT_TRUE(vertical.ends_with(L",split=2[vt][vb];[vb]vflip[vf];[vt][vf]vstack[vis]")) << Narrow(vertical);
    // Circle: the sound against its Hilbert transform (delayed to match), drawn as x/y.
    const auto circle = VisualGraph(VisualStyle::Circle, 1920, 1080, L"0:a:0");
    EXPECT_NE(circle.find(L"hilbert=sample_rate=44100:taps=255[ch];[cy][ch]afir=irnorm=-1[cyh];[cx]adelay=127S[cxd]"), std::wstring::npos) << Narrow(circle);
    EXPECT_NE(circle.find(L"join=inputs=2:channel_layout=stereo,avectorscope=s=1080x1080:mode=lissajous_xy"), std::wstring::npos);
    EXPECT_TRUE(circle.ends_with(L"[vis]"));
    EXPECT_EQ(VisualGraph(VisualStyle::Wave, 1080, 1080, L"0:a:0"),
              L"[0:a:0]aformat=channel_layouts=mono,showwaves=s=1080x540:mode=cline:rate=30:colors=0x7AB8FF:scale=sqrt:draw=full[vis]");
    EXPECT_TRUE(VisualGraph(VisualStyle::Picture, 1080, 1080, L"0:a:0").empty());

    // Over a dark colour: one input, the animation centred on it.
    const auto bars = VisualizerArgs(L"song.mp3", {}, Song("mp3", 2, 44100, 30'000'000), VisualStyle::Bars, Shape::Landscape, H264Encoder::Nvenc, L"out");
    const auto barsGraph = After(bars, L"-filter_complex");
    EXPECT_TRUE(barsGraph.starts_with(L"color=c=0x0E0F13:s=1920x1080:r=30[bg];[0:a:0]")) << Narrow(barsGraph);
    EXPECT_TRUE(barsGraph.ends_with(L"[vis];[bg][vis]overlay=x=(W-w)/2:y=(H-h)/2:shortest=1,format=yuv420p[v]")) << Narrow(barsGraph);
    EXPECT_EQ(std::count(bars.begin(), bars.end(), L"-i"), 1);
    // Over a picture: the picture first (looped), filled and darkened; the sound is input 1.
    const auto over = VisualizerArgs(L"song.m4a", L"C:\\bg.jpg", Song("aac", 2, 44100, 30'000'000), VisualStyle::Circle, Shape::Square,
                                     H264Encoder::OpenH264, L"out");
    EXPECT_EQ(std::count(over.begin(), over.end(), L"-i"), 2);
    EXPECT_TRUE(Has(over, L"-loop", L"1"));
    EXPECT_TRUE(Has(over, L"-framerate", L"1"));
    const auto overGraph = After(over, L"-filter_complex");
    EXPECT_TRUE(overGraph.starts_with(L"[0:v]scale=1080:1080:force_original_aspect_ratio=increase,crop=1080:1080,setsar=1,format=yuv420p,"
                                      L"drawbox=c=black@0.45:t=fill,fps=30[bg];[1:a:0]"))
        << Narrow(overGraph);
    EXPECT_TRUE(Has(over, L"-map", L"1:a:0"));
    EXPECT_TRUE(Has(over, L"-t", L"30.000000"));
    EXPECT_EQ(std::count(over.begin(), over.end(), L"file"), 2);   // both inputs whitelisted to local files
    // Picture: the still picture, or the waveform when there is none.
    EXPECT_EQ(VisualizerArgs(L"song.m4a", L"C:\\cover.jpg", Song("aac"), VisualStyle::Picture, Shape::Square, H264Encoder::OpenH264, L"out"),
              StillVideoArgs(L"song.m4a", L"C:\\cover.jpg", Song("aac"), Shape::Square, H264Encoder::OpenH264, L"out"));
    EXPECT_EQ(VisualizerArgs(L"song.m4a", {}, Song("aac"), VisualStyle::Picture, Shape::Square, H264Encoder::OpenH264, L"out"),
              VisualizerArgs(L"song.m4a", {}, Song("aac"), VisualStyle::Wave, Shape::Square, H264Encoder::OpenH264, L"out"));
}

// ---- Trim ----------------------------------------------------------------------------------------

TEST(AudioTools, SilenceIsFoundAndTrimmed)
{
    const auto silences = ParseSilence({ "[silencedetect @ 0x1] silence_start: 0", "[silencedetect @ 0x1] silence_end: 1.25 | silence_duration: 1.25",
                                         "[silencedetect @ 0x1] silence_start: 4.5", "[silencedetect @ 0x1] silence_end: 5 | silence_duration: 0.5",
                                         "[silencedetect @ 0x1] silence_start: 8.75" },
                                       2'000'000, 12'000'000);
    ASSERT_EQ(silences.size(), 3u);
    EXPECT_EQ(silences[0], (TimeRange{ 2'000'000, 3'250'000 }));    // on the file's timeline (detection began at 2 s)
    EXPECT_EQ(silences[2], (TimeRange{ 10'750'000, 12'000'000 }));  // still silent at the end
    const auto kept = TrimSilence({ 2'000'000, 12'000'000 }, silences);
    EXPECT_EQ(kept, (TimeRange{ 3'250'000 - kSilencePadUs, 10'750'000 + kSilencePadUs }));   // the pause in the middle stays
    EXPECT_EQ(TrimSilence({ 0, 1'000'000 }, { { 0, 1'000'000 } }), (TimeRange{ 0, 1'000'000 }));   // all silence: unchanged
    const auto detect = SilenceDetectArgs(L"in.wav", { 2'000'000, 12'000'000 });
    EXPECT_EQ(After(detect, L"-af"), L"silencedetect=noise=-50dB:d=0.1");
    EXPECT_TRUE(Has(detect, L"-ss", L"2.000000"));
    const auto trim = TrimAudioArgs(L"in.flac", L".flac", Song("flac"), { 1'000'000, 2'500'000 }, L"out");
    EXPECT_TRUE(Has(trim, L"-to", L"2.500000"));
    EXPECT_TRUE(Has(trim, L"-c:a", L"flac"));
}

// ---- Channels ------------------------------------------------------------------------------------

TEST(AudioTools, PanFilters)
{
    EXPECT_EQ(PanFilter({ true, false, 1, 1 }, 2), L"pan=mono|c0=0.5*c0+0.5*c1");
    EXPECT_EQ(PanFilter({ false, false, 1, 1 }, 1), L"pan=stereo|c0=1*c0|c1=1*c0");     // mono to stereo
    EXPECT_EQ(PanFilter({ false, true, 1, 1 }, 2), L"pan=stereo|c0=1*c1|c1=1*c0");      // swapped
    EXPECT_EQ(PanFilter({ false, false, 0.5, 1.5 }, 2), L"pan=stereo|c0=0.5*c0|c1=1.5*c1");
    EXPECT_EQ(PanFilter({ true, false, 1, 0 }, 2), L"pan=mono|c0=0.5*c0+0*c1");         // left only
    EXPECT_EQ(PanFilter({ true, false, 1, 1 }, 1), L"pan=mono|c0=1*c0");
    EXPECT_EQ(PanFilter({ false, false, 1, 1 }, 6), L"aformat=channel_layouts=stereo,pan=stereo|c0=1*c0|c1=1*c1");   // 5.1 mixed down first
    EXPECT_EQ(PanFilter({ false, false, 5, -1 }, 2), L"pan=stereo|c0=2*c0|c1=0*c1");    // gains kept in 0..2
    const auto args = ChannelsArgs(L"in.wav", L".wav", Song("pcm_s16le"), { true }, L"out");
    EXPECT_TRUE(Has(args, L"-af", L"pan=mono|c0=0.5*c0+0.5*c1"));
    EXPECT_TRUE(Has(args, L"-f", L"wav"));
}

// ---- Bleep ---------------------------------------------------------------------------------------

TEST(AudioTools, BleepGraphs)
{
    const std::vector<TimeRange> ranges{ { 1'000'000, 2'000'000 }, { 5'000'000, 5'500'000 } };
    EXPECT_EQ(AnyBetween(ranges), L"between(t,1.000000,2.000000)+between(t,5.000000,5.500000)");
    EXPECT_EQ(AnyBetween(ranges, 1'500'000), L"between(t,0.000000,0.500000)+between(t,3.500000,4.000000)");   // a preview from 1.5 s
    EXPECT_EQ(AnyBetween({}), L"0");
    EXPECT_EQ(BleepGraph(ranges, BleepSound::Silence, kDuckDb, 44100), L"[0:a]volume=0:enable='between(t,1.000000,2.000000)+between(t,5.000000,5.500000)'[out]");
    // Duck: turned down by the amount, not muted.
    EXPECT_EQ(BleepGraph(ranges, BleepSound::Duck, 20, 44100), L"[0:a]volume=-20dB:enable='between(t,1.000000,2.000000)+between(t,5.000000,5.500000)'[out]");
    EXPECT_EQ(BleepGraph(ranges, BleepSound::Duck, 12.5, 44100, 1'500'000),
              L"[0:a]volume=-12.5dB:enable='between(t,0.000000,0.500000)+between(t,3.500000,4.000000)'[out]");
    EXPECT_EQ(BleepGraph(ranges, BleepSound::Duck, 500, 44100).substr(0, 20), L"[0:a]volume=-60dB:en");   // at most 60 dB
    EXPECT_EQ(BleepGraph(ranges, BleepSound::Duck, 0, 44100).substr(0, 19), L"[0:a]volume=-3dB:en");     // at least 3 dB
    EXPECT_EQ(BleepSoundByName("duck"), BleepSound::Duck);
    EXPECT_EQ(BleepSoundByName("silence"), BleepSound::Silence);
    EXPECT_EQ(BleepSoundByName("tone"), BleepSound::Tone);
    EXPECT_EQ(BleepSoundByName("other"), BleepSound::Tone);
    const auto tone = BleepGraph(ranges, BleepSound::Tone, kDuckDb, 44100);
    EXPECT_NE(tone.find(L"sine=frequency=1000:sample_rate=44100"), std::wstring::npos) << Narrow(tone);
    EXPECT_NE(tone.find(L"volume=0:enable='not(between(t,1.000000,2.000000)+between(t,5.000000,5.500000))'[t]"), std::wstring::npos);
    EXPECT_NE(tone.find(L"[m][t]amix=inputs=2:duration=first:normalize=0[out]"), std::wstring::npos);
    const auto args = BleepArgs(L"in.m4a", L".m4a", Song("aac"), ranges, BleepSound::Tone, kDuckDb, L"out");
    EXPECT_TRUE(Has(args, L"-map", L"[out]"));
    EXPECT_TRUE(Has(args, L"-f", L"ipod"));
    EXPECT_NE(After(BleepArgs(L"in.m4a", L".m4a", Song("aac"), ranges, BleepSound::Duck, 30, L"out"), L"-filter_complex").find(L"volume=-30dB"),
              std::wstring::npos);
    const auto play = PlaybackPcmArgs(L"in.m4a", { 1'500'000, 4'000'000 }, BleepGraph(ranges, BleepSound::Tone, kDuckDb, 44100, 1'500'000), true, 44100, L"pcm");
    EXPECT_TRUE(Has(play, L"-t", L"2.500000"));
    EXPECT_TRUE(Has(play, L"-map", L"[out]"));
    EXPECT_TRUE(Has(play, L"-f", L"s16le"));
    EXPECT_TRUE(Has(play, L"-ac", L"2"));
}

TEST(AudioTools, SlicesOnTheToolsWheelForAudioOnly)
{
    TempDir dir;
    Recipes recipes(dir);
    const PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr, nullptr);
    int forAudio = 0, tools = 0;
    for (const auto& r : recipes.store.All())
        if (r.ring == "tools" && runner.Applies(r, L"C:\\a\\song.flac"))
        {
            ++forAudio;
            if (!r.id.starts_with("audio-"))
                continue;
            ++tools;
            EXPECT_FALSE(runner.Applies(r, L"C:\\a\\clip.mp4")) << r.id;
        }
    // Two rings hold sixteen slices; past that the wheel pages. Normalize and the four editor tools.
    EXPECT_LE(forAudio, 16);
    EXPECT_EQ(tools, 5);
    EXPECT_NE(recipes.store.Find("audio-visualizer"), nullptr);
    EXPECT_EQ(SiblingTools(EditorTool::BleepAudio).size(), 4u);
    for (const auto tool : SiblingTools(EditorTool::BleepAudio))
    {
        EXPECT_FALSE(IsVideoTool(tool));
        Recipe parsed;
        std::string error;
        EXPECT_TRUE(ParseRecipe(ToolRecipe(tool).dump(), recipes.registry, parsed, error)) << error;
        EXPECT_EQ(parsed.steps[0].action, ToolAction(tool));
    }
    const auto* engine = runner.EngineFor(recipes.Get("audio-normalize"));
    ASSERT_NE(engine, nullptr);
    EXPECT_TRUE(engine->NeedsEditor(recipes.Get("audio-normalize")));   // Speech, Music or Broadcast
    Recipe loud = recipes.Get("audio-normalize");
    loud.steps[0].params = { { "target", -14 }, { "ask", false } };
    EXPECT_FALSE(engine->NeedsEditor(loud));   // a one-click preset with its own level
    Recipe mono = recipes.Get("audio-channels");
    mono.steps[0].params["mode"] = "mono";
    mono.steps[0].params["ask"] = false;
    EXPECT_FALSE(engine->NeedsEditor(mono));   // a one-click preset
    EXPECT_TRUE(engine->NeedsEditor(recipes.Get("audio-bleep")));
    // Sound-only tools run several files at once; the visualizer encodes video, one at a time.
    for (const char* id : { "audio-normalize", "audio-trim", "audio-channels", "audio-bleep" })
        EXPECT_EQ(runner.Lane(recipes.Get(id)), JobLane::Audio) << id;
    EXPECT_EQ(runner.Lane(recipes.Get("audio-visualizer")), JobLane::Media);
    EXPECT_TRUE(engine->NeedsEditor(recipes.Get("audio-trim")));
    EXPECT_FALSE(engine->IsBatch(recipes.Get("audio-trim")));
}

TEST(AudioTools, NormalizeAsksHowLoud)
{
    // The chooser is an ask:: question: answered here, it gives the target of the link picked.
    ask::Question asked;
    int pick = 14;
    ask::SetAnswers([&](const ask::Question& q) {
        asked = q;
        return pick;
    }, {});
    double target = 0;
    EXPECT_TRUE(ChooseLoudness(nullptr, 3, target));
    EXPECT_EQ(target, -14);
    EXPECT_EQ(asked.title, L"Normalize Volume");
    EXPECT_EQ(asked.heading, L"How loud should it be?");
    EXPECT_EQ(asked.text, L"All 3 files get the same loudness. Peaks stay under \u22121 dBTP.");
    ASSERT_EQ(asked.choices.size(), 3u);
    EXPECT_EQ(asked.choices[0].id, 16);
    EXPECT_EQ(asked.choices[0].label, L"Speech");
    EXPECT_EQ(asked.choices[0].note, L"\u221216 LUFS, for podcasts, voice notes, talks and lectures");
    EXPECT_EQ(asked.choices[1].id, 14);
    EXPECT_EQ(asked.choices[1].label, L"Music");
    EXPECT_EQ(asked.choices[2].id, 23);
    EXPECT_EQ(asked.choices[2].label, L"Broadcast");
    EXPECT_TRUE(ChooseLoudness(nullptr, 1, target));
    EXPECT_EQ(asked.text, L"Peaks stay under \u22121 dBTP.");
    // Cancel, or anything that is not one of the three, leaves the target alone.
    target = -16;
    for (const int other : { ask::kCancel, 99 })
    {
        pick = other;
        EXPECT_FALSE(ChooseLoudness(nullptr, 1, target)) << other;
        EXPECT_EQ(target, -16);
    }
    ask::SetAnswers({}, {});
    EXPECT_FALSE(ChooseLoudness(nullptr, 1, target)) << "no one to ask: cancelled, as before";
}

// ---- With the real ffmpeg.exe (SHIFTDROP_FFMPEG) --------------------------------------------------

namespace
{
    // Stereo 16-bit PCM of a whole file.
    std::vector<int16_t> Pcm(const std::wstring& path, int rate = 8000)
    {
        TempFolder temp;
        std::vector<int16_t> out;
        DecodePlaybackPcm(*TestFfmpeg(), path, { 0, 0 }, L"", false, rate, temp, {}, out);
        return out;
    }

    // Zero crossings per second of one channel over [from, to) seconds: about twice the frequency.
    double Crossings(const std::vector<int16_t>& stereo, int channel, double from, double to, int rate = 8000)
    {
        const auto a = static_cast<size_t>(from * rate), b = std::min(stereo.size() / 2, static_cast<size_t>(to * rate));
        int n = 0;
        for (size_t i = a + 1; i < b; ++i)
            n += (stereo[2 * (i - 1) + channel] < 0) != (stereo[2 * i + channel] < 0);
        return n / (to - from);
    }

    double Rms(const std::vector<int16_t>& stereo, int channel, double from, double to, int rate = 8000)
    {
        const auto a = static_cast<size_t>(from * rate), b = std::min(stereo.size() / 2, static_cast<size_t>(to * rate));
        double sum = 0;
        for (size_t i = a; i < b; ++i)
            sum += static_cast<double>(stereo[2 * i + channel]) * stereo[2 * i + channel];
        return b > a ? std::sqrt(sum / static_cast<double>(b - a)) : 0;
    }
}

TEST(AudioToolsEngine, NormalizesToTheTargetLoudness)
{
    if (!TestFfmpeg()->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    MediaToolsFixture f;
    const auto quiet = f.dir / L"quiet talk.wav";
    // A quiet, varying tone (two levels), 8 s: well below -16 LUFS.
    ASSERT_TRUE(Make({ L"-f", L"lavfi", L"-i", L"sine=f=300:d=8:sample_rate=44100,volume='if(lt(t,4),0.03,0.06)':eval=frame", L"-ac", L"2",
                       L"-c:a", L"pcm_s16le", L"-f", L"wav", quiet }));
    const auto r = f.Run("audio-normalize", {}, quiet);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    EXPECT_EQ(FileName(r.output), L"quiet talk (normalized).wav");
    EXPECT_NE(r.message.find(L"\u2192 \u221216."), std::wstring::npos) << Narrow(r.message);   // "Loudness -3x.x -> -16.0 LUFS"
    // Measured again from the file itself: within half a LU of the target, at the source's rate.
    std::vector<std::string> log;
    std::stop_source never;
    TestFfmpeg()->Run(MeasureArgs(r.output, TargetFor(-16)), nullptr, never.get_token(), &log);
    const auto after = ParseEbur128(log);
    ASSERT_TRUE(after.has_value());
    EXPECT_NEAR(after->inputI, -16, 0.6);
    EXPECT_LE(after->inputTP, -0.9);
    EXPECT_EQ(Probe(*TestFfmpeg(), r.output).sampleRate, 44100);
    // Normalizing the result again: already there, nothing written.
    const auto again = f.Run("audio-normalize", {}, r.output);
    EXPECT_FALSE(again.ok);
    EXPECT_TRUE(again.skipped);
    EXPECT_TRUE(again.error.starts_with(L"already at \u2212")) << Narrow(again.error);
    EXPECT_TRUE(again.error.ends_with(L" LUFS")) << Narrow(again.error);
    EXPECT_EQ(again.error.find(L"\u2192"), std::wstring::npos) << Narrow(again.error);
    EXPECT_EQ(CountFiles(f.dir, L".sdpart"), 0u);
}

TEST(AudioToolsEngine, NormalizeLimitsPeaksOneGainWouldClip)
{
    if (!TestFfmpeg()->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    MediaToolsFixture f;
    const auto talk = f.dir / L"talk with claps.wav";
    // A quiet tone with a loud click every second: the gain to -16 LUFS would push the clicks far over
    // -1 dBTP, so loudnorm limits them instead of one plain gain.
    ASSERT_TRUE(Make({ L"-f", L"lavfi", L"-i", L"aevalsrc=0.03*sin(2*PI*300*t)+0.9*lt(mod(t\\,1)\\,0.002):s=44100:d=8", L"-ac", L"2",
                       L"-c:a", L"pcm_s16le", L"-f", L"wav", talk }));
    std::vector<std::string> log;
    std::stop_source never;
    ASSERT_TRUE(TestFfmpeg()->Run(MeasureArgs(talk, TargetFor(-16)), nullptr, never.get_token(), &log).ok);
    const auto before = ParseEbur128(log);
    ASSERT_TRUE(before.has_value());
    EXPECT_FALSE(IsLinear(TargetFor(-16), *before));
    const auto r = f.Run("audio-normalize", { { "ask", false }, { "target", -16 } }, talk);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    log.clear();
    TestFfmpeg()->Run(MeasureArgs(r.output, TargetFor(-16)), nullptr, never.get_token(), &log);
    const auto after = ParseEbur128(log);
    ASSERT_TRUE(after.has_value());
    // Louder, with the clicks kept under the ceiling (one plain gain would have clipped them). On a
    // clip this short and this spiky loudnorm's limiter stays a few LU short of the target.
    EXPECT_GT(after->inputI, before->inputI + 3);
    EXPECT_LE(after->inputTP, -0.5);
}

TEST(AudioToolsEngine, VisualizerMakesAVideo)
{
    if (!TestFfmpeg()->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    MediaToolsFixture f;
    const auto song = f.dir / L"song.mp3";
    ASSERT_TRUE(Make({ L"-f", L"lavfi", L"-i", L"sine=f=220:d=3", L"-c:a", L"libmp3lame", L"-f", L"mp3", song }));
    const auto waves = f.Run("audio-visualizer", { { "shape", "square" } }, song);
    ASSERT_TRUE(waves.ok) << Narrow(waves.error);
    EXPECT_EQ(FileName(waves.output), L"song (video).mp4");
    const auto v = Probe(*TestFfmpeg(), waves.output);
    EXPECT_EQ(v.width, 1080);
    EXPECT_EQ(v.height, 1080);
    EXPECT_TRUE(v.HasAudio());
    EXPECT_NEAR(static_cast<double>(v.DurationUs()) / 1e6, 3.0, 0.15);
    const auto picture = f.dir / L"cover.png";
    ASSERT_TRUE(SUCCEEDED(WritePng(picture, 64, 48)));
    // Every moving style, and one over a picture, renders a video as long as the song.
    for (const auto* style : { "bars", "circle", "square", "vertical" })
    {
        const auto moving = f.Run("audio-visualizer", { { "style", style }, { "shape", "landscape" } }, song);
        ASSERT_TRUE(moving.ok) << style << ": " << Narrow(moving.error);
        const auto m = Probe(*TestFfmpeg(), moving.output);
        EXPECT_EQ(m.width, 1920) << style;
        EXPECT_EQ(m.height, 1080) << style;
        EXPECT_NEAR(static_cast<double>(m.DurationUs()) / 1e6, 3.0, 0.15) << style;
    }
    const auto over = f.Run("audio-visualizer", { { "style", "bars" }, { "image", Narrow(picture) }, { "shape", "square" } }, song);
    ASSERT_TRUE(over.ok) << Narrow(over.error);
    const auto o = Probe(*TestFfmpeg(), over.output);
    EXPECT_EQ(o.width, 1080);
    EXPECT_TRUE(o.HasAudio());
    EXPECT_NEAR(static_cast<double>(o.DurationUs()) / 1e6, 3.0, 0.15);
    const auto still = f.Run("audio-visualizer", { { "style", "image" }, { "image", Narrow(picture) }, { "shape", "portrait" } }, song);
    ASSERT_TRUE(still.ok) << Narrow(still.error);
    const auto s = Probe(*TestFfmpeg(), still.output);
    EXPECT_EQ(s.width, 1080);
    EXPECT_EQ(s.height, 1920);
    EXPECT_NEAR(static_cast<double>(s.DurationUs()) / 1e6, 3.0, 0.15);
}

TEST(AudioToolsEngine, TrimsExactlyAndDropsSilence)
{
    if (!TestFfmpeg()->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    MediaToolsFixture f;
    // 1 s silence, 2 s tone, 1 s silence.
    const auto padded = f.dir / L"padded.flac";
    ASSERT_TRUE(Make({ L"-f", L"lavfi", L"-i", L"sine=f=440:d=4:sample_rate=44100,volume=0:enable='lt(t,1)+gte(t,3)'", L"-c:a", L"flac", L"-f", L"flac", padded }));
    const auto exact = f.Run("audio-trim", { { "start", 0.5 }, { "end", 1.75 } }, padded);
    ASSERT_TRUE(exact.ok) << Narrow(exact.error);
    EXPECT_NEAR(static_cast<double>(Probe(*TestFfmpeg(), exact.output).DurationUs()) / 1e6, 1.25, 0.02);
    const auto trimmed = f.Run("audio-trim", { { "silence", true } }, padded);
    ASSERT_TRUE(trimmed.ok) << Narrow(trimmed.error);
    EXPECT_NEAR(static_cast<double>(Probe(*TestFfmpeg(), trimmed.output).DurationUs()) / 1e6, 2.0 + 2 * kSilencePadUs / 1e6, 0.06);
}

TEST(AudioToolsEngine, ChannelsAndBleep)
{
    if (!TestFfmpeg()->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    MediaToolsFixture f;
    // A tone on the left only.
    const auto left = f.dir / L"left.wav";
    ASSERT_TRUE(Make({ L"-f", L"lavfi", L"-i", L"sine=f=440:d=3:sample_rate=8000", L"-af", L"pan=stereo|c0=c0|c1=0*c0", L"-c:a", L"pcm_s16le", L"-f", L"wav", left }));
    const auto swapped = f.Run("audio-channels", { { "swap", true } }, left);
    ASSERT_TRUE(swapped.ok) << Narrow(swapped.error);
    EXPECT_EQ(FileName(swapped.output), L"left (swapped).wav");
    const auto pcm = Pcm(swapped.output);
    EXPECT_LT(Rms(pcm, 0, 0.5, 2.5), 50);     // left is silent now
    EXPECT_GT(Rms(pcm, 1, 0.5, 2.5), 1000);   // and the tone is on the right
    const auto mono = f.Run("audio-channels", { { "mode", "mono" }, { "ask", false } }, left);
    ASSERT_TRUE(mono.ok) << Narrow(mono.error);
    EXPECT_EQ(Probe(*TestFfmpeg(), mono.output).channels, 1);
    EXPECT_FALSE(f.Run("audio-channels", { { "mode", "mono" }, { "ask", false } }, mono.output).ok);   // already mono

    const auto talk = f.dir / L"talk.wav";
    ASSERT_TRUE(Make({ L"-f", L"lavfi", L"-i", L"sine=f=440:d=4:sample_rate=8000", L"-ac", L"2", L"-c:a", L"pcm_s16le", L"-f", L"wav", talk }));
    nlohmann::json ranges = nlohmann::json::array({ nlohmann::json::array({ 1.0, 2.0 }) });
    const auto bleeped = f.Run("audio-bleep", { { "ranges", ranges } }, talk);
    ASSERT_TRUE(bleeped.ok) << Narrow(bleeped.error);
    const auto b = Pcm(bleeped.output);
    EXPECT_NEAR(Crossings(b, 0, 0.1, 0.9), 880, 40);    // the original 440 Hz before
    EXPECT_NEAR(Crossings(b, 0, 1.1, 1.9), 2000, 60);   // the 1 kHz bleep inside
    EXPECT_NEAR(Crossings(b, 0, 2.1, 3.9), 880, 40);    // and the original after
    const auto silenced = f.Run("audio-bleep", { { "ranges", ranges }, { "tone", "silence" } }, talk);
    ASSERT_TRUE(silenced.ok) << Narrow(silenced.error);
    const auto s = Pcm(silenced.output);
    EXPECT_LT(Rms(s, 0, 1.1, 1.9), 10);
    EXPECT_GT(Rms(s, 0, 2.1, 3.9), 1000);
    // Duck by 20 dB: the same tone, a tenth as loud, inside the range only.
    const auto ducked = f.Run("audio-bleep", { { "ranges", ranges }, { "tone", "duck" }, { "duck", 20 } }, talk);
    ASSERT_TRUE(ducked.ok) << Narrow(ducked.error);
    const auto d = Pcm(ducked.output);
    const double outside = Rms(d, 0, 2.1, 3.9), inside = Rms(d, 0, 1.1, 1.9);
    EXPECT_GT(outside, 1000);
    EXPECT_NEAR(inside / outside, 0.1, 0.02);
    EXPECT_NEAR(Crossings(d, 0, 1.1, 1.9), 880, 40);   // still the 440 Hz original, only quieter
    EXPECT_EQ(CountFiles(f.dir, L".sdpart"), 0u);
}
