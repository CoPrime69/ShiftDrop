// The output-path tests that write through a COM stream (CreateStaging, platform/win/StagingStream.h),
// which only Windows has. The others are in tests/OutputPathTests.cpp.
#include "OutputPath.h"
#include "PathUtil.h"
#include "TestUtil.h"
#include "platform/win/StagingStream.h"
#include "preview/Prediction.h"

#include <cstring>
#include <filesystem>

using namespace sd;
using namespace sd::test;

TEST(OutputPath, CommitNeverOverwrites)
{
    TempDir dir;
    const auto source = dir / L"a.png";
    WriteText(source, "source");
    WriteText(dir / L"a (converted).png", "earlier output");

    std::wstring staging;
    Microsoft::WRL::ComPtr<IStream> stream;
    ASSERT_HRESULT_SUCCEEDED(CreateStaging(source, staging, stream));
    EXPECT_TRUE(FileName(staging).starts_with(L"~a."));
    EXPECT_EQ(std::filesystem::path(staging).extension(), L".sdpart");
    ULONG written = 0;
    ASSERT_HRESULT_SUCCEEDED(stream->Write("new", 3, &written));
    stream.Reset();

    std::wstring final;
    ASSERT_HRESULT_SUCCEEDED(CommitStaging(staging, source, L"converted", L".png", final));
    EXPECT_EQ(FileName(final), L"a (converted 2).png");
    EXPECT_EQ(ReadAll(dir / L"a (converted).png"), "earlier output");
    EXPECT_EQ(ReadAll(final), "new");
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
}

namespace
{
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
        EXPECT_EQ(FileName(final), std::format(L"a{0}converted{0}2.png", sep));
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
