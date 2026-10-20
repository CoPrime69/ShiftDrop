#include "HistoryStore.h"
#include "TestUtil.h"

using namespace sd;
using namespace sd::test;

namespace
{
    std::unique_ptr<HistoryStore> OpenStore(const TempDir& dir)
    {
        std::wstring error;
        auto store = HistoryStore::Open(dir / L"history.db", error);
        EXPECT_TRUE(store) << std::string(error.begin(), error.end());
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
