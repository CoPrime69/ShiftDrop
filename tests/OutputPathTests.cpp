#include "OutputPath.h"
#include "StagedOutput.h"
#include "TestUtil.h"
#include "preview/Prediction.h"

using namespace sd;
using namespace sd::test;

TEST(OutputPath, NamesFollowThePattern)
{
    EXPECT_EQ(OutputName(L"photo", L"converted", 1, L".png"), L"photo (converted).png");
    EXPECT_EQ(OutputName(L"photo", L"converted", 2, L".png"), L"photo (converted 2).png");
    EXPECT_EQ(OutputName(L"my.photo", L"edited", 10, L".jpg"), L"my.photo (edited 10).jpg");
}

TEST(OutputPath, LowerExtension)
{
    EXPECT_EQ(LowerExtension(L"C:\\a\\Photo.JPEG"), L".jpeg");
    EXPECT_EQ(LowerExtension(L"C:\\a.b\\noext"), L"");
    EXPECT_EQ(LowerExtension(L"archive.tar.GZ"), L".gz");
}

TEST(OutputPath, CommitNeverOverwrites)
{
    TempDir dir;
    const auto source = dir / L"a.png";
    WriteText(source, "source");
    WriteText(dir / L"a (converted).png", "earlier output");

    std::wstring staging;
    Microsoft::WRL::ComPtr<IStream> stream;
    ASSERT_HRESULT_SUCCEEDED(CreateStaging(source, staging, stream));
    EXPECT_TRUE(std::filesystem::path(staging).filename().wstring().starts_with(L"~a."));
    EXPECT_EQ(std::filesystem::path(staging).extension(), L".sdpart");
    ULONG written = 0;
    ASSERT_HRESULT_SUCCEEDED(stream->Write("new", 3, &written));
    stream.Reset();

    std::wstring final;
    ASSERT_HRESULT_SUCCEEDED(CommitStaging(staging, source, L"converted", L".png", final));
    EXPECT_EQ(std::filesystem::path(final).filename(), L"a (converted 2).png");
    EXPECT_EQ(ReadAll(dir / L"a (converted).png"), "earlier output");
    EXPECT_EQ(ReadAll(final), "new");
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
}

namespace
{
    // Sets the naming style for one test and puts the default back afterwards.
    struct NamingScope
    {
        explicit NamingScope(OutputNaming naming) { SetOutputNaming(naming); }
        ~NamingScope() { SetOutputNaming(OutputNaming::Parentheses); }
    };

    // Writes `text` through a staging file and commits it as an output of `source`.
    std::wstring Commit(const std::wstring& source, const char* text)
    {
        std::wstring staging, final;
        Microsoft::WRL::ComPtr<IStream> stream;
        if (FAILED(CreateStaging(source, staging, stream)))
            return {};
        ULONG written = 0;
        stream->Write(text, static_cast<ULONG>(strlen(text)), &written);
        stream.Reset();
        if (FAILED(CommitStaging(staging, source, L"converted", L".png", final)))
            return {};
        return final;
    }
}

TEST(OutputPath, EveryNamingStyle)
{
    EXPECT_EQ(OutputName(OutputNaming::Parentheses, L"photo", L"converted", 1, L".png"), L"photo (converted).png");
    EXPECT_EQ(OutputName(OutputNaming::Dash, L"photo", L"converted", 1, L".png"), L"photo-converted.png");
    EXPECT_EQ(OutputName(OutputNaming::Dash, L"photo", L"converted", 3, L".png"), L"photo-converted-3.png");
    EXPECT_EQ(OutputName(OutputNaming::Underscore, L"photo", L"converted", 1, L".png"), L"photo_converted.png");
    EXPECT_EQ(OutputName(OutputNaming::Underscore, L"photo", L"converted", 2, L".png"), L"photo_converted_2.png");
    EXPECT_EQ(OutputName(OutputNaming::Subfolder, L"photo", L"converted", 1, L".png"), L"photo.png");
    EXPECT_EQ(OutputName(OutputNaming::Subfolder, L"photo", L"converted", 2, L".png"), L"photo (2).png");
    // A suffix with a space takes the style's separator; a folder output has no extension.
    EXPECT_EQ(OutputName(OutputNaming::Dash, L"song", L"no metadata", 1, L".mp3"), L"song-no-metadata.mp3");
    EXPECT_EQ(OutputName(OutputNaming::Underscore, L"clip", L"frames", 1, L""), L"clip_frames");
    // No suffix (a zip of several items): the name alone, then numbered in the style's way.
    EXPECT_EQ(OutputName(OutputNaming::Parentheses, L"Archive", L"", 1, L".zip"), L"Archive.zip");
    EXPECT_EQ(OutputName(OutputNaming::Parentheses, L"Archive", L"", 2, L".zip"), L"Archive (2).zip");
    EXPECT_EQ(OutputName(OutputNaming::Dash, L"Archive", L"", 3, L".zip"), L"Archive-3.zip");
    EXPECT_EQ(OutputName(OutputNaming::Underscore, L"Archive", L"", 2, L".7z"), L"Archive_2.7z");
    EXPECT_EQ(OutputName(OutputNaming::Subfolder, L"Archive", L"", 2, L".zip"), L"Archive (2).zip");
    // The current style is the default one until Settings changes it.
    EXPECT_EQ(CurrentOutputNaming(), OutputNaming::Parentheses);
    EXPECT_EQ(OutputName(L"photo", L"edited", 1, L".jpg"), L"photo (edited).jpg");
}

TEST(OutputPath, DashAndUnderscoreNeverOverwrite)
{
    TempDir dir;
    const auto source = dir / L"a.png";
    WriteText(source, "source");
    for (const auto naming : { OutputNaming::Dash, OutputNaming::Underscore })
    {
        NamingScope scope(naming);
        const wchar_t sep = naming == OutputNaming::Dash ? L'-' : L'_';
        const auto taken = dir / std::format(L"a{}converted.png", sep);
        WriteText(taken, "earlier output");
        const auto final = Commit(source, "new");
        EXPECT_EQ(std::filesystem::path(final).filename(), std::format(L"a{0}converted{0}2.png", sep));
        EXPECT_EQ(ReadAll(taken), "earlier output");
        EXPECT_EQ(ReadAll(final), "new");
    }
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
}

TEST(OutputPath, SubfolderStyleKeepsTheName)
{
    TempDir dir;
    const auto source = dir / L"a.png";
    WriteText(source, "source");
    NamingScope scope(OutputNaming::Subfolder);
    EXPECT_EQ(OutputFolder(source), dir / L"ShiftDrop");

    const auto first = Commit(source, "one");
    EXPECT_EQ(first, dir / L"ShiftDrop\\a.png");
    EXPECT_EQ(ReadAll(first), "one");
    const auto second = Commit(source, "two");   // the name is taken now: numbered, the first untouched
    EXPECT_EQ(second, dir / L"ShiftDrop\\a (2).png");
    EXPECT_EQ(ReadAll(first), "one");
    EXPECT_EQ(ReadAll(source), "source");
    EXPECT_EQ(preview::PredictOutputName(source, L"converted", L".png"), L"ShiftDrop\\a (3).png");

    // A file already in a ShiftDrop folder gets no ShiftDrop folder inside it: it is numbered next to itself.
    EXPECT_EQ(OutputFolder(first), dir / L"ShiftDrop");
    EXPECT_EQ(Commit(first, "three"), dir / L"ShiftDrop\\a (3).png");
    EXPECT_EQ(ReadAll(first), "one");
}

TEST(OutputPath, SubfolderStyleNeverReplacesAFileNamedShiftDrop)
{
    TempDir dir;
    const auto source = dir / L"a.png";
    WriteText(source, "source");
    WriteText(dir / L"ShiftDrop", "a file, not a folder");
    NamingScope scope(OutputNaming::Subfolder);
    std::wstring staging;
    Microsoft::WRL::ComPtr<IStream> stream;
    EXPECT_HRESULT_FAILED(CreateStaging(source, staging, stream));
    EXPECT_EQ(ReadAll(dir / L"ShiftDrop"), "a file, not a folder");
}

TEST(OutputPath, SubfolderStyleLeavesNoEmptyFolderAfterAFailure)
{
    TempDir dir;
    const auto source = dir / L"a.png";
    WriteText(source, "source");
    NamingScope scope(OutputNaming::Subfolder);
    std::wstring staging;
    Microsoft::WRL::ComPtr<IStream> stream;
    ASSERT_HRESULT_SUCCEEDED(CreateStaging(source, staging, stream));
    stream.Reset();
    EXPECT_TRUE(std::filesystem::is_directory(dir / L"ShiftDrop"));
    EXPECT_TRUE(RemoveStaging(staging));   // what a failed job does with its staging file
    EXPECT_FALSE(std::filesystem::exists(dir / L"ShiftDrop"));
}
