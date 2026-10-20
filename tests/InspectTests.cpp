#include "Engine.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "inspect/InspectModule.h"
#include "inspect/Metadata.h"
#include "inspect/MetadataReader.h"
#include "inspect/ProbeReport.h"

#include <propvarutil.h>

using namespace sd;
using namespace sd::inspect;
using namespace sd::test;

namespace
{
    std::shared_ptr<const FfmpegEngine> Engine()
    {
        static const auto engine = std::make_shared<const FfmpegEngine>(FfmpegEngine::Locate());
        return engine;
    }

    const Field* Find(const std::vector<Field>& fields, std::wstring_view group, std::wstring_view name)
    {
        for (const auto& f : fields)
            if (f.group == group && f.name == name)
                return &f;
        return nullptr;
    }

    const std::vector<std::string> kProbeLog = {
        "Input #0, matroska,webm, from 'C:\\clips\\talk.mkv':",
        "  Metadata:",
        "    title           : Team talk",
        "    COMMENT         : first line",
        "                    : second line",
        "    ENCODER         : Lavf61.7.100",
        "  Duration: 00:01:30.50, start: 0.000000, bitrate: 2500 kb/s",
        "  Chapters:",
        "    Chapter #0:0: start 0.000000, end 45.000000",
        "      Metadata:",
        "        title           : Intro",
        "    Chapter #0:1: start 45.000000, end 90.500000",
        "      Metadata:",
        "        title           : Questions",
        "  Stream #0:0: Video: h264 (High), yuv420p(progressive), 1920x1080 [SAR 1:1 DAR 16:9], 29.97 fps, 29.97 tbr, 1k tbn (default)",
        "      Metadata:",
        "        HANDLER_NAME    : VideoHandler",
        "  Stream #0:1(eng): Audio: aac (LC), 48000 Hz, stereo, fltp, 160 kb/s (default)",
        "      Metadata:",
        "        title           : English",
        "  Stream #0:2(fre): Subtitle: subrip",
        "  Stream #0:3: Video: mjpeg (Baseline), yuvj420p(pc, bt470bg/unknown/unknown), 600x600 [SAR 1:1 DAR 1:1], 90k tbr, 90k tbn (attached pic)",
        "At least one output file must be specified",
    };
}

TEST(InspectModel, GroupsInOrderAndDropsRepeats)
{
    const std::vector<Field> fields = {
        { L"EXIF", L"Make", L"Canon" },
        { L"Camera", L"Camera maker", L"Canon" },
        { L"File", L"Name", L"a.jpg" },
        { L"Audio track 1", L"Codec", L"aac" },
        { L"Video track 1", L"Codec", L"h264" },
        { L"Zebra", L"x", L"y" },
        { L"Camera", L"Camera maker", L"Canon" },   // a repeat (the property system and WIC both said it)
        { L"GPS", L"Latitude", L"1" },
        { L"Chapters", L"1. Intro", L"0:00" },
    };
    const auto groups = GroupFields(fields);
    std::vector<std::wstring> names;
    for (const auto& g : groups)
        names.push_back(g.name);
    EXPECT_EQ(names, (std::vector<std::wstring>{ L"File", L"Camera", L"GPS", L"Audio track 1", L"Video track 1", L"Chapters", L"EXIF", L"Zebra" }));
    EXPECT_EQ(groups[1].fields.size(), 1u);
}

TEST(InspectModel, SearchMatchesEveryWord)
{
    const std::vector<Field> fields = {
        { L"Camera", L"Camera model", L"Canon EOS R6" },
        { L"Camera", L"F-stop", L"f/2.8" },
        { L"GPS", L"Latitude", L"51; 30; 12" },
        { L"File", L"Name", L"holiday.jpg" },
    };
    EXPECT_EQ(Filter(fields, L"").size(), 4u);
    EXPECT_EQ(Filter(fields, L"canon").size(), 1u);
    EXPECT_EQ(Filter(fields, L"CAMERA").size(), 2u);           // the group counts
    EXPECT_EQ(Filter(fields, L"camera  2.8").size(), 1u);      // every word must match
    EXPECT_EQ(Filter(fields, L"gps 51").front().name, L"Latitude");
    EXPECT_TRUE(Filter(fields, L"nikon").empty());
}

TEST(InspectModel, PropertyGroups)
{
    EXPECT_EQ(PropertyGroup(L"System.GPS.Latitude"), L"GPS");
    EXPECT_EQ(PropertyGroup(L"System.Photo.CameraModel"), L"Camera");
    EXPECT_EQ(PropertyGroup(L"System.Image.Dimensions"), L"Image");
    EXPECT_EQ(PropertyGroup(L"System.Video.FrameRate"), L"Media");
    EXPECT_EQ(PropertyGroup(L"System.Music.Artist"), L"Media");
    EXPECT_EQ(PropertyGroup(L"System.Title"), L"Description");
    EXPECT_EQ(PropertyGroup(L"System.Author"), L"Description");
    EXPECT_EQ(PropertyGroup(L"System.TitleSomething"), L"Other properties");
    EXPECT_EQ(PropertyGroup(L"System.Size"), L"");               // the File group has it
    EXPECT_EQ(PropertyGroup(L"System.DateModified"), L"");
    EXPECT_EQ(PropertyGroup(L"System.ItemFolderPathDisplay"), L"");
    EXPECT_EQ(PropertyGroup(L"System.Document.PageCount"), L"Document");
}

TEST(InspectModel, WicPathsAndExifNames)
{
    auto d = DescribeWicPath(L"/app1/ifd/exif/{ushort=33434}");
    EXPECT_EQ(d.group, L"EXIF");
    EXPECT_EQ(d.name, L"ExposureTime");
    d = DescribeWicPath(L"/app1/ifd/{ushort=272}");
    EXPECT_EQ(d.name, L"Model");
    d = DescribeWicPath(L"/app1/ifd/gps/{ushort=2}");
    EXPECT_EQ(d.group, L"EXIF GPS");
    EXPECT_EQ(d.name, L"GPSLatitude");
    d = DescribeWicPath(L"/xmp/dc:creator/{ulong=0}");
    EXPECT_EQ(d.group, L"XMP");
    EXPECT_EQ(d.name, L"dc:creator [0]");
    d = DescribeWicPath(L"/xmp/{wstr=http://ns.adobe.com/xap/1.0/}:CreatorTool");
    EXPECT_EQ(d.group, L"XMP");
    d = DescribeWicPath(L"/tEXt/{str=Software}");
    EXPECT_EQ(d.group, L"PNG text");
    EXPECT_EQ(d.name, L"Software");
    EXPECT_TRUE(DescribeWicPath(L"/app1/thumb/{ushort=513}").skip);
    EXPECT_TRUE(DescribeWicPath(L"/app1/ifd/{ushort=34665}").skip);   // the pointer to the EXIF block
    EXPECT_EQ(DescribeWicPath(L"/ifd/{ushort=315}").name, L"Artist");
    EXPECT_EQ(ExifTagName(0xABCD, false), L"Tag 0xABCD");
}

TEST(InspectModel, NumbersAndTimes)
{
    EXPECT_EQ(FormatRational(1, 250), L"1/250");
    EXPECT_EQ(FormatRational(10, 2500), L"1/250");
    EXPECT_EQ(FormatRational(28, 10), L"2.8");
    EXPECT_EQ(FormatRational(72, 1), L"72");
    EXPECT_EQ(FormatRational(5, 0), L"5");
    EXPECT_EQ(FormatRational(-1, 3), L"-0.3333");
    EXPECT_EQ(FileSizeText(512), L"512 bytes");
    EXPECT_EQ(FileSizeText(3456789), L"3.3 MB (3,456,789 bytes)");
    EXPECT_EQ(DurationText(90'500'000), L"0:01:30.500");
    EXPECT_EQ(DurationText(-1), L"unknown");
}

TEST(InspectModel, ValuesAsText)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_UI8;
    v.uhVal.QuadPart = (250ull << 32) | 1u;   // 1/250 the way WIC packs EXIF rationals
    EXPECT_EQ(ValueText(v), L"1/250");
    v.vt = VT_UI2;
    v.uiVal = 6;
    EXPECT_EQ(ValueText(v), L"6");
    PROPVARIANT s;
    ASSERT_HRESULT_SUCCEEDED(InitPropVariantFromString(L"Canon", &s));
    EXPECT_EQ(ValueText(s), L"Canon");
    PropVariantClear(&s);
    // ExifVersion is four ASCII bytes.
    BYTE version[] = { '0', '2', '3', '2' };
    PROPVARIANT b;
    ASSERT_HRESULT_SUCCEEDED(InitPropVariantFromBuffer(version, sizeof(version), &b));
    EXPECT_EQ(ValueText(b), L"0232");
    PropVariantClear(&b);
}

TEST(InspectProbe, ParsesTracksChaptersAndTags)
{
    const auto r = ParseProbeReport(kProbeLog);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.container, "matroska,webm");
    EXPECT_EQ(r.durationUs, 90'500'000);
    EXPECT_EQ(r.bitrate, 2'500'000);
    ASSERT_EQ(r.tags.size(), 3u);
    EXPECT_EQ(r.tags[0], (std::pair<std::string, std::string>{ "title", "Team talk" }));
    EXPECT_EQ(r.tags[1].second, "first line\nsecond line");
    ASSERT_EQ(r.chapters.size(), 2u);
    EXPECT_EQ(r.chapters[1].title, "Questions");
    EXPECT_DOUBLE_EQ(r.chapters[1].start, 45.0);
    EXPECT_DOUBLE_EQ(r.chapters[1].end, 90.5);
    ASSERT_EQ(r.streams.size(), 4u);
    EXPECT_EQ(r.streams[0].kind, "Video");
    EXPECT_EQ(r.streams[0].codec, "h264");
    EXPECT_EQ(r.streams[0].width, 1920);
    EXPECT_NEAR(r.streams[0].fps, 29.97, 1e-9);
    EXPECT_TRUE(r.streams[0].isDefault);
    EXPECT_EQ(r.streams[0].tags.at(0).first, "HANDLER_NAME");
    EXPECT_EQ(r.streams[1].language, "eng");
    EXPECT_EQ(r.streams[1].sampleRate, 48000);
    EXPECT_EQ(r.streams[1].channels, "stereo");
    EXPECT_EQ(r.streams[1].bitrate, 160'000);
    EXPECT_EQ(r.streams[1].tags.at(0).second, "English");
    EXPECT_EQ(r.streams[2].kind, "Subtitle");
    EXPECT_EQ(r.streams[2].language, "fre");
    EXPECT_TRUE(r.streams[3].attachedPic);
    EXPECT_EQ(ChannelCount("5.1(side)"), 6);
    EXPECT_EQ(ChannelCount("mono"), 1);
    EXPECT_EQ(ChannelCount(""), 0);
}

TEST(InspectProbe, FieldsPerTrack)
{
    const auto fields = ProbeFields(ParseProbeReport(kProbeLog));
    ASSERT_NE(Find(fields, L"Media", L"Duration"), nullptr);
    EXPECT_EQ(Find(fields, L"Media", L"Duration")->value, L"0:01:30.500");
    EXPECT_EQ(Find(fields, L"Media", L"Tracks")->value, L"1 video, 1 audio, 1 subtitle, 1 other");
    EXPECT_EQ(Find(fields, L"Video track 1", L"Size")->value, L"1920 \u00d7 1080");
    EXPECT_EQ(Find(fields, L"Audio track 1", L"Language")->value, L"eng");
    EXPECT_EQ(Find(fields, L"Audio track 1", L"title")->value, L"English");
    EXPECT_NE(Find(fields, L"Subtitle track 1", L"Codec"), nullptr);
    EXPECT_EQ(Find(fields, L"Other track 1", L"Kind")->value, L"Cover art");
    EXPECT_EQ(Find(fields, L"Chapters", L"2. Questions")->value, L"0:00:45.000 \u2192 0:01:30.500");
    EXPECT_EQ(Find(fields, L"Tags", L"title")->value, L"Team talk");
    EXPECT_TRUE(ProbeFields(ParseProbeReport({ "garbage" })).empty());
}

TEST(InspectReader, ReadsAnImage)
{
    TempDir dir;
    const auto png = dir / L"pic.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(png, 64, 32));
    const auto m = ReadMetadata(png, nullptr, std::stop_token{});
    EXPECT_TRUE(m.error.empty());
    ASSERT_NE(Find(m.fields, L"File", L"Name"), nullptr);
    EXPECT_EQ(Find(m.fields, L"File", L"Name")->value, L"pic.png");
    ASSERT_NE(Find(m.fields, L"File", L"Size"), nullptr);
    ASSERT_NE(Find(m.fields, L"Image", L"Dimensions"), nullptr);
    EXPECT_EQ(Find(m.fields, L"Image", L"Dimensions")->value, L"64 \u00d7 32 pixels");

    const auto missing = ReadMetadata(dir / L"nope.png", nullptr, std::stop_token{});
    EXPECT_FALSE(missing.error.empty());
}

TEST(InspectReader, ReadsTracksAndChaptersThroughFfmpeg)
{
    const auto engine = Engine();
    if (!engine->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    TempDir dir;
    const auto meta = dir / L"chapters.txt";
    WriteText(meta, ";FFMETADATA1\ntitle=Inspector test\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=1000\ntitle=Intro\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=1000\nEND=2000\ntitle=Outro\n");
    const auto clip = dir / L"clip.mkv";
    std::stop_source never;
    ASSERT_TRUE(engine->Run({ L"-hide_banner", L"-nostdin", L"-f", L"lavfi", L"-i", L"testsrc=s=160x120:r=10:d=2", L"-f", L"lavfi",
                              L"-i", L"sine=d=2", L"-i", meta, L"-map", L"0", L"-map", L"1", L"-map_metadata", L"2", L"-map_chapters", L"2",
                              L"-c:v", L"mpeg4", L"-c:a", L"flac", L"-y", L"-f", L"matroska", clip },
                            nullptr, never.get_token()).ok);
    const auto m = ReadMetadata(clip, engine.get(), std::stop_token{});
    ASSERT_NE(Find(m.fields, L"Video track 1", L"Codec"), nullptr);
    EXPECT_EQ(Find(m.fields, L"Video track 1", L"Codec")->value, L"mpeg4");
    EXPECT_EQ(Find(m.fields, L"Audio track 1", L"Codec")->value, L"flac");
    ASSERT_NE(Find(m.fields, L"Chapters", L"1. Intro"), nullptr);
    EXPECT_NE(Find(m.fields, L"Chapters", L"2. Outro"), nullptr);
    ASSERT_NE(Find(m.fields, L"Tags", L"title"), nullptr);
    EXPECT_EQ(Find(m.fields, L"Tags", L"title")->value, L"Inspector test");
}

TEST(InspectModule, InspectSliceIsOnToolsForEveryFile)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto& inspect = recipes.Get("inspect");
    EXPECT_EQ(inspect.ring, "tools");
    EXPECT_EQ(inspect.icon, 0xE946);
    const PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    const auto* engine = runner.EngineFor(inspect);
    ASSERT_NE(engine, nullptr);
    EXPECT_TRUE(engine->NeedsEditor(inspect));
    const auto txt = dir / L"notes.txt";
    WriteText(txt, "hello");
    EXPECT_TRUE(runner.Applies(inspect, txt));
    EXPECT_TRUE(runner.Accepts(inspect, txt));
    EXPECT_FALSE(runner.Applies(inspect, dir.path().wstring()));   // folders: not here

    // The window opener the app installs gets the files.
    static std::vector<std::wstring> opened;
    SetWindowOpener([](HWND, const std::vector<std::wstring>& files, std::shared_ptr<const FfmpegEngine>) { opened = files; });
    engine->OpenEditor(nullptr, inspect, { txt }, [](Recipe, std::vector<std::wstring>) { FAIL() << "read-only: never runs"; });
    SetWindowOpener(nullptr);
    EXPECT_EQ(opened, (std::vector<std::wstring>{ txt }));
}
