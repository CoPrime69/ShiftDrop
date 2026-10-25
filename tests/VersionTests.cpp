#include "AppVersion.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>

// ---- AppVersion.h (Version.h, generated from VERSION) ---------------------------------------------
// Only AppVersion.cpp includes the generated header, so this file isn't recompiled for every commit.

TEST(Version, BaseIsWhatVersionFileSays)
{
    std::ifstream in(SHIFTDROP_VERSION_FILE);
    ASSERT_TRUE(in) << SHIFTDROP_VERSION_FILE;
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    EXPECT_EQ(std::wstring(line.begin(), line.end()), sd::version::Base());
}

TEST(Version, BaseIsThreeNumbers)
{
    const std::wstring base = sd::version::Base();
    int dots = 0;
    bool digit = false;
    for (wchar_t c : base)
    {
        if (c == L'.')
        {
            EXPECT_TRUE(digit) << base;
            ++dots;
            digit = false;
        }
        else
        {
            EXPECT_TRUE(c >= L'0' && c <= L'9') << base;
            digit = true;
        }
    }
    EXPECT_EQ(dots, 2) << base;
    EXPECT_TRUE(digit) << base;
}

// The release build shows VERSION; any other build a pre-release of the next version,
// "<VERSION or the patch after it>-dev.<Build()>+<commit or dev>" (scripts/make-version-header.cmake).
TEST(Version, DisplayIsTheReleaseOrAPreReleaseOfTheNext)
{
    const std::wstring base = sd::version::Base();
    const std::wstring display = sd::version::Display();
    const std::wstring commit = sd::version::Commit();
    if (display == base)
    {
        EXPECT_TRUE(commit.empty());   // the release build
        EXPECT_EQ(sd::version::Build(), 0u);
        return;
    }
    const size_t dash = display.find(L'-'), plus = display.find(L'+');
    ASSERT_NE(dash, std::wstring::npos) << display;
    ASSERT_NE(plus, std::wstring::npos) << display;
    ASSERT_LT(dash, plus) << display;

    // The next version: VERSION (not released yet) or its next patch (VERSION is released).
    const std::wstring next = display.substr(0, dash);
    const size_t lastDot = base.rfind(L'.');
    const std::wstring nextPatch = base.substr(0, lastDot + 1) + std::to_wstring(std::stoi(base.substr(lastDot + 1)) + 1);
    EXPECT_TRUE(next == base || next == nextPatch) << display;

    EXPECT_EQ(display.substr(dash + 1, plus - dash - 1), L"dev." + std::to_wstring(sd::version::Build())) << display;

    const std::wstring where = display.substr(plus + 1);
    if (commit.empty())
    {
        EXPECT_EQ(where, L"dev") << display;   // built outside a git checkout
        return;
    }
    EXPECT_EQ(where, commit);
    ASSERT_EQ(commit.size(), 7u);
    for (wchar_t c : commit)
        EXPECT_TRUE((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')) << display;
}
