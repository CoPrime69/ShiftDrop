#include "Brand.h"
#include "ErrorText.h"
#include "FileIo.h"
#include "FormatText.h"
#include "HistoryStore.h"
#include "OutputPath.h"
#include "PathUtil.h"
#include "Recipe.h"
#include "StagedOutput.h"
#include "Strings.h"
#include "TempPath.h"
#include "TestUtil.h"

#include <cmath>
#include <filesystem>
#include <limits>

using namespace sd;
using namespace sd::test;

// ---- Strings.h -----------------------------------------------------------------------------------

TEST(Strings, CaseAndWhitespace)
{
    EXPECT_EQ(Lower(L"Photo.JPG"), L"photo.jpg");
    EXPECT_EQ(Lower(L"a-Z_09"), L"a-z_09");
    EXPECT_EQ(AsciiLower("<W:Body>"), "<w:body>");
    EXPECT_EQ(Trim(std::wstring_view(L" \t name \r\n")), L"name");
    EXPECT_EQ(Trim(std::string_view("\f\v value \n")), "value");
    EXPECT_EQ(Trim(std::wstring_view(L"   ")), L"");
    EXPECT_EQ(Indent("    Stream #0:0"), 4u);
    EXPECT_EQ(Indent("Input #0"), 0u);
}

TEST(Strings, EqualsNoCaseIsOrdinal)
{
    EXPECT_TRUE(EqualsNoCase(L"C:\\Pics\\A.png", L"c:\\pics\\a.PNG"));
    EXPECT_FALSE(EqualsNoCase(L"a.png", L"a.png "));
    EXPECT_FALSE(EqualsNoCase(L"a", L"b"));
    EXPECT_TRUE(EqualsNoCase(L"", L""));
}

TEST(Strings, ExtensionTokens)
{
    EXPECT_TRUE(HasExtToken(L".mp4.m4v", L".mp4"));
    EXPECT_TRUE(HasExtToken(L".mp4.m4v", L".m4v"));
    EXPECT_FALSE(HasExtToken(L".mp4.m4v", L".mp"));
    EXPECT_FALSE(HasExtToken(L".mp4.m4v", L".m4"));
    EXPECT_FALSE(HasExtToken(L".mp4", L""));
}

TEST(Strings, Latin1StopsAtNul)
{
    const uint8_t bytes[] = { 'C', 'a', 'f', 0xE9, 0, 'x' };
    EXPECT_EQ(Latin1(bytes), L"Café");
    EXPECT_EQ(Latin1({}), L"");
}

// ---- PathUtil.h ----------------------------------------------------------------------------------

TEST(PathUtil, FileName)
{
    EXPECT_EQ(FileName(L"C:\\pics\\photo.jpg"), L"photo.jpg");
    EXPECT_EQ(FileName(L"photo.jpg"), L"photo.jpg");
    EXPECT_EQ(FileName(L"D:\\"), L"");
}

TEST(PathUtil, IsWithin)
{
    EXPECT_TRUE(IsWithin(L"C:\\Pics\\a\\b.png", L"c:\\pics"));
    EXPECT_FALSE(IsWithin(L"C:\\Pictures\\b.png", L"C:\\Pic"));
    EXPECT_FALSE(IsWithin(L"C:\\Pics", L"C:\\Pics"));
    EXPECT_TRUE(IsWithin(L"C:\\Pics", L"C:\\PICS", true));
    EXPECT_TRUE(IsWithin(L"C:\\x.png", L"C:\\"));
    EXPECT_TRUE(IsWithin(L"C:\\Pics/b.png", L"C:\\Pics"));
    EXPECT_FALSE(IsWithin(L"C:\\Pics\\b.png", L""));
}

// ---- FileIo.h ------------------------------------------------------------------------------------

TEST(FileIo, RoundTripsBytes)
{
    TempDir dir;
    const auto path = dir / L"data.bin";
    std::string bytes(70'000, '\0');
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>(i * 7);
    std::wstring error;
    ASSERT_TRUE(WriteFileBytes(path, bytes, WriteMode::New, &error)) << Narrow(error);
    std::string back;
    ASSERT_TRUE(ReadFileBytes(path, back, &error)) << Narrow(error);
    EXPECT_EQ(back, bytes);
    std::vector<uint8_t> raw;
    ASSERT_TRUE(ReadFileBytes(path, raw));
    EXPECT_EQ(raw.size(), bytes.size());

    // Existing replaces the contents, shorter included; Any creates or replaces.
    ASSERT_TRUE(WriteFileBytes(path, std::string_view("short"), WriteMode::Existing));
    EXPECT_EQ(ReadAll(path), "short");
    const uint8_t two[] = { 1, 2 };
    ASSERT_TRUE(WriteFileBytes(path, std::span<const uint8_t>(two), WriteMode::Any));
    EXPECT_EQ(ReadAll(path), std::string("\x01\x02"));
    ASSERT_TRUE(WriteFileBytes(dir / L"other.bin", std::string_view("x"), WriteMode::Any));
    EXPECT_EQ(ReadAll(dir / L"other.bin"), "x");
}

TEST(FileIo, NeverReplacesOrCreatesByMistake)
{
    TempDir dir;
    const auto path = dir / L"taken.txt";
    WriteText(path, "keep");
    std::wstring error;
    EXPECT_FALSE(WriteFileBytes(path, std::string_view("new"), WriteMode::New, &error));
    EXPECT_EQ(error, std::format(L"cannot write taken.txt (error {})", ERROR_FILE_EXISTS));
    EXPECT_EQ(ReadAll(path), "keep");
    EXPECT_FALSE(WriteFileBytes(dir / L"missing.txt", std::string_view("x"), WriteMode::Existing));
    EXPECT_FALSE(PathExists(dir / L"missing.txt"));
}

// ---- ErrorText.h ---------------------------------------------------------------------------------

// ---- TempPath.h ----------------------------------------------------------------------------------

TEST(TempPath, NamesAndFolders)
{
    const auto temp = TempDirectory();
    ASSERT_FALSE(temp.empty());
    EXPECT_EQ(temp.back(), L'\\');
    const auto a = TempFilePath(L"office", L".pdf");
    const auto b = TempFilePath(L"office", L".pdf");
    EXPECT_TRUE(a.starts_with(temp + SD_NAME L"-office-")) << Narrow(a);
    EXPECT_TRUE(a.ends_with(L".pdf"));
    EXPECT_NE(a, b);
    EXPECT_FALSE(PathExists(a));   // a name only

    std::wstring kept;
    {
        TempFolder folder;
        ASSERT_TRUE(folder.Ok());
        kept = folder.Path();
        EXPECT_TRUE(IsFolder(kept));
        WriteText(folder.File(L"x.txt"), "x");
        EXPECT_TRUE(IsFile(kept + L"\\x.txt"));
    }
    EXPECT_FALSE(PathExists(kept));
}

// ---- FormatText.h --------------------------------------------------------------------------------

TEST(FormatText, Durations)
{
    EXPECT_EQ(DurationText(0), L"1 s");
    EXPECT_EQ(DurationText(38'000), L"38 s");
    EXPECT_EQ(DurationText(252'000), L"4 min 12 s");
    EXPECT_EQ(DurationText(120'000), L"2 min");
    EXPECT_EQ(DurationText(3'900'000), L"1 h 5 min");
    EXPECT_EQ(DurationText(7'200'000), L"2 h");
}

TEST(FormatText, Sizes)
{
    EXPECT_EQ(SizeText(0), L"0 bytes");
    EXPECT_EQ(SizeText(1), L"1 byte");
    EXPECT_EQ(SizeText(12), L"12 bytes");
    EXPECT_EQ(SizeText(512), L"512 bytes");
    EXPECT_EQ(SizeText(820 * 1024), L"820 KB");
    EXPECT_EQ(SizeText(860 * 1024), L"860 KB");
    EXPECT_EQ(SizeText(1000 * 1024), L"1.0 MB");   // never "1000 KB"
    EXPECT_EQ(SizeText(1258291), L"1.2 MB");
    EXPECT_EQ(SizeText(1468006), L"1.4 MB");
    EXPECT_EQ(SizeText(48ull * 1024 * 1024), L"48 MB");
    EXPECT_EQ(SizeText(250ull * 1024 * 1024), L"250 MB");
    EXPECT_EQ(SizeText(3ull * 1024 * 1024 * 1024), L"3.0 GB");
    EXPECT_EQ(ExactSizeText(512), L"512 bytes");
    EXPECT_EQ(ExactSizeText(3456789), L"3.3 MB (3,456,789 bytes)");
}

TEST(FormatText, CountsAndTimes)
{
    EXPECT_EQ(Thousands(0), L"0");
    EXPECT_EQ(Thousands(999), L"999");
    EXPECT_EQ(Thousands(20000), L"20,000");
    EXPECT_EQ(Thousands(1234567), L"1,234,567");
    EXPECT_EQ(Timecode(83'456'400, false), L"0:01:23.456");
    EXPECT_EQ(Timecode(83'456'600, true), L"00:01:23.457");
    EXPECT_EQ(Timecode(-5, true), L"00:00:00.000");
    EXPECT_EQ(Timecode(36'000'000'000, false), L"10:00:00.000");
}

// ---- OutputPath.h / StagedOutput.h ---------------------------------------------------------------

TEST(OutputPath, StagingNameAndFreeRename)
{
    const auto name = StagingName(L"photo");
    EXPECT_TRUE(name.starts_with(L"~photo.")) << Narrow(name);
    EXPECT_TRUE(name.ends_with(L".sdpart"));
    EXPECT_EQ(name.size(), std::wstring(L"~photo.12345678.sdpart").size());

    TempDir dir;
    WriteText(dir / L"photo (converted).png", "taken");
    const auto staging = dir / name;
    WriteText(staging, "new");
    std::wstring final;
    ASSERT_EQ(RenameToFreeName(staging, OutputNaming::Parentheses, L"photo", L"converted", L".png", final), S_OK);
    EXPECT_EQ(final, dir / L"photo (converted 2).png");
    EXPECT_EQ(ReadAll(final), "new");
    EXPECT_EQ(ReadAll(dir / L"photo (converted).png"), "taken");
    EXPECT_FALSE(PathExists(staging));
}

TEST(StagedOutput, RecordFailureAndDone)
{
    TempDir dir;
    Recipes recipes(dir);
    std::wstring error;
    auto history = HistoryStore::Open(dir / L"history.db", error);
    ASSERT_TRUE(history) << Narrow(error);
    const auto run = history->BeginRun(recipes.Get("to-webm"), 3);
    const auto failed = RecordFailure(history.get(), run, L"C:\\a.avi", L"cannot read");
    EXPECT_FALSE(failed.ok);
    EXPECT_FALSE(failed.skipped);
    EXPECT_EQ(failed.error, L"cannot read");
    const auto skipped = RecordFailure(history.get(), run, L"C:\\b.avi", L"cancelled", true);
    EXPECT_TRUE(skipped.skipped);
    RecordDone(history.get(), run, L"C:\\c.avi");
    const auto items = history->Items(run);
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].source, L"C:\\a.avi");
    EXPECT_EQ(items[0].status, status::Failed);
    EXPECT_EQ(items[0].error, L"cannot read");
    EXPECT_EQ(items[1].status, status::Skipped);
    EXPECT_EQ(items[2].status, status::Done);
    EXPECT_EQ(items[2].output, L"");
    // No history (a preview run): still the result.
    EXPECT_EQ(RecordFailure(nullptr, 0, L"C:\\d.avi", L"x").error, L"x");
}

// ---- Recipe.h param helpers ----------------------------------------------------------------------

TEST(Recipe, ParamReaders)
{
    const nlohmann::json j = { { "speed", 3.5 }, { "name", "x" }, { "big", 1e9 } };
    EXPECT_EQ(JsonNumber(j, "speed", 1), 3.5);
    EXPECT_EQ(JsonNumber(j, "name", 1), 1);   // not a number
    EXPECT_EQ(JsonNumber(j, "missing", 2), 2);
    EXPECT_EQ(JsonNumber(j, "big", 1, 0.25, 4), 4);
    EXPECT_EQ(JsonNumber(j, "speed", 1, 0.25, 2), 2);
    EXPECT_EQ(JsonNumber(nlohmann::json{ { "v", -1 } }, "v", 1, 0, 10), 0);
    EXPECT_EQ(JsonNumber(nlohmann::json{ { "v", std::numeric_limits<double>::infinity() } }, "v", 1, 0, 10), 1);
    EXPECT_EQ(JsonNumber(nlohmann::json{ { "v", std::nan("") } }, "v", 7, 0, 10), 7);

    Recipe r;
    EXPECT_EQ(r.FirstParam("format"), "");
    r.steps.push_back({ "docs.convert", nlohmann::json{ { "format", "pdf" } } });
    EXPECT_EQ(r.FirstParam("format"), "pdf");
    EXPECT_EQ(r.FirstParam("missing"), "");

    const nlohmann::json params = { { "start", 1 }, { "perFile", { { "C:\\a.mp4", { { "start", 5 } } } } } };
    EXPECT_EQ(ParamsForFile(params, L"C:\\a.mp4"), (nlohmann::json{ { "start", 5 } }));
    EXPECT_EQ(ParamsForFile(params, L"C:\\b.mp4"), (nlohmann::json{ { "start", 1 } }));

    const auto list = nlohmann::json::parse(R"([{"id":"a","steps":[{"action":"video.trim"}]},
                                                {"id":"b","steps":[{"action":"video.speed"}]},
                                                {"id":"c","steps":[]}])");
    EXPECT_EQ(FindToolRecipe(list, "video.speed").value("id", ""), "b");
    EXPECT_TRUE(FindToolRecipe(list, "video.crop").is_null());
}
