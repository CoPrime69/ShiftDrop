#include "SettingsPages.h"
#include "Strings.h"
#include "TestUtil.h"
#include "WheelContents.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <set>
#include <string>

using namespace sd;
using settings::Item;
using settings::Page;

namespace
{
    // `text` names `key` whole: "[FileActions] Terminal" is not in "[FileActions] TerminalCommand".
    bool Holds(const wchar_t* text, std::wstring_view key)
    {
        if (!text)
            return false;
        const std::wstring_view t(text);
        for (size_t at = t.find(key); at != std::wstring_view::npos; at = t.find(key, at + 1))
            if (at + key.size() == t.size() || !iswalpha(t[at + key.size()]))
                return true;
        return false;
    }

    // A Convert section like the app's: photos, video, and the file actions' "Any file".
    SettingsTree::Section Convert()
    {
        SettingsTree::Section s{ Ring::Convert, L"Convert", {} };
        s.groups.push_back({ L"Photos", { { "to-png", L"PNG", L"PNG, lossless", 0 }, { "to-webp", L"WebP", L"WebP, smaller", 0 }, { "to-tiff", L"TIFF", L"", 0 } } });
        s.groups.push_back({ L"Video", { { "to-mp4", L"MP4", L"", 0 }, { "to-gif", L"GIF", L"Animated GIF", 0 } } });
        s.groups.push_back({ L"Empty", {} });
        return s;
    }
}

// Every key of settings.ini a user could set before this window had pages has exactly one card.
TEST(SettingsPages, EverySettingHasOnePlace)
{
    const wchar_t* keys[] = {
        L"[Appearance] Theme", L"[Appearance] Style",  L"[Appearance] Size",        L"[Appearance] PanelSize",
        L"[Appearance] Accent", L"[Output] Naming",    L"[Output] VirtualFiles",    L"[FileActions] Terminal",
        L"[FileActions] TerminalCommand", L"[Startup] StartWithWindows", L"[Ocr] Language", L"[Features] Convert",
        L"Off", L"shelf.json", L"HKCU\\Software\\Classes",
    };
    for (const wchar_t* key : keys)
    {
        int places = 0;
        for (const auto& e : settings::Catalogue())
            places += Holds(e.stored, key) && !(std::wstring_view(key) == L"[Appearance] Accent" && e.item == Item::WindowsAccent) ? 1 : 0;
        EXPECT_EQ(places, 1) << key;
    }
}

// What the old window had (a choice list, the accent row, the slider, the tick tree) all moved to a page.
TEST(SettingsPages, TheOldWindowsSettingsAllMoved)
{
    const std::pair<const wchar_t*, Page> moved[] = {
        { L"Settings \u203A Terminal", Page::General },      { L"Settings \u203A Command line", Page::General },
        { L"Settings \u203A Theme", Page::Appearance },      { L"Settings \u203A Accent colour", Page::Appearance },
        { L"Settings \u203A Top panel size", Page::Appearance }, { L"Settings \u203A Output names", Page::Output },
        { L"Settings \u203A the tick list", Page::Wheel },
    };
    for (const auto& [before, page] : moved)
    {
        bool found = false;
        for (const auto& e : settings::Catalogue())
            if (e.before && std::wstring_view(e.before) == before)
            {
                found = true;
                EXPECT_EQ(e.page, page);
            }
        EXPECT_TRUE(found);
    }
}

TEST(SettingsPages, CatalogueIsInPageOrderWithEveryItemOnce)
{
    std::set<int> items;
    std::set<std::wstring> titles;
    int page = 0;
    for (const auto& e : settings::Catalogue())
    {
        EXPECT_TRUE(items.insert(static_cast<int>(e.item)).second);
        EXPECT_TRUE(titles.insert(e.title).second);
        EXPECT_GE(static_cast<int>(e.page), page) << "pages out of order";
        page = static_cast<int>(e.page);
        EXPECT_EQ(&settings::Find(e.item), &e);
    }
    EXPECT_EQ(items.size(), static_cast<size_t>(settings::kItemCount));
    for (int p = 0; p < settings::kPageCount; ++p)
    {
        EXPECT_FALSE(settings::OnPage(static_cast<Page>(p)).empty());
        EXPECT_NE(settings::PageIcon(static_cast<Page>(p)), 0);
        EXPECT_GT(std::wstring_view(settings::PageTitle(static_cast<Page>(p))).size(), 0u);
    }
    // The size the wheel opens at is next to its style, the slider for the top panel after them.
    const auto look = settings::OnPage(Page::Appearance);
    ASSERT_EQ(look.size(), 6u);
    EXPECT_EQ(look[2], Item::Size);
    EXPECT_EQ(look[3], Item::PanelSize);
}

TEST(SettingsPages, FindASetting)
{
    EXPECT_TRUE(settings::Search(L"").empty());
    EXPECT_TRUE(settings::Search(L"   ").empty());
    EXPECT_EQ(settings::Search(L"dark"), std::vector<Item>{ Item::Theme });
    EXPECT_EQ(settings::Search(L"GLASS"), std::vector<Item>{ Item::Style });
    EXPECT_EQ(settings::Search(L"powershell"), std::vector<Item>{ Item::Terminal });
    const auto terminal = settings::Search(L"terminal");
    ASSERT_GE(terminal.size(), 2u);
    EXPECT_EQ(terminal[0], Item::Terminal);
    // Every word has to be there, in any order.
    EXPECT_EQ(settings::Search(L"colour windows"), (std::vector<Item>{ Item::Theme, Item::Accent, Item::WindowsAccent }));
    EXPECT_EQ(settings::Search(L"panel top"), std::vector<Item>{ Item::PanelSize });
    // A page's name finds what is on it.
    EXPECT_FALSE(settings::Search(L"advanced logs").empty());
    EXPECT_TRUE(settings::Search(L"zebra").empty());
}

namespace
{
    // The wheel's sections as Settings lists them: Convert above, and Reduce size with its photos.
    SettingsTree Tree()
    {
        SettingsTree tree{ { Convert() } };
        SettingsTree::Section reduce{ Ring::Reduce, L"Reduce size", {} };
        reduce.groups.push_back({ L"Photos", { { "reduce-50", L"50%", L"Half as heavy", 0 }, { "reduce-mb", L"Under 10 MB", L"", 0 } } });
        tree.sections.push_back(reduce);
        return tree;
    }
}

TEST(SettingsPages, FindASettingKnowsOtherWords)
{
    // Prakhar's: "reduce size" is a section of the wheel, found first, before anything that only mentions it.
    const auto tree = Tree();
    const auto reduce = settings::SearchAll(L"reduce size", tree);
    ASSERT_FALSE(reduce.empty());
    EXPECT_EQ(reduce[0].page, Page::Wheel);
    EXPECT_EQ(reduce[0].item, Item::WheelContents);
    EXPECT_EQ(reduce[0].section, static_cast<int>(Ring::Reduce));
    EXPECT_EQ(reduce[0].group, -1);
    EXPECT_EQ(reduce[0].where, L"Wheel \u203A Reduce size");
    // Its actions aren't listed only because their section is named.
    for (const auto& f : reduce)
        EXPECT_LT(f.slice, 0) << Narrow(f.where);
    EXPECT_TRUE(settings::SearchAll(L"zebra", tree).empty());
    EXPECT_TRUE(settings::SearchAll(L"  ", tree).empty());

    // A kind of file and an action, by their own names, in any case, any word order.
    const auto photos = settings::SearchAll(L"PHOTOS reduce", tree);
    ASSERT_FALSE(photos.empty());
    EXPECT_EQ(photos[0].section, static_cast<int>(Ring::Reduce));
    EXPECT_EQ(photos[0].group, 0);
    EXPECT_EQ(photos[0].slice, -1);
    const auto webp = settings::SearchAll(L"webp", tree);
    ASSERT_FALSE(webp.empty());
    EXPECT_EQ(webp[0], (settings::Found{ Item::WheelContents, Page::Wheel, static_cast<int>(Ring::Convert), 0, 1, L"Wheel \u203A Convert \u203A Photos \u203A WebP" }));
    const auto mb = settings::SearchAll(L"10 mb", tree);
    ASSERT_FALSE(mb.empty());
    EXPECT_EQ(mb[0].slice, 1);
    // Detail text counts too: "lossless" is PNG's.
    const auto lossless = settings::SearchAll(L"lossless", tree);
    ASSERT_EQ(lossless.size(), 1u);
    EXPECT_EQ(lossless[0].slice, 0);

    // Other words for the same thing.
    const auto first = [&](const wchar_t* query) {
        const auto found = settings::SearchAll(query, tree);
        return found.empty() ? std::optional<Item>{} : std::optional<Item>{ found.front().item };
    };
    EXPECT_EQ(first(L"color"), Item::Accent);
    EXPECT_EQ(first(L"accent"), Item::Accent);
    EXPECT_EQ(first(L"highlight"), Item::Accent);
    EXPECT_EQ(first(L"dark"), Item::Theme);
    EXPECT_EQ(first(L"theme"), Item::Theme);
    EXPECT_EQ(first(L"startup"), Item::StartWithWindows);
    EXPECT_EQ(first(L"start with windows"), Item::StartWithWindows);
    EXPECT_EQ(first(L"ocr"), Item::OcrLanguage);
    EXPECT_EQ(first(L"text recognition"), Item::OcrLanguage);
    EXPECT_EQ(first(L"rename"), Item::OutputNaming);
    EXPECT_EQ(first(L"naming"), Item::OutputNaming);
    EXPECT_EQ(first(L"live example"), Item::OutputNaming);
    EXPECT_EQ(first(L"size"), Item::Size);
    EXPECT_EQ(first(L"top panel size"), Item::PanelSize);
    EXPECT_EQ(first(L"context menu"), Item::ShellMenu);
    EXPECT_EQ(first(L"delete colour"), Item::Accent);
    // A page's name, and every label of a card's own controls, find it.
    EXPECT_EQ(first(L"advanced"), Item::ShellMenu);
    EXPECT_EQ(first(L"about"), Item::Version);
    EXPECT_EQ(first(L"reset to default"), Item::WheelContents);
    for (const auto& e : settings::Catalogue())
    {
        const auto all = settings::SearchAll(e.title, tree);
        EXPECT_TRUE(std::any_of(all.begin(), all.end(), [&](const settings::Found& f) { return f.item == e.item; })) << Narrow(e.title);
        for (const wchar_t* option : settings::Options(e.item))
        {
            const auto found = settings::Search(option);
            EXPECT_NE(std::find(found.begin(), found.end(), e.item), found.end()) << Narrow(option);
        }
    }
    // What is named by every word comes before what only mentions them.
    const auto windows = settings::SearchAll(L"colour windows", tree);
    ASSERT_GE(windows.size(), 3u);
    EXPECT_EQ(windows[0].item, Item::WindowsAccent);
}

TEST(SettingsPages, OtherWordsAndOptions)
{
    EXPECT_EQ(settings::Alternatives(L"colour"), (std::vector<std::wstring>{ L"colour", L"color" }));
    EXPECT_EQ(settings::Alternatives(L"color"), (std::vector<std::wstring>{ L"color", L"colour" }));
    EXPECT_EQ(settings::Alternatives(L"zebra"), std::vector<std::wstring>{ L"zebra" });
    const auto dark = settings::Alternatives(L"dark");
    EXPECT_NE(std::find(dark.begin(), dark.end(), L"theme"), dark.end());
    EXPECT_TRUE(settings::HasEveryWordOrAlternative(L"Accent colour", SearchWords(L"COLOR accent")));
    EXPECT_FALSE(settings::HasEveryWordOrAlternative(L"Accent colour", SearchWords(L"color zebra")));
    EXPECT_TRUE(settings::HasEveryWordOrAlternative(L"anything", {}));
    // The options are the window's controls, in its order.
    ASSERT_EQ(settings::Options(Item::Size).size(), 4u);
    EXPECT_EQ(std::wstring(settings::Options(Item::Size)[0]), L"Auto");
    EXPECT_EQ(settings::Options(Item::Theme).size(), 3u);
    EXPECT_EQ(settings::Options(Item::Terminal).size(), 4u);
    EXPECT_TRUE(settings::Options(Item::Version).empty());
}

TEST(WheelContents, ChipsWriteTheFeaturesStore)
{
    Features f;
    EXPECT_TRUE(contents::SliceOn(f, "to-png"));
    f = contents::WithSlice(f, "to-png", false);
    EXPECT_FALSE(contents::SliceOn(f, "to-png"));
    EXPECT_EQ(f.off, std::set<std::string>{ "to-png" });
    f = contents::WithSlice(f, "to-png", false);   // twice off is still once
    EXPECT_EQ(f.off.size(), 1u);
    f = contents::WithSlice(f, "to-png", true);
    EXPECT_TRUE(f.off.empty());
    EXPECT_EQ(f, Features{});
}

TEST(WheelContents, ASectionOffKeepsItsChips)
{
    const auto section = Convert();
    Features f = contents::WithSlice(Features{}, "to-tiff", false);
    f = contents::WithSection(f, Ring::Convert, false);
    EXPECT_FALSE(f.SectionOn(Ring::Convert));
    EXPECT_TRUE(f.off.contains("to-tiff"));
    // The wheel's own test: off with its section, on again as it was.
    Recipe png;
    png.id = "to-png";
    png.ring = "convert";
    EXPECT_FALSE(f.RecipeOn(png));
    f = contents::WithSection(f, Ring::Convert, true);
    EXPECT_TRUE(f.RecipeOn(png));
    EXPECT_TRUE(f.off.contains("to-tiff"));
    EXPECT_EQ(contents::CountOn(f, section).on, 4);
}

TEST(WheelContents, TheLastSectionStaysOn)
{
    Features f;
    for (const Ring ring : { Ring::Tools, Ring::Reduce, Ring::Files })
    {
        EXPECT_TRUE(contents::CanTurnOff(f, ring));
        f = contents::WithSection(f, ring, false);
    }
    EXPECT_EQ(f.SectionsOn(), 1);
    EXPECT_FALSE(contents::CanTurnOff(f, Ring::Convert));
    EXPECT_EQ(contents::WithSection(f, Ring::Convert, false), f);
    // A section that is already off can always be "turned off".
    EXPECT_TRUE(contents::CanTurnOff(f, Ring::Tools));
}

TEST(WheelContents, ResetTurnsTheSectionAndItsSlicesOn)
{
    const auto section = Convert();
    Features f;
    f.off = { "to-png", "to-gif", "elsewhere" };
    f.sections[0] = false;
    f = contents::Reset(f, section);
    EXPECT_TRUE(f.SectionOn(Ring::Convert));
    EXPECT_EQ(f.off, std::set<std::string>{ "elsewhere" });   // another section's slice keeps its choice
}

TEST(WheelContents, CountsAndText)
{
    const auto section = Convert();
    Features f = contents::WithSlice(Features{}, "to-gif", false);
    EXPECT_EQ(contents::CountOn(f, section.groups[0]).on, 3);
    EXPECT_EQ(contents::CountOn(f, section.groups[1]).on, 1);
    EXPECT_EQ(contents::CountOn(f, section.groups[1]).total, 2);
    EXPECT_EQ(contents::CountText(contents::CountOn(f, section.groups[1])), L"1 of 2 on");
    EXPECT_EQ(contents::Summary(f, section), L"Convert is on. 2 kinds of file, 4 actions on the wheel.");
    f = contents::WithSection(f, Ring::Convert, false);
    EXPECT_EQ(contents::Summary(f, section), L"Convert is off: it isn\u2019t on the wheel.");
    EXPECT_EQ(contents::Listing(section.groups[0]), L"PNG \u00B7 WebP \u00B7 TIFF");
    EXPECT_EQ(contents::Listing(section.groups[0], 2), L"PNG \u00B7 WebP \u2026");
}

TEST(WheelContents, FindAnActionAndThePreview)
{
    const auto section = Convert();
    const auto& photos = section.groups[0];
    EXPECT_TRUE(contents::Matches(photos.slices[1], L""));
    EXPECT_TRUE(contents::Matches(photos.slices[1], L"webp"));
    EXPECT_TRUE(contents::Matches(photos.slices[0], L"lossless png"));
    EXPECT_FALSE(contents::Matches(photos.slices[2], L"lossless"));
    const auto preview = contents::PreviewSlices(contents::WithSlice(Features{}, "to-webp", false), photos);
    ASSERT_EQ(preview.size(), 2u);
    EXPECT_EQ(preview[0].id, "to-png");
    EXPECT_EQ(preview[1].id, "to-tiff");
    EXPECT_EQ(section.groups.size(), 3u);
    EXPECT_EQ(SettingsTree{ { section } }.Find(Ring::Convert)->title, L"Convert");
    EXPECT_EQ(SettingsTree{ { section } }.Find(Ring::Files), nullptr);
}
