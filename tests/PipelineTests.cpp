#include "FileMove.h"
#include "Hash.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Undo.h"

using namespace sd;
using namespace sd::test;

namespace
{
    struct Fixture
    {
        TempDir dir;
        Recipes recipes{ dir };
        std::unique_ptr<HistoryStore> history;
        std::unique_ptr<PipelineRunner> runner;

        Fixture()
        {
            std::wstring error;
            history = HistoryStore::Open(dir / L"history.db", error);
            runner = std::make_unique<PipelineRunner>(std::make_shared<const ImageCodecs>(), history.get());
        }

        // Runs a recipe on files as the app does, and returns the history run.
        RunRecord Run(std::string_view recipeId, const std::vector<std::wstring>& files, std::vector<ItemResult>* results = nullptr)
        {
            const auto& recipe = recipes.Get(recipeId);
            const auto runId = history->BeginRun(recipe, static_cast<int>(files.size()));
            for (const auto& f : files)
            {
                auto r = runner->Run(recipe, runId, f, std::stop_token{});
                if (results)
                    results->push_back(std::move(r));
            }
            history->FinishRun(runId);
            return history->RecentRuns(1).at(0);
        }
    };
}

TEST(Pipeline, WebReadyIsOneStepWithNoLeftovers)
{
    Fixture f;
    const auto source = f.dir / L"photo.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(source, 3000, 1500));

    std::vector<ItemResult> results;
    const auto run = f.Run("web-ready", { source }, &results);
    ASSERT_TRUE(results[0].ok) << std::string(results[0].error.begin(), results[0].error.end());
    EXPECT_EQ(std::filesystem::path(results[0].output).filename(), L"photo (converted).webp");
    EXPECT_EQ(run.status, status::Done);
    EXPECT_EQ(CountFiles(f.dir, L".sdpart"), 0u);
    EXPECT_EQ(CountFiles(f.dir, L".webp"), 1u);   // no intermediate resized PNG anywhere

    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(results[0].output, w, h));
    EXPECT_EQ(w, 2048u);
    EXPECT_EQ(h, 1024u);

    const auto items = f.history->Items(run.id);
    EXPECT_EQ(items[0].outputSha256, Sha256File(results[0].output));
    EXPECT_TRUE(items[0].staging.empty());   // cleared once the output is committed
}

TEST(Pipeline, RotateKeepsFormatAndSwapsSize)
{
    Fixture f;
    const auto source = f.dir / L"tall.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(source, 40, 30));
    std::vector<ItemResult> results;
    f.Run("rotate-right", { source }, &results);
    ASSERT_TRUE(results[0].ok) << std::string(results[0].error.begin(), results[0].error.end());
    EXPECT_EQ(std::filesystem::path(results[0].output).filename(), L"tall (edited).png");
    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(results[0].output, w, h));
    EXPECT_EQ(w, 30u);
    EXPECT_EQ(h, 40u);
}

TEST(Pipeline, SameFormatIsSkippedAndNonImagesFail)
{
    Fixture f;
    const auto png = f.dir / L"a.png";
    const auto text = f.dir / L"notes.txt";
    ASSERT_HRESULT_SUCCEEDED(WritePng(png, 8, 8));
    WriteText(text, "hello");
    EXPECT_TRUE(f.runner->IsNoOp(f.recipes.Get("to-png"), png));
    EXPECT_FALSE(f.runner->Accepts(f.recipes.Get("to-png"), text));

    std::vector<ItemResult> results;
    const auto run = f.Run("to-png", { png, text }, &results);
    EXPECT_TRUE(results[0].skipped);
    EXPECT_FALSE(results[1].ok);
    EXPECT_EQ(run.status, status::Failed);   // nothing was produced
    EXPECT_EQ(CountFiles(f.dir, L".png"), 1u);
}

TEST(Pipeline, DuplicateCopiesBytes)
{
    Fixture f;
    const auto source = f.dir / L"notes.txt";
    WriteText(source, "hello");
    std::vector<ItemResult> results;
    f.Run("duplicate", { source }, &results);
    ASSERT_TRUE(results[0].ok);
    EXPECT_EQ(std::filesystem::path(results[0].output).filename(), L"notes (copy).txt");
    EXPECT_EQ(ReadAll(results[0].output), "hello");
    EXPECT_FALSE(f.runner->Accepts(f.recipes.Get("duplicate"), f.dir.path().wstring()));   // folders: not yet
}

TEST(Undo, RefusedWhenAnOutputChanged)
{
    Fixture f;
    const auto a = f.dir / L"a.png";
    const auto b = f.dir / L"b.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(a, 16, 16));
    ASSERT_HRESULT_SUCCEEDED(WritePng(b, 16, 16));
    std::vector<ItemResult> results;
    const auto run = f.Run("to-bmp", { a, b }, &results);
    ASSERT_TRUE(results[0].ok && results[1].ok);

    WriteText(results[1].output, "edited by someone");
    const auto report = UndoRun(*f.history, run, nullptr);
    EXPECT_FALSE(report.ok);
    ASSERT_EQ(report.modified.size(), 1u);
    EXPECT_EQ(report.modified[0], results[1].output);
    // All or nothing: the unmodified output is still there too.
    EXPECT_TRUE(std::filesystem::exists(results[0].output));
    EXPECT_TRUE(std::filesystem::exists(results[1].output));
    EXPECT_EQ(f.history->RecentRuns(1)[0].status, status::Done);
}

TEST(Undo, RecyclesOutputs)
{
    Fixture f;
    const auto a = f.dir / L"a.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(a, 16, 16));
    std::vector<ItemResult> results;
    const auto run = f.Run("to-jpg", { a }, &results);
    ASSERT_TRUE(results[0].ok);

    const auto report = UndoRun(*f.history, run, nullptr);
    ASSERT_TRUE(report.ok) << std::string(report.message.begin(), report.message.end());
    EXPECT_EQ(report.recycled, 1);
    EXPECT_FALSE(std::filesystem::exists(results[0].output));
    EXPECT_TRUE(std::filesystem::exists(a));
    EXPECT_EQ(f.history->RecentRuns(1)[0].status, status::Undone);
}

TEST(Move, MovesRenamesOnCollisionAndUndoes)
{
    Fixture f;
    const auto target = f.dir / L"target";
    std::filesystem::create_directories(target);
    const auto a = f.dir / L"a.txt";
    const auto folder = f.dir / L"folder";
    WriteText(a, "mine");
    std::filesystem::create_directories(folder);
    WriteText(folder + L"\\inside.txt", "x");
    WriteText(target + L"\\a.txt", "already there");

    int64_t runId = 0;
    const auto outcomes = MoveToFolder(f.recipes.Get("move"), { a, folder }, target, f.history.get(), runId);
    ASSERT_EQ(outcomes.size(), 2u);
    ASSERT_TRUE(outcomes[0].ok) << std::string(outcomes[0].error.begin(), outcomes[0].error.end());
    ASSERT_TRUE(outcomes[1].ok);
    EXPECT_FALSE(std::filesystem::exists(a));
    EXPECT_EQ(ReadAll(target + L"\\a.txt"), "already there");          // never overwritten
    EXPECT_NE(outcomes[0].destination, target + L"\\a.txt");            // renamed instead
    EXPECT_EQ(ReadAll(outcomes[0].destination), "mine");
    EXPECT_TRUE(std::filesystem::exists(target + L"\\folder\\inside.txt"));

    const auto run = f.history->RecentRuns(1)[0];
    EXPECT_TRUE(IsMoveRun(run));
    const auto report = UndoRun(*f.history, run, nullptr);
    ASSERT_TRUE(report.ok) << std::string(report.message.begin(), report.message.end());
    EXPECT_EQ(report.movedBack, 2);
    EXPECT_EQ(ReadAll(a), "mine");
    EXPECT_TRUE(std::filesystem::exists(folder + L"\\inside.txt"));
    EXPECT_EQ(ReadAll(target + L"\\a.txt"), "already there");
}

TEST(Move, SkipsItemsAlreadyThereAndRefusesFolderIntoItself)
{
    Fixture f;
    const auto a = f.dir / L"a.txt";
    const auto folder = f.dir / L"folder";
    WriteText(a, "x");
    std::filesystem::create_directories(folder + L"\\sub");
    int64_t runId = 0;
    auto outcomes = MoveToFolder(f.recipes.Get("move"), { a }, f.dir.path().wstring(), f.history.get(), runId);
    EXPECT_TRUE(outcomes[0].skipped);
    outcomes = MoveToFolder(f.recipes.Get("move"), { folder }, folder + L"\\sub", f.history.get(), runId);
    EXPECT_FALSE(outcomes[0].ok);
    EXPECT_TRUE(std::filesystem::exists(folder));
}

TEST(Move, UndoRefusedWhenOriginalPlaceIsTaken)
{
    Fixture f;
    const auto target = f.dir / L"target";
    std::filesystem::create_directories(target);
    const auto a = f.dir / L"a.txt";
    WriteText(a, "mine");
    int64_t runId = 0;
    const auto outcomes = MoveToFolder(f.recipes.Get("move"), { a }, target, f.history.get(), runId);
    ASSERT_TRUE(outcomes[0].ok);
    WriteText(a, "a new file with the old name");
    const auto report = UndoRun(*f.history, f.history->RecentRuns(1)[0], nullptr);
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.occupied.size(), 1u);
    EXPECT_EQ(ReadAll(a), "a new file with the old name");
    EXPECT_EQ(ReadAll(outcomes[0].destination), "mine");
}
