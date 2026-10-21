#include "Clipboard.h"
#include "FileActions.h"

#include <shellapi.h>
#include <shlobj.h>

#include <gtest/gtest.h>

using namespace sd;

TEST(Terminal, WindowsTerminalFallsBackToPowerShell)
{
    const auto t = TerminalCommands(TerminalChoice::WindowsTerminal, L"", L"C:\\Users\\me\\My Photos");
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0].exe, L"wt.exe");
    EXPECT_EQ(t[0].args, L"-d \"C:\\Users\\me\\My Photos\"");
    EXPECT_EQ(t[0].folder, L"C:\\Users\\me\\My Photos");
    EXPECT_EQ(t[1].exe, L"pwsh.exe");
    EXPECT_EQ(t[2].exe, L"powershell.exe");
    EXPECT_EQ(t[2].args, L"-NoExit");
    // A drive's root: the backslash must not escape the closing quote.
    EXPECT_EQ(TerminalCommands(TerminalChoice::WindowsTerminal, L"", L"D:\\")[0].args, L"-d \"D:\\.\"");
}

TEST(Terminal, PowerShellAndCommandPrompt)
{
    const auto ps = TerminalCommands(TerminalChoice::PowerShell, L"", L"C:\\work");
    ASSERT_EQ(ps.size(), 2u);
    EXPECT_EQ(ps[0].exe, L"pwsh.exe");
    EXPECT_EQ(ps[1].exe, L"powershell.exe");
    EXPECT_EQ(ps[1].folder, L"C:\\work");
    const auto cmd = TerminalCommands(TerminalChoice::CommandPrompt, L"", L"C:\\work");
    ASSERT_EQ(cmd.size(), 1u);
    EXPECT_EQ(cmd[0].exe, L"cmd.exe");
    EXPECT_EQ(cmd[0].args, L"/K");
    EXPECT_EQ(cmd[0].folder, L"C:\\work");
}

TEST(Terminal, CustomCommandGetsTheFolder)
{
    const std::wstring folder = L"C:\\My Files";
    // An unquoted placeholder is quoted for the folder's spaces; a quoted one is left as written.
    auto t = TerminalCommands(TerminalChoice::Custom, L"alacritty --working-directory {folder}", folder);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].exe, L"alacritty");
    EXPECT_EQ(t[0].args, L"--working-directory \"C:\\My Files\"");
    EXPECT_EQ(t[0].folder, folder);
    t = TerminalCommands(TerminalChoice::Custom, L"  \"C:\\Program Files\\Term\\term.exe\" -d \"{folder}\" --new  ", folder);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].exe, L"C:\\Program Files\\Term\\term.exe");
    EXPECT_EQ(t[0].args, L"-d \"C:\\My Files\" --new");
    // No placeholder: the program starts in the folder. Twice: both are replaced.
    t = TerminalCommands(TerminalChoice::Custom, L"nu.exe", folder);
    EXPECT_EQ(t[0].exe, L"nu.exe");
    EXPECT_EQ(t[0].args, L"");
    EXPECT_EQ(t[0].folder, folder);
    t = TerminalCommands(TerminalChoice::Custom, L"x {folder} {folder}", L"E:\\");
    EXPECT_EQ(t[0].args, L"\"E:\\.\" \"E:\\.\"");
}

TEST(Terminal, EmptyCustomCommandMeansWindowsTerminal)
{
    for (const wchar_t* command : { L"", L"   ", L"\"\"" })
    {
        const auto t = TerminalCommands(TerminalChoice::Custom, command, L"C:\\work");
        ASSERT_FALSE(t.empty()) << command;
        EXPECT_EQ(t[0].exe, L"wt.exe") << command;
    }
}

namespace
{
    // The clipboard's block in `format`, copied out (empty when there is none).
    std::vector<uint8_t> ClipboardBytes(UINT format)
    {
        std::vector<uint8_t> bytes;
        if (!OpenClipboard(nullptr))
            return bytes;
        if (HANDLE h = GetClipboardData(format))
            if (const auto* p = static_cast<const uint8_t*>(GlobalLock(h)))
            {
                bytes.assign(p, p + GlobalSize(h));
                GlobalUnlock(h);
            }
        CloseClipboard();
        return bytes;
    }
}

TEST(Clipboard, TextReplacesWhatWasThere)
{
    std::wstring error;
    ASSERT_TRUE(clipboard::SetText(nullptr, L"first", &error));
    ASSERT_TRUE(clipboard::SetText(nullptr, L"\"C:\\a b\\photo.png\"", &error));
    const auto bytes = ClipboardBytes(CF_UNICODETEXT);
    ASSERT_GE(bytes.size(), sizeof(wchar_t));
    EXPECT_EQ(std::wstring(reinterpret_cast<const wchar_t*>(bytes.data())), L"\"C:\\a b\\photo.png\"");
}

TEST(Clipboard, FilesAsExplorerCopiesThemWithAPicture)
{
    const std::vector<std::wstring> files{ L"C:\\a\\one.png", L"D:\\two words.txt" };
    const std::vector<uint8_t> png{ 0x89, 'P', 'N', 'G' }, dib(48, 7);
    ASSERT_TRUE(clipboard::SetFiles(nullptr, files, DROPEFFECT_COPY, png, dib));

    ASSERT_TRUE(OpenClipboard(nullptr));
    std::vector<std::wstring> listed;
    if (const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP)))
        for (UINT i = 0, n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0); i < n; ++i)
        {
            wchar_t path[MAX_PATH]{};
            DragQueryFileW(drop, i, path, MAX_PATH);
            listed.push_back(path);
        }
    CloseClipboard();
    EXPECT_EQ(listed, files);

    const auto effect = ClipboardBytes(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    ASSERT_GE(effect.size(), sizeof(DWORD));
    EXPECT_EQ(*reinterpret_cast<const DWORD*>(effect.data()), static_cast<DWORD>(DROPEFFECT_COPY));
    auto pngBack = ClipboardBytes(RegisterClipboardFormatW(L"PNG"));
    pngBack.resize(std::min(pngBack.size(), png.size()));   // GlobalSize may round up
    EXPECT_EQ(pngBack, png);
    EXPECT_GE(ClipboardBytes(CF_DIB).size(), dib.size());

    // Without a picture, only the files: no stale picture from before.
    ASSERT_TRUE(clipboard::SetFiles(nullptr, files, DROPEFFECT_COPY));
    EXPECT_TRUE(ClipboardBytes(RegisterClipboardFormatW(L"PNG")).empty());
}
