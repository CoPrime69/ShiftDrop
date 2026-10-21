#include "FileMove.h"
#include "HistoryStore.h"
#include "Undo.h"
#include "rename/RenamePattern.h"
#include "TestUtil.h"
#include "Utf.h"

#include <set>

using namespace sd;
using namespace sd::rename;
using namespace sd::test;

namespace
{
    std::wstring Name(const Pattern& pattern, const Item& item, size_t n = 0)
    {
        std::wstring error;
        auto name = Expand(pattern, item, n, error);
        return error.empty() ? name : L"error: " + error;
    }

    Pattern With(std::wstring text)
    {
        Pattern p;
        p.text = std::move(text);
        return p;
    }

    // A pretend folder: the names in it, ignoring case like NTFS.
    Exists Folder(std::set<std::wstring> paths)
    {
        return [paths = std::move(paths)](const std::wstring& path) {
            for (const auto& p : paths)
                if (CompareStringOrdinal(p.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL)
                    return true;
            return false;
        };
    }
}

TEST(RenamePattern, Fields)
{
    const Item photo{ L"C:\\Trips\\Rome\\IMG_4411.JPG", L"2026-10-21", false };
    EXPECT_EQ(Name(With(L"{name}"), photo), L"IMG_4411.JPG");   // the extension is always kept
    EXPECT_EQ(Name(With(L"Rome {n}"), photo, 0), L"Rome 1.JPG");
    EXPECT_EQ(Name(With(L"Rome {n}"), photo, 11), L"Rome 12.JPG");
    EXPECT_EQ(Name(With(L"Rome {n:000}"), photo, 4), L"Rome 005.JPG");
    EXPECT_EQ(Name(With(L"{date} {name}"), photo), L"2026-10-21 IMG_4411.JPG");
    EXPECT_EQ(Name(With(L"{folder}-{n:00}"), photo, 2), L"Rome-03.JPG");
    EXPECT_EQ(Name(With(L"{name} ({ext})"), photo), L"IMG_4411 (JPG).JPG");
    EXPECT_EQ(Name(With(L"{{draft}} {name}"), photo), L"{draft} IMG_4411.JPG");
    Pattern from10 = With(L"{n}");
    from10.start = 10;
    EXPECT_EQ(Name(from10, photo, 2), L"12.JPG");
    // A folder's whole name is {name}; it has no extension.
    const Item folder{ L"C:\\Trips\\Rome.2026", L"", true };
    EXPECT_EQ(Name(With(L"{name} done"), folder), L"Rome.2026 done");
    EXPECT_EQ(Name(With(L"[{ext}]"), folder), L"[]");
}

TEST(RenamePattern, FindAndReplace)
{
    const Item item{ L"C:\\a\\IMG_img_0001.png", L"", false };
    Pattern p;
    p.find = L"img_";
    p.replace = L"Photo ";
    EXPECT_EQ(Name(p, item), L"Photo Photo 0001.png");   // every match, any case
    p.matchCase = true;
    EXPECT_EQ(Name(p, item), L"IMG_Photo 0001.png");
    p.find = L"0001";
    p.replace = L"";
    p.text = L"{name}x";
    EXPECT_EQ(Name(p, item), L"IMG_img_x.png");
    p.find = L"zzz";   // no match: unchanged
    EXPECT_EQ(Name(p, item), L"IMG_img_0001x.png");
}

TEST(RenamePattern, BadPatternsSayWhy)
{
    const Item item{ L"C:\\a\\b.txt", L"", false };
    for (const wchar_t* bad : { L"{size}", L"{name", L"name}", L"{n:3}", L"{n:}", L"{}" })
    {
        std::wstring error;
        EXPECT_TRUE(Expand(With(bad), item, 0, error).empty()) << Narrow(bad);
        EXPECT_FALSE(error.empty()) << Narrow(bad);
    }
    const auto plan = MakePlan(With(L"{nmae}"), { item }, Folder({}));
    EXPECT_FALSE(plan.error.empty());
    EXPECT_TRUE(plan.items.empty());
    EXPECT_FALSE(plan.CanRun());
}

TEST(RenamePattern, InvalidNames)
{
    EXPECT_EQ(NameProblem(L"holiday.jpg"), L"");
    EXPECT_EQ(NameProblem(L"icon.txt"), L"");
    EXPECT_EQ(NameProblem(L"console.log"), L"");
    // Device names are reserved with any extension, however many dots follow.
    for (const wchar_t* bad : { L"", L".", L"a:b.txt", L"a/b", L"a\\b", L"what?.png", L"x*", L"<x>", L"a|b", L"q\".txt", L"trailing.",
                                L"trailing ", L"CON", L"con.txt", L"con.tar.gz", L"Nul", L"COM1.log", L"lpt9", L"tab\there" })
        EXPECT_FALSE(NameProblem(bad).empty()) << Narrow(bad);
    EXPECT_FALSE(NameProblem(std::wstring(256, L'a')).empty());
    EXPECT_TRUE(NameProblem(std::wstring(255, L'a')).empty());

    // A pattern that makes an invalid name marks that item and stops the rename.
    const auto plan = MakePlan(With(L"{name}?"), { Item{ L"C:\\a\\b.txt", L"", false } }, Folder({}));
    ASSERT_EQ(plan.items.size(), 1u);
    EXPECT_FALSE(plan.items[0].problem.empty());
    EXPECT_FALSE(plan.CanRun());
}

TEST(RenamePattern, PlanNumbersInOrderAndFindsNoProblem)
{
    const std::vector<Item> items{ { L"C:\\a\\x.jpg" }, { L"C:\\a\\y.jpg" }, { L"C:\\a\\z.png" } };
    const auto plan = MakePlan(With(L"Trip {n:00}"), items, Folder({ L"C:\\a\\x.jpg", L"C:\\a\\y.jpg", L"C:\\a\\z.png" }));
    ASSERT_EQ(plan.items.size(), 3u);
    EXPECT_EQ(plan.items[0].to, L"C:\\a\\Trip 01.jpg");
    EXPECT_EQ(plan.items[1].to, L"C:\\a\\Trip 02.jpg");
    EXPECT_EQ(plan.items[2].to, L"C:\\a\\Trip 03.png");
    EXPECT_EQ(plan.changes, 3u);
    EXPECT_EQ(plan.problems, 0u);
    EXPECT_TRUE(plan.CanRun());
}

TEST(RenamePattern, NeverOverwritesAndRefusesCollisions)
{
    const std::vector<Item> items{ { L"C:\\a\\one.txt" }, { L"C:\\a\\two.txt" }, { L"C:\\a\\three.md" } };
    // "{n}" with the same extension... but "1.txt" is already in the folder.
    auto plan = MakePlan(With(L"{n}"), items, Folder({ L"C:\\a\\one.txt", L"C:\\a\\two.txt", L"C:\\a\\three.md", L"C:\\a\\1.TXT" }));
    EXPECT_FALSE(plan.items[0].problem.empty());   // taken by a file that is not being renamed (any case)
    EXPECT_TRUE(plan.items[1].problem.empty());
    EXPECT_EQ(plan.problems, 1u);
    EXPECT_FALSE(plan.CanRun());

    // A fixed name for every file: the two .txt files clash with each other; the .md one does not.
    plan = MakePlan(With(L"notes"), items, Folder({}));
    EXPECT_FALSE(plan.items[0].problem.empty());
    EXPECT_FALSE(plan.items[1].problem.empty());
    EXPECT_TRUE(plan.items[2].problem.empty());
    EXPECT_EQ(plan.problems, 2u);
    EXPECT_FALSE(plan.CanRun());

    // Swapping two names is refused too: each new name is another item's current one.
    const std::vector<Item> pair{ { L"C:\\a\\a.txt" }, { L"C:\\a\\b.txt" } };
    Pattern swap;
    swap.find = L"a";
    swap.replace = L"b";
    plan = MakePlan(swap, { pair[0] }, Folder({ L"C:\\a\\a.txt", L"C:\\a\\b.txt" }));
    EXPECT_FALSE(plan.CanRun());
}

TEST(RenamePattern, ChangingOnlyTheCaseIsFine)
{
    Pattern p;
    p.find = L"img";
    p.replace = L"IMG";
    p.matchCase = true;
    const auto plan = MakePlan(p, { Item{ L"C:\\a\\img.png" } }, Folder({ L"C:\\a\\img.png" }));
    ASSERT_EQ(plan.items.size(), 1u);
    EXPECT_EQ(plan.items[0].to, L"C:\\a\\IMG.png");
    EXPECT_TRUE(plan.items[0].problem.empty());
    EXPECT_TRUE(plan.CanRun());
}

TEST(RenamePattern, NothingToDoWhenNoNameChanges)
{
    const auto plan = MakePlan(Pattern{}, { Item{ L"C:\\a\\b.txt" } }, Folder({ L"C:\\a\\b.txt" }));
    EXPECT_EQ(plan.changes, 0u);
    EXPECT_EQ(plan.problems, 0u);
    EXPECT_FALSE(plan.items[0].Changes());
    EXPECT_FALSE(plan.CanRun());
}

// The real thing, on disk: renamed through IFileOperation, then undone from history.
TEST(RenameFiles, RenamesAndUndoes)
{
    TempDir dir;
    std::wstring dbError;
    auto history = HistoryStore::Open(dir / L"history.db", dbError);
    ASSERT_TRUE(history) << Narrow(dbError);
    Recipes recipes{ dir };
    const Recipe& recipe = recipes.Get("rename");
    EXPECT_TRUE(recipe.IsFileAction());

    const auto a = dir / L"a.txt", b = dir / L"b.txt";
    WriteText(a, "first");
    WriteText(b, "second");
    const auto plan = MakePlan(With(L"note {n}"), { Item{ a }, Item{ b } });
    ASSERT_TRUE(plan.CanRun());
    std::vector<Rename> renames;
    for (const auto& p : plan.items)
        renames.push_back(Rename{ p.from, p.to });
    int64_t runId = 0;
    const auto outcomes = RenameItems(recipe, renames, history.get(), runId);
    ASSERT_EQ(outcomes.size(), 2u);
    EXPECT_TRUE(outcomes[0].ok) << Narrow(outcomes[0].error);
    EXPECT_TRUE(outcomes[1].ok) << Narrow(outcomes[1].error);
    EXPECT_EQ(ReadAll(dir / L"note 1.txt"), "first");
    EXPECT_EQ(ReadAll(dir / L"note 2.txt"), "second");
    EXPECT_FALSE(std::filesystem::exists(a));

    const auto run = history->RecentRuns(1)[0];
    EXPECT_TRUE(IsRenameRun(run));
    EXPECT_FALSE(IsMoveRun(run));
    const auto report = UndoRun(*history, run, nullptr);
    ASSERT_TRUE(report.ok) << Narrow(report.message);
    EXPECT_EQ(report.movedBack, 2);
    EXPECT_EQ(ReadAll(a), "first");
    EXPECT_EQ(ReadAll(b), "second");
    EXPECT_FALSE(std::filesystem::exists(dir / L"note 1.txt"));
}
