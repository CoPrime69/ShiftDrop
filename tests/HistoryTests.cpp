#include "HistoryStore.h"
#include "HistoryView.h"
#include "PathUtil.h"
#include "TestUtil.h"
#include "Utf.h"

#include <algorithm>
#include <chrono>

using namespace sd;
using namespace sd::test;

namespace
{
    std::unique_ptr<HistoryStore> OpenStore(const TempDir& dir)
    {
        std::wstring error;
        auto store = HistoryStore::Open(dir / L"history.db", error);
        EXPECT_TRUE(store) << Narrow(error);
        return store;
    }
}

TEST(History, RunStatusFollowsItems)
{
    TempDir dir;
    Recipes recipes(dir);
    auto history = OpenStore(dir);
    ASSERT_TRUE(history);

    const auto& png = recipes.Get("to-png");
    const auto all = history->BeginRun(png, 2);
    history->FinishItem(history->BeginItem(all, L"C:\\a.jpg", L""), status::Done, L"C:\\a (converted).png", 10, "ab", L"", 5);
    history->FinishItem(history->BeginItem(all, L"C:\\b.jpg", L""), status::Done, L"C:\\b (converted).png", 20, "cd", L"", 5);
    history->FinishRun(all);

    const auto some = history->BeginRun(png, 2);
    history->FinishItem(history->BeginItem(some, L"C:\\c.jpg", L""), status::Done, L"C:\\c (converted).png", 10, "ef", L"", 5);
    history->FinishItem(history->BeginItem(some, L"C:\\d.txt", L""), status::Failed, L"", 0, "", L"no decoder", 1);
    history->FinishRun(some);

    const auto none = history->BeginRun(png, 1);
    history->FinishItem(history->BeginItem(none, L"C:\\e.txt", L""), status::Failed, L"", 0, "", L"no decoder", 1);
    history->FinishRun(none);

    const auto runs = history->RecentRuns(10);
    ASSERT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[0].id, none);   // newest first
    EXPECT_EQ(runs[0].status, status::Failed);
    EXPECT_EQ(runs[1].status, status::Partial);
    EXPECT_EQ(runs[1].done, 1);
    EXPECT_EQ(runs[1].items, 2);
    EXPECT_EQ(runs[2].status, status::Done);
    EXPECT_EQ(runs[2].recipeId, "to-png");
    EXPECT_NE(runs[2].recipeJson.find("image.encode"), std::string::npos);   // the recipe as it ran

    const auto items = history->Items(all);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[1].output, L"C:\\b (converted).png");
    EXPECT_EQ(items[1].outputSize, 20u);
    EXPECT_EQ(items[1].outputSha256, "cd");
}

TEST(History, InterruptedItemsAreRecoveredOnce)
{
    TempDir dir;
    Recipes recipes(dir);
    int64_t run = 0;
    {
        auto history = OpenStore(dir);
        ASSERT_TRUE(history);
        run = history->BeginRun(recipes.Get("to-webp"), 1);
        history->BeginItem(run, L"C:\\a.png", L"C:\\~a.1234abcd.sdpart");   // the app "dies" here
    }
    auto history = OpenStore(dir);
    ASSERT_TRUE(history);
    const auto recovered = history->RecoverInterrupted();
    ASSERT_EQ(recovered.size(), 1u);
    EXPECT_EQ(recovered[0].staging, L"C:\\~a.1234abcd.sdpart");
    EXPECT_EQ(history->Items(run)[0].status, status::Interrupted);
    EXPECT_EQ(history->RecentRuns(1)[0].status, status::Interrupted);
    EXPECT_TRUE(history->RecoverInterrupted().empty());
}

// ---- What the History window shows (HistoryView.h) ------------------------------------------------

namespace
{
    using namespace std::chrono;

    RunRecord MakeRun(std::wstring label, std::wstring started, std::vector<std::wstring> files, const char* runStatus = status::Done)
    {
        RunRecord r;
        r.recipeLabel = std::move(label);
        r.started = std::move(started);
        r.files = std::move(files);
        r.status = runStatus;
        r.items = static_cast<int>(r.files.size());
        r.done = runStatus == std::string(status::Done) ? r.items : 0;
        return r;
    }

    constexpr year_month_day kToday{ year{ 2026 }, month{ 10 }, day{ 3 } };
}

TEST(History, RunsCarryTheirFileNames)
{
    TempDir dir;
    Recipes recipes(dir);
    auto history = OpenStore(dir);
    ASSERT_TRUE(history);
    const auto run = history->BeginRun(recipes.Get("to-png"), 2);
    history->FinishItem(history->BeginItem(run, L"C:\\Trips\\Rome\\beach.jpg", L""), status::Done, L"C:\\Trips\\Rome\\beach (converted).png", 1, "a", L"", 1);
    history->FinishItem(history->BeginItem(run, L"C:\\Trips\\city.jpg", L""), status::Done, L"C:\\Trips\\city (converted).png", 1, "b", L"", 1);
    history->FinishRun(run);
    const auto runs = history->RecentRuns(5);
    ASSERT_EQ(runs.size(), 1u);
    auto files = runs[0].files;
    std::sort(files.begin(), files.end());
    EXPECT_EQ(files, (std::vector<std::wstring>{ L"beach.jpg", L"city.jpg" }));   // names only, no folders
}

TEST(History, SearchMatchesFileNamesAndActions)
{
    const std::vector<RunRecord> runs{
        MakeRun(L"PNG", L"2026-10-03 10:00:00", { L"Holiday 01.jpg", L"Holiday 02.jpg" }),
        MakeRun(L"Compress", L"2026-10-03 09:00:00", { L"report.pdf" }),
        MakeRun(L"Rename", L"2026-10-02 12:00:00", { L"notes.txt" }),
    };
    const auto find = [&](std::wstring search) {
        history::Filter f;
        f.search = std::move(search);
        return history::Apply(runs, f, kToday);
    };
    EXPECT_EQ(find(L""), (std::vector<size_t>{ 0, 1, 2 }));
    EXPECT_EQ(find(L"holiday"), (std::vector<size_t>{ 0 }));      // a file's name, any case
    EXPECT_EQ(find(L"compress"), (std::vector<size_t>{ 1 }));     // the action
    EXPECT_EQ(find(L"  png  HOLIDAY "), (std::vector<size_t>{ 0 }));   // every word, anywhere
    EXPECT_EQ(find(L"png report"), (std::vector<size_t>{}));      // no run has both
    EXPECT_EQ(find(L".txt"), (std::vector<size_t>{ 2 }));
    EXPECT_EQ(find(L"Trips"), (std::vector<size_t>{}));           // folders are not searched
}

TEST(History, FiltersByActionAndPeriod)
{
    const std::vector<RunRecord> runs{
        MakeRun(L"PNG", L"2026-10-03 00:00:00", { L"a.jpg" }),   // today, at midnight
        MakeRun(L"WebP", L"2026-10-02 23:59:59", { L"b.jpg" }),  // yesterday
        MakeRun(L"PNG", L"2026-09-27 08:00:00", { L"c.jpg" }),   // six days ago: still in the last 7 days
        MakeRun(L"PNG", L"2026-09-26 23:59:59", { L"d.jpg" }),   // seven days ago: not
    };
    // The periods start at midnight, across a month's end too.
    EXPECT_EQ(history::PeriodStart(history::Period::All, kToday), L"");
    EXPECT_EQ(history::PeriodStart(history::Period::Today, kToday), L"2026-10-03 00:00:00");
    EXPECT_EQ(history::PeriodStart(history::Period::Week, kToday), L"2026-09-27 00:00:00");
    EXPECT_EQ(history::PeriodStart(history::Period::Week, year_month_day{ year{ 2027 }, month{ 1 }, day{ 2 } }), L"2026-12-27 00:00:00");

    history::Filter f;
    f.period = history::Period::Today;
    EXPECT_EQ(history::Apply(runs, f, kToday), (std::vector<size_t>{ 0 }));
    f.period = history::Period::Week;
    EXPECT_EQ(history::Apply(runs, f, kToday), (std::vector<size_t>{ 0, 1, 2 }));
    f.period = history::Period::All;
    EXPECT_EQ(history::Apply(runs, f, kToday).size(), 4u);

    f.action = L"PNG";
    EXPECT_EQ(history::Apply(runs, f, kToday), (std::vector<size_t>{ 0, 2, 3 }));
    f.period = history::Period::Week;
    f.search = L"c.jpg";
    EXPECT_EQ(history::Apply(runs, f, kToday), (std::vector<size_t>{ 2 }));   // all three together

    // The Action filter's choices: each action once, by name.
    EXPECT_EQ(history::Actions(runs), (std::vector<std::wstring>{ L"PNG", L"WebP" }));
}

TEST(History, RowsShowUndoneAndFailed)
{
    const auto state = [](const char* s) { return history::StateOf(MakeRun(L"PNG", L"2026-10-03 10:00:00", { L"a.jpg" }, s)); };
    EXPECT_EQ(state(status::Done).text, L"Done");
    EXPECT_EQ(state(status::Done).tone, history::Tone::Normal);
    EXPECT_EQ(state(status::Undone).text, L"Undone");
    EXPECT_EQ(state(status::Undone).tone, history::Tone::Muted);
    EXPECT_EQ(state(status::Failed).text, L"Failed");
    EXPECT_EQ(state(status::Failed).tone, history::Tone::Problem);
    EXPECT_EQ(state(status::Interrupted).tone, history::Tone::Problem);
    EXPECT_EQ(state(status::Partial).text, L"Partly done");
    EXPECT_EQ(state(status::Running).text, L"Running");

    // Undo only for a run that made something and was not undone yet.
    auto partial = MakeRun(L"PNG", L"2026-10-03 10:00:00", { L"a.jpg", L"b.txt" }, status::Partial);
    partial.done = 1;
    EXPECT_TRUE(history::CanUndo(partial));
    EXPECT_TRUE(history::CanUndo(MakeRun(L"PNG", L"", { L"a.jpg" }, status::Done)));
    EXPECT_FALSE(history::CanUndo(MakeRun(L"PNG", L"", { L"a.jpg" }, status::Undone)));
    EXPECT_FALSE(history::CanUndo(MakeRun(L"PNG", L"", { L"a.jpg" }, status::Failed)));
}

TEST(History, ClearEmptiesTheListAndLeavesFilesAlone)
{
    TempDir dir;
    Recipes recipes(dir);
    auto history = OpenStore(dir);
    ASSERT_TRUE(history);
    const auto& png = recipes.Get("to-png");

    // A finished run whose output is on disk, a failed one, an undone one, and one still going.
    const auto output = dir / L"a (converted).png";
    WriteText(output, "png");
    const auto done = history->BeginRun(png, 1);
    history->FinishItem(history->BeginItem(done, dir / L"a.jpg", L""), status::Done, output, 3, "x", L"", 1);
    history->FinishRun(done);
    const auto failed = history->BeginRun(png, 1);
    history->FinishItem(history->BeginItem(failed, dir / L"b.txt", L""), status::Failed, L"", 0, "", L"no decoder", 1);
    history->FinishRun(failed);
    const auto undone = history->BeginRun(png, 1);
    history->FinishItem(history->BeginItem(undone, dir / L"c.jpg", L""), status::Done, dir / L"c (converted).png", 3, "y", L"", 1);
    history->FinishRun(undone);
    history->SetRunStatus(undone, status::Undone);
    const auto running = history->BeginRun(png, 1);
    history->BeginItem(running, dir / L"d.jpg", dir / L"~d.1234abcd.sdpart");

    EXPECT_EQ(history->Clear(), 3);
    const auto runs = history->RecentRuns(10);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].id, running);                    // still going: kept for crash recovery
    EXPECT_TRUE(history->Items(done).empty());         // the items went with their runs
    EXPECT_TRUE(PathExists(output));                   // no file is touched
    EXPECT_EQ(history->RecoverInterrupted().size(), 1u);   // the staging file can still be cleaned up

    // New runs are recorded as before, and a second clear finds the rest.
    const auto after = history->BeginRun(png, 1);
    history->FinishItem(history->BeginItem(after, dir / L"e.jpg", L""), status::Done, dir / L"e (converted).png", 3, "z", L"", 1);
    history->FinishRun(after);
    EXPECT_EQ(history->RecentRuns(10).front().id, after);
    EXPECT_EQ(history->Clear(), 2);   // the interrupted run is finished now, and the new one
    EXPECT_TRUE(history->RecentRuns(10).empty());
    EXPECT_EQ(history->Clear(), 0);
}
