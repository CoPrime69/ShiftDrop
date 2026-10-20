#include "OutputPath.h"
#include "TestUtil.h"

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
