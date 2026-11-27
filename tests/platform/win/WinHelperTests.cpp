// The helper tests that need Windows: Win32 file data, environment variables set through Win32,
// GetLastError, and Windows' own error codes and texts. The others are in tests/HelperTests.cpp.
#include "ErrorText.h"
#include "FileIo.h"
#include "PathUtil.h"
#include "TestUtil.h"

#include <windows.h>
#include <shobjidl.h>

#include <format>
#include <string>

using namespace sd;
using namespace sd::test;

TEST(PathUtil, FilesAndFolders)
{
    TempDir dir;
    const auto file = dir / L"a.txt";
    WriteText(file, "hello");
    const auto folder = dir.path().wstring();
    const auto missing = dir / L"missing";
    EXPECT_TRUE(PathExists(file));
    EXPECT_TRUE(PathExists(folder));
    EXPECT_FALSE(PathExists(missing));
    EXPECT_TRUE(IsFile(file));
    EXPECT_FALSE(IsFile(folder));
    EXPECT_TRUE(IsFolder(folder));
    EXPECT_FALSE(IsFolder(file));
    // INVALID_FILE_ATTRIBUTES has every bit set, the folder bit too: a missing path is not a folder.
    EXPECT_FALSE(IsFolder(missing));
    EXPECT_FALSE(IsFile(missing));
    EXPECT_EQ(FileSize(file), 5u);
    EXPECT_EQ(FileSize(missing), 0u);

    WIN32_FILE_ATTRIBUTE_DATA data{};
    data.nFileSizeHigh = 2;
    data.nFileSizeLow = 5;
    EXPECT_EQ(FileSizeOf(data), (2ull << 32) | 5);
}

TEST(PathUtil, ThisExeAndEngineExe)
{
    const auto self = ThisExe();
    ASSERT_FALSE(self.empty());
    EXPECT_TRUE(IsFile(self));

    constexpr wchar_t kVar[] = L"SHIFTDROP_HELPERTESTS_ENGINE";
    SetEnvironmentVariableW(kVar, self.c_str());
    EXPECT_EQ(EngineExe(kVar, L"none.exe"), self);
    TempDir dir;
    SetEnvironmentVariableW(kVar, (dir / L"gone.exe").c_str());
    EXPECT_EQ(EngineExe(kVar, L"none.exe"), L"");   // a path that is not there is not used
    SetEnvironmentVariableW(kVar, nullptr);
    EXPECT_EQ(EngineExe(kVar, L"no-such-engine.exe"), L"");
}

TEST(FileIo, ReadFailuresSayWhy)
{
    TempDir dir;
    const auto path = dir / L"big.bin";
    WriteText(path, std::string(4096, 'x'));
    std::string bytes;
    std::wstring error;
    EXPECT_FALSE(ReadFileBytes(path, bytes, &error, 1024));
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_FILE_TOO_LARGE));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(ReadFileBytes(path, bytes, &error, 4096));

    const auto missing = dir / L"missing.bin";
    EXPECT_FALSE(ReadFileBytes(missing, bytes, &error));
    EXPECT_EQ(error, std::format(L"cannot open {} (error {})", missing, ERROR_FILE_NOT_FOUND));
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_FILE_NOT_FOUND));
}

TEST(ErrorText, Codes)
{
    EXPECT_EQ(HexCode(E_ACCESSDENIED), L"0x80070005");
    EXPECT_EQ(HexCode(S_OK), L"0x00000000");
    EXPECT_TRUE(HResultText(E_ACCESSDENIED).starts_with(L"0x80070005"));
    EXPECT_EQ(HResultMessage(HRESULT_FROM_WIN32(ERROR_CANCELLED)), L"cancelled");
    EXPECT_EQ(HResultMessage(COPYENGINE_E_USER_CANCELLED), L"cancelled");
    const auto denied = HResultMessage(E_ACCESSDENIED);
    EXPECT_FALSE(denied.empty());
    EXPECT_NE(denied.back(), L'.');
    EXPECT_NE(denied.back(), L'\n');
    EXPECT_EQ(HResultMessage(static_cast<HRESULT>(0xA0001234)), L"error 0xA0001234");   // no system text
    EXPECT_EQ(TrimMessage(L"Access is denied.\r\n "), L"Access is denied.");
}
