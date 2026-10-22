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

TEST(Version, DisplayIsTheBaseOrTheBasePlusWhereItCameFrom)
{
    const std::wstring base = sd::version::Base();
    const std::wstring display = sd::version::Display();
    const std::wstring commit = sd::version::Commit();
    if (display == base)
    {
        EXPECT_TRUE(commit.empty());   // the release build
        return;
    }
    if (commit.empty())
    {
        EXPECT_EQ(display, base + L"+dev");   // built outside a git checkout
        return;
    }
    EXPECT_EQ(display, base + L"+" + commit);
    ASSERT_EQ(commit.size(), 7u);
    for (wchar_t c : commit)
        EXPECT_TRUE((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')) << display;
}
