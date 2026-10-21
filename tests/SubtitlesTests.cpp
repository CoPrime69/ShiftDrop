#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "subtitles/Subtitles.h"
#include "subtitles/SubtitlesModule.h"

using namespace sd;
using namespace sd::subtitles;
using namespace sd::test;

TEST(Subtitles, TimestampsParseInEveryCommonForm)
{
    int64_t ms = -1;
    EXPECT_TRUE(ParseTimestamp("01:02:03,456", ms));
    EXPECT_EQ(ms, 3723456);
    EXPECT_TRUE(ParseTimestamp("01:02:03.456", ms));
    EXPECT_EQ(ms, 3723456);
    EXPECT_TRUE(ParseTimestamp(" 02:03.456 ", ms));   // VTT without hours
    EXPECT_EQ(ms, 123456);
    EXPECT_TRUE(ParseTimestamp("1:02:03,5", ms));     // short fraction: tenths
    EXPECT_EQ(ms, 3723500);
    EXPECT_TRUE(ParseTimestamp("00:00:01:250", ms));  // colon before the milliseconds
    EXPECT_EQ(ms, 1250);
    EXPECT_TRUE(ParseTimestamp("100:00:00,000", ms)); // more than 99 hours
    EXPECT_EQ(ms, 360000000);
    EXPECT_FALSE(ParseTimestamp("", ms));
    EXPECT_FALSE(ParseTimestamp("12", ms));
    EXPECT_FALSE(ParseTimestamp("00:61:00,000", ms));
    EXPECT_FALSE(ParseTimestamp("00:00:01,000x", ms));
    EXPECT_FALSE(ParseTimestamp("00:00:01,000,1", ms));

    EXPECT_EQ(FormatSrtTime(3723456), "01:02:03,456");
    EXPECT_EQ(FormatVttTime(3723456), "01:02:03.456");
    EXPECT_EQ(FormatSrtTime(-5), "00:00:00,000");
    EXPECT_EQ(FormatVttTime(360000001), "100:00:00.001");
}

TEST(Subtitles, SrtWithBomCrlfAndTagsBecomesVtt)
{
    const std::string srt =
        "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,500\r\n<i>Hello</i> & <font color=\"#ff0\">welcome</font>\r\n{\\an8}Second line\r\n\r\n"
        "2\r\n00:00:03,000 --> 00:00:04,000\r\nA < B --> C\r\n\r\n";
    const auto track = Parse(srt);
    ASSERT_EQ(track.cues.size(), 2u);
    EXPECT_EQ(track.source, Format::Srt);
    EXPECT_EQ(track.cues[0].id, "1");
    EXPECT_EQ(track.cues[0].startMs, 1000);
    EXPECT_EQ(track.cues[0].endMs, 2500);
    EXPECT_EQ(track.cues[0].text, "<i>Hello</i> & <font color=\"#ff0\">welcome</font>\n{\\an8}Second line");

    EXPECT_EQ(WriteVtt(track),
              "WEBVTT\n\n"
              "1\n00:00:01.000 --> 00:00:02.500\n<i>Hello</i> &amp; welcome\nSecond line\n\n"
              "2\n00:00:03.000 --> 00:00:04.000\nA &lt; B --&gt; C\n");
}

TEST(Subtitles, MissingIndicesAndMissingBlankLinesAreTolerated)
{
    // No index on the first cue, no blank line before the third, a wrong index on the second.
    const std::string srt =
        "00:00:01,000 --> 00:00:02,000\nOne\n\n"
        "7\n00:00:03,000 --> 00:00:04,000\nTwo\n"
        "3\n00:00:05,000 --> 00:00:06,000\nThree\n";
    const auto track = Parse(srt);
    ASSERT_EQ(track.cues.size(), 3u);
    EXPECT_EQ(track.cues[0].text, "One");
    EXPECT_EQ(track.cues[1].text, "Two");
    EXPECT_EQ(track.cues[1].id, "7");
    EXPECT_EQ(track.cues[2].text, "Three");
    EXPECT_EQ(track.cues[2].id, "3");
    // Written back numbered from 1.
    EXPECT_EQ(WriteSrt(track),
              "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,000\r\nOne\r\n\r\n"
              "2\r\n00:00:03,000 --> 00:00:04,000\r\nTwo\r\n\r\n"
              "3\r\n00:00:05,000 --> 00:00:06,000\r\nThree\r\n\r\n");
}

TEST(Subtitles, StrayBlankLineInsideACueKeepsTheText)
{
    const auto track = Parse("1\n00:00:01,000 --> 00:00:02,000\nFirst\n\nstill first\n\n2\n00:00:03,000 --> 00:00:04,000\nSecond\n");
    ASSERT_EQ(track.cues.size(), 2u);
    EXPECT_EQ(track.cues[0].text, "First\nstill first");
    EXPECT_EQ(track.cues[1].text, "Second");
}

TEST(Subtitles, LoneCrLineEndsEmptyCuesAndBackwardTimes)
{
    const auto track = Parse("1\r00:00:05,000 --> 00:00:06,000\rLater\r\r2\r00:00:01,000 --> 00:00:00,500\rEarlier\r\r3\r00:00:07,000 --> 00:00:08,000\r\r");
    ASSERT_EQ(track.cues.size(), 2u);   // the empty third cue is dropped
    EXPECT_EQ(track.cues[0].text, "Earlier");   // sorted by start
    EXPECT_EQ(track.cues[0].endMs, 1000);       // end clamped to start
    EXPECT_EQ(track.cues[1].text, "Later");
}

TEST(Subtitles, VttHeaderNotesStylesAndSettings)
{
    const std::string vtt =
        "WEBVTT - My film\nKind: captions\nLanguage: en\n\n"
        "STYLE\n::cue { color: yellow }\n\n"
        "NOTE This is a comment\nover two lines\n\n"
        "intro\n00:01.000 --> 00:02.000 align:start position:10%\n<v Bob>Hi &amp; <c.loud>bye</c></v>\n\n"
        "NOTE\nanother\n\n"
        "00:00:03.000 --> 00:00:04.000 line:0\n<b.x>Bold</b> <00:00:03.500>later <ruby>漢<rt>kan</rt></ruby>\n";
    const auto track = Parse(vtt);
    EXPECT_EQ(track.source, Format::Vtt);
    ASSERT_EQ(track.cues.size(), 2u);
    EXPECT_EQ(track.cues[0].id, "intro");
    EXPECT_EQ(track.cues[0].startMs, 1000);
    EXPECT_EQ(track.cues[1].startMs, 3000);

    EXPECT_EQ(WriteSrt(track),
              "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,000\r\nHi & bye\r\n\r\n"
              "2\r\n00:00:03,000 --> 00:00:04,000\r\n<b>Bold</b> later 漢\r\n\r\n");
    EXPECT_EQ(WriteText(track), "Hi & bye\r\nBold later 漢\r\n");
}

TEST(Subtitles, VttWithoutBlankLineAfterHeader)
{
    const auto track = Parse("WEBVTT\n00:00.500 --> 00:01.000\nQuick\n");
    ASSERT_EQ(track.cues.size(), 1u);
    EXPECT_EQ(track.cues[0].text, "Quick");
}

TEST(Subtitles, TextMergesLinesAndDropsRollUpRepeats)
{
    const auto track = Parse(
        "1\n00:00:01,000 --> 00:00:02,000\n<i>so we   went</i>\nto the shop\n\n"
        "2\n00:00:02,000 --> 00:00:03,000\nto the shop\nand bought bread\n\n"
        "3\n00:00:03,000 --> 00:00:04,000\nand bought bread\n\n");
    EXPECT_EQ(WriteText(track), "so we went to the shop\r\nand bought bread\r\n");
}

TEST(Subtitles, EngineConvertsFilesAndSkipsSameFormat)
{
    TempDir dir;
    Recipes recipes(dir);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    const auto srt = dir / L"film.srt";
    // UTF-16 LE with a BOM, as some subtitle editors save.
    const std::wstring wide = L"\xFEFF" L"1\r\n00:00:01,000 --> 00:00:02,000\r\nCafé\r\n";
    WriteText(srt, std::string(reinterpret_cast<const char*>(wide.data()), wide.size() * 2));

    EXPECT_FALSE(runner.Applies(recipes.Get("subs-to-srt"), srt));
    EXPECT_TRUE(runner.Applies(recipes.Get("subs-to-vtt"), srt));
    EXPECT_TRUE(runner.Applies(recipes.Get("subs-to-txt"), srt));
    EXPECT_FALSE(runner.Applies(recipes.Get("subs-to-vtt"), dir / L"notes.txt"));

    const auto vtt = runner.Run(recipes.Get("subs-to-vtt"), 0, srt, {});
    ASSERT_TRUE(vtt.ok) << Narrow(vtt.error);
    EXPECT_EQ(std::filesystem::path(vtt.output).extension(), L".vtt");
    EXPECT_EQ(ReadAll(vtt.output), "WEBVTT\n\n1\n00:00:01.000 --> 00:00:02.000\nCafé\n");
    // Never overwrites: a second run makes a second file.
    const auto again = runner.Run(recipes.Get("subs-to-vtt"), 0, srt, {});
    ASSERT_TRUE(again.ok);
    EXPECT_NE(again.output, vtt.output);

    const auto empty = dir / L"empty.vtt";
    WriteText(empty, "WEBVTT\n\nNOTE nothing here\n");
    const auto none = runner.Run(recipes.Get("subs-to-srt"), 0, empty, {});
    EXPECT_FALSE(none.ok);
    EXPECT_EQ(CountFiles(dir, L".srt"), 1u);   // only the source
}
