#include "toast/ToastXml.h"

#include <gtest/gtest.h>

#include <format>

using namespace sd::toast;

TEST(Toast, ArgumentsRoundTrip)
{
    const std::vector<std::wstring> paths{ L"C:\\Photos\\a (converted).png", L"\\\\nas\\share\\b & c.png" };
    const auto reveal = Arguments(Verb::Reveal, paths);
    EXPECT_EQ(reveal, L"reveal|C:\\Photos\\a (converted).png|\\\\nas\\share\\b & c.png");
    const auto back = ParseArguments(reveal);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->verb, Verb::Reveal);
    EXPECT_EQ(back->paths, paths);

    const auto open = ParseArguments(Arguments(Verb::Open, paths));   // Open takes the first only
    ASSERT_TRUE(open);
    EXPECT_EQ(open->verb, Verb::Open);
    ASSERT_EQ(open->paths.size(), 1u);
    EXPECT_EQ(open->paths[0], paths[0]);
}

TEST(Toast, ArgumentsStayWithinTheBudget)
{
    std::vector<std::wstring> paths;
    for (int i = 0; i < 100; ++i)
        paths.push_back(std::format(L"C:\\Users\\me\\Pictures\\Holiday\\photo {:03} (converted).webp", i));
    const auto args = Arguments(Verb::Reveal, paths);
    EXPECT_LE(args.size(), kArgumentBudget);
    const auto back = ParseArguments(args);
    ASSERT_TRUE(back);
    EXPECT_GT(back->paths.size(), 5u);
    EXPECT_LT(back->paths.size(), paths.size());
    EXPECT_EQ(back->paths[0], paths[0]);
    // A first path longer than the budget is still kept.
    const std::wstring longPath = L"C:\\" + std::wstring(2000, L'x');
    EXPECT_EQ(ParseArguments(Arguments(Verb::Reveal, { longPath, L"C:\\b" }))->paths, std::vector<std::wstring>{ longPath });
}

TEST(Toast, ParseRefusesAnythingElse)
{
    for (const wchar_t* bad : { L"", L"reveal", L"reveal|", L"open", L"delete|C:\\a.txt", L"reveal|relative\\a.txt", L"open|C:\\a|C:\\b",
                                L"reveal|C:\\a||C:\\b", L"REVEAL|C:\\a", L"open|http://example.com/x" })
        EXPECT_FALSE(ParseArguments(bad)) << testing::PrintToString(std::wstring(bad));
}

TEST(Toast, EscapesXml)
{
    EXPECT_EQ(EscapeXml(L"a & b <c> \"d\" 'e'"), L"a &amp; b &lt;c&gt; &quot;d&quot; &apos;e&apos;");
    EXPECT_EQ(EscapeXml(L"one\ntwo\x01\tthree"), L"one&#10;two\tthree");
}

TEST(Toast, XmlHasTheButtons)
{
    const auto one = BuildXml({ L"1 file converted to PNG", L"Saved next to the original", { L"C:\\a\\b & c.png" } });
    EXPECT_NE(one.find(L"<toast launch=\"reveal|C:\\a\\b &amp; c.png\" activationType=\"foreground\">"), std::wstring::npos) << one;
    EXPECT_NE(one.find(L"<text>1 file converted to PNG</text><text>Saved next to the original</text>"), std::wstring::npos);
    EXPECT_NE(one.find(L"<action content=\"Open folder\" arguments=\"reveal|C:\\a\\b &amp; c.png\""), std::wstring::npos);
    EXPECT_NE(one.find(L"<action content=\"Open\" arguments=\"open|C:\\a\\b &amp; c.png\""), std::wstring::npos);
    EXPECT_TRUE(one.ends_with(L"</actions></toast>"));

    // Several outputs: Open folder selects them all; there is no single file to open.
    const auto many = BuildXml({ L"2 files converted", L"", { L"C:\\a\\1.png", L"C:\\a\\2.png" } });
    EXPECT_NE(many.find(L"arguments=\"reveal|C:\\a\\1.png|C:\\a\\2.png\""), std::wstring::npos);
    EXPECT_EQ(many.find(L"content=\"Open\""), std::wstring::npos);
    EXPECT_EQ(many.find(L"<text></text>"), std::wstring::npos);

    // Nothing written (Copy text, or every file failed): a plain toast.
    const auto none = BuildXml({ L"Copy: failed", L"a.png: <unreadable>", {} });
    EXPECT_TRUE(none.starts_with(L"<toast><visual>"));
    EXPECT_EQ(none.find(L"<actions>"), std::wstring::npos);
    EXPECT_NE(none.find(L"a.png: &lt;unreadable&gt;"), std::wstring::npos);
}

TEST(Toast, AFailedToastOnlyDismisses)
{
    Content content{ L"Couldn\u2019t convert scan-07.png", L"The file is damaged or isn\u2019t really a PNG.", {} };
    content.failed = true;
    const auto xml = BuildXml(content);
    EXPECT_NE(xml.find(L"<action content=\"Dismiss\" arguments=\"dismiss\" activationType=\"system\"/>"), std::wstring::npos) << xml;
    EXPECT_EQ(xml.find(L"content=\"Open"), std::wstring::npos);
}

TEST(Toast, SummaryNamesTheFirstOutputAndWhere)
{
    EXPECT_EQ(Summary({ L"photo (converted).webp", L"b.webp", L"c.webp" }, L"next to the originals"),
              L"photo (converted).webp and 2 more, next to the originals.");
    EXPECT_EQ(Summary({ L"a.png" }, L"to Downloads"), L"a.png, to Downloads.");
    EXPECT_EQ(Summary({}, L"anywhere"), L"");
}

TEST(Toast, WhenABatchGetsAToast)
{
    using namespace std::chrono_literals;
    // A later batch owns the top panel: always a toast, however short (as before).
    EXPECT_TRUE(ShouldNotify(false, 200ms, false));
    EXPECT_TRUE(ShouldNotify(false, 30s, false));
    EXPECT_TRUE(ShouldNotify(false, 1s, true));
    // Its own panel showed the result: a toast only once the job took over 10 seconds.
    EXPECT_FALSE(ShouldNotify(true, 0ms, false));
    EXPECT_FALSE(ShouldNotify(true, 2s, false));
    EXPECT_FALSE(ShouldNotify(true, kLongJob, false));   // "over" 10 seconds
    EXPECT_TRUE(ShouldNotify(true, kLongJob + 1ms, false));
    EXPECT_TRUE(ShouldNotify(true, 5min, false));
    // Cancelled from its panel: the user was there.
    EXPECT_FALSE(ShouldNotify(true, 5min, true));
}

// An update's toast: a click (or "Update now…") brings back "update" and nothing else; Later dismisses.
TEST(Toast, AnUpdateToastOffersUpdateNow)
{
    Content content{ L"ShiftDrop 0.7.1 is available", L"You have 0.7.0.", {} };
    content.update = true;
    const auto xml = BuildXml(content);
    EXPECT_NE(xml.find(L"<toast launch=\"update\" activationType=\"foreground\">"), std::wstring::npos) << xml;
    EXPECT_NE(xml.find(L"<action content=\"Update now\u2026\" arguments=\"update\" activationType=\"foreground\"/>"), std::wstring::npos) << xml;
    EXPECT_NE(xml.find(L"<action content=\"Later\" arguments=\"dismiss\" activationType=\"system\"/>"), std::wstring::npos) << xml;
    EXPECT_EQ(Arguments(Verb::Update, { L"C:\\a.png" }), L"update");
    const auto click = ParseArguments(L"update");
    ASSERT_TRUE(click);
    EXPECT_EQ(click->verb, Verb::Update);
    EXPECT_TRUE(click->paths.empty());
    EXPECT_FALSE(ParseArguments(L"update|C:\\a.png"));
}
