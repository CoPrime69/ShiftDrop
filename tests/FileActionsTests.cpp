#include "FileActions.h"

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
