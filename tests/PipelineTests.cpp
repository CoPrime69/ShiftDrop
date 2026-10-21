#include "FileMove.h"
#include "Hash.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "Undo.h"

#include <wil/resource.h>

#include <format>

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
    ASSERT_TRUE(results[0].ok) << Narrow(results[0].error);
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
    ASSERT_TRUE(results[0].ok) << Narrow(results[0].error);
    EXPECT_EQ(std::filesystem::path(results[0].output).filename(), L"tall (edited).png");
    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(results[0].output, w, h));
    EXPECT_EQ(w, 30u);
    EXPECT_EQ(h, 40u);
}

namespace
{
    WICPixelFormatGUID PixelFormatOf(const std::wstring& path)
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormatUndefined;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)))
            frame->GetPixelFormat(&format);
        return format;
    }
}

TEST(Pipeline, OpaquePicturesSkipTheAlphaChannel)
{
    Fixture f;
    const auto png = f.dir / L"photo.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(png, 4000, 3000, true));
    std::vector<ItemResult> jpg;
    f.Run("to-jpg", { png }, &jpg);
    ASSERT_TRUE(jpg[0].ok) << Narrow(jpg[0].error);

    // A JPEG has no alpha: its PNG is 24-bit, the same pixels without an empty alpha channel.
    std::vector<ItemResult> results;
    f.Run("to-png", { jpg[0].output }, &results);
    ASSERT_TRUE(results[0].ok) << Narrow(results[0].error);
    EXPECT_EQ(PixelFormatOf(results[0].output), GUID_WICPixelFormat24bppBGR);
    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(results[0].output, w, h));
    EXPECT_EQ(w, 4000u);
    EXPECT_EQ(h, 3000u);

    // Shrunk from the JPEG (decoded at half size first, then scaled): exactly the size it always was.
    results.clear();
    f.Run("max-1920", { jpg[0].output }, &results);
    ASSERT_TRUE(results[0].ok) << Narrow(results[0].error);
    ASSERT_TRUE(ImageSize(results[0].output, w, h));
    EXPECT_EQ(w, 1920u);
    EXPECT_EQ(h, 1440u);
    results.clear();
    f.Run("half-size", { jpg[0].output }, &results);
    ASSERT_TRUE(results[0].ok) << Narrow(results[0].error);
    ASSERT_TRUE(ImageSize(results[0].output, w, h));
    EXPECT_EQ(w, 2000u);
    EXPECT_EQ(h, 1500u);

    // A picture with transparency keeps its alpha channel.
    const auto clear = f.dir / L"clear.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(clear, 40, 30));
    results.clear();
    f.Run("to-tiff", { clear }, &results);
    ASSERT_TRUE(results[0].ok) << Narrow(results[0].error);
    EXPECT_NE(PixelFormatOf(results[0].output), GUID_WICPixelFormat24bppBGR);
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

// Image and duplicate outputs go through StagedOutput like every engine's: a job that fails after
// its staging file was made removes the "ShiftDrop" folder made for it, and is in history.
TEST(Pipeline, FailedJobsLeaveNoOutputFolder)
{
    Fixture f;
    NamingScope scope(OutputNaming::Subfolder);
    const auto broken = f.dir / L"broken.png";
    WriteText(broken, "not a picture");
    const auto notes = f.dir / L"notes.txt";
    WriteText(notes, "hello");
    // Held open with no sharing, so the copy fails once its staging file exists.
    const wil::unique_hfile lock(CreateFileW(notes.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_TRUE(lock);

    for (const auto& [id, source] : { std::pair{ "to-jpg", broken }, std::pair{ "duplicate", notes } })
    {
        std::vector<ItemResult> results;
        const auto run = f.Run(id, { source }, &results);
        EXPECT_FALSE(results[0].ok) << id;
        EXPECT_FALSE(std::filesystem::exists(f.dir / L"ShiftDrop")) << id;
        const auto items = f.history->Items(run.id);
        ASSERT_EQ(items.size(), 1u) << id;
        EXPECT_EQ(items[0].status, status::Failed) << id;
        EXPECT_TRUE(items[0].staging.empty()) << id;
    }
    EXPECT_EQ(CountFiles(f.dir, L".sdpart"), 0u);
}

// An output that cannot even start (here a file named "ShiftDrop" is where the output folder goes)
// is a failed row in history for every built-in path, not a silent gap.
TEST(Pipeline, OutputThatCannotStartIsInHistory)
{
    Fixture f;
    NamingScope scope(OutputNaming::Subfolder);
    WriteText(f.dir / L"ShiftDrop", "a file, not a folder");
    const auto png = f.dir / L"a.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(png, 8, 8));
    const auto notes = f.dir / L"notes.txt";
    WriteText(notes, "hello");

    for (const auto& [id, source] : { std::pair{ "to-jpg", png }, std::pair{ "duplicate", notes } })
    {
        std::vector<ItemResult> results;
        const auto run = f.Run(id, { source }, &results);
        EXPECT_FALSE(results[0].ok) << id;
        EXPECT_TRUE(results[0].error.starts_with(L"cannot write next to the source")) << id << ": " << Narrow(results[0].error);
        const auto items = f.history->Items(run.id);
        ASSERT_EQ(items.size(), 1u) << id;
        EXPECT_EQ(items[0].status, status::Failed) << id;
        EXPECT_EQ(items[0].error, results[0].error) << id;
    }
    EXPECT_EQ(ReadAll(f.dir / L"ShiftDrop"), "a file, not a folder");
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
    ASSERT_TRUE(report.ok) << Narrow(report.message);
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
    ASSERT_TRUE(outcomes[0].ok) << Narrow(outcomes[0].error);
    ASSERT_TRUE(outcomes[1].ok) << Narrow(outcomes[1].error);
    EXPECT_FALSE(std::filesystem::exists(a));
    EXPECT_EQ(ReadAll(target + L"\\a.txt"), "already there");          // never overwritten
    EXPECT_NE(outcomes[0].destination, target + L"\\a.txt");            // renamed instead
    EXPECT_EQ(ReadAll(outcomes[0].destination), "mine");
    EXPECT_TRUE(std::filesystem::exists(target + L"\\folder\\inside.txt"));

    const auto run = f.history->RecentRuns(1)[0];
    EXPECT_TRUE(IsMoveRun(run));
    const auto report = UndoRun(*f.history, run, nullptr);
    ASSERT_TRUE(report.ok) << Narrow(report.message);
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
    ASSERT_TRUE(outcomes[0].ok) << Narrow(outcomes[0].error);
    WriteText(a, "a new file with the old name");
    const auto report = UndoRun(*f.history, f.history->RecentRuns(1)[0], nullptr);
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.occupied.size(), 1u);
    EXPECT_EQ(ReadAll(a), "a new file with the old name");
    EXPECT_EQ(ReadAll(outcomes[0].destination), "mine");
}

TEST(Compress, QualitySearchFindsTheHighestFit)
{
    int calls = 0;
    const auto linear = [&](int q) { ++calls; return static_cast<int64_t>(q) * 1000; };
    EXPECT_EQ(BestQualityUnder(55'500, 40, 92, linear), 55);
    EXPECT_LE(calls, 7);   // a binary search, not a scan
    EXPECT_EQ(BestQualityUnder(39'000, 40, 92, linear), -1);
    EXPECT_EQ(BestQualityUnder(1'000'000, 40, 92, linear), 92);
    EXPECT_NEAR(ShrinkFactor(250, 1000), 0.46, 1e-9);   // half the side length, minus the margin
    EXPECT_DOUBLE_EQ(ShrinkFactor(0, 1000), 1.0);
}

TEST(Pipeline, CompressFitsTheTargetAndShrinksOnlyWhenNeeded)
{
    Fixture f;
    const auto source = f.dir / L"photo.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(source, 1600, 1200, true));

    std::vector<ItemResult> results;
    f.Run("image-under-1mb", { source }, &results);
    ASSERT_TRUE(results[0].ok) << Narrow(results[0].error);
    EXPECT_EQ(std::filesystem::path(results[0].output).filename(), L"photo (compressed).jpg");
    EXPECT_LE(std::filesystem::file_size(results[0].output), 1024u * 1024u);
    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(results[0].output, w, h));
    EXPECT_EQ(w, 1600u);   // fits at full size, so it is not scaled

    // A target too small for any quality at full size: the picture is scaled down to fit.
    Recipe tiny;
    std::string error;
    const char* json = R"({ "id": "tiny", "label": "Tiny", "ring": "tools", "suffix": "tiny",
                           "steps": [ { "action": "image.compress", "params": { "maxKB": 30 } } ] })";
    ASSERT_TRUE(ParseRecipe(json, f.recipes.registry, tiny, error)) << error;
    const auto runId = f.history->BeginRun(tiny, 1);
    const auto shrunk = f.runner->Run(tiny, runId, source, std::stop_token{});
    ASSERT_TRUE(shrunk.ok) << Narrow(shrunk.error);
    EXPECT_LE(std::filesystem::file_size(shrunk.output), 30u * 1024u);
    ASSERT_TRUE(ImageSize(shrunk.output, w, h));
    EXPECT_LT(w, 1600u);
    EXPECT_EQ(CountFiles(f.dir, L".sdpart"), 0u);

    // Already under the target: refused, nothing written.
    const auto again = f.runner->Run(f.recipes.Get("image-under-1mb"), runId, shrunk.output, std::stop_token{});
    EXPECT_FALSE(again.ok);
    EXPECT_EQ(Narrow(again.error), "already under 1 MB");
}

TEST(Pipeline, ReduceSizeEndsAtMostItsShareOfTheSource)
{
    Fixture f;
    const auto source = f.dir / L"photo.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(source, 1600, 1200, true));
    const auto before = std::filesystem::file_size(source);
    for (const int smaller : { 90, 50, 20 })
    {
        const auto id = std::format("reduce-image-{}", smaller);
        const auto& recipe = f.recipes.Get(id);
        EXPECT_EQ(recipe.ring, "reduce");
        const auto runId = f.history->BeginRun(recipe, 1);
        const auto result = f.runner->Run(recipe, runId, source, std::stop_token{});
        ASSERT_TRUE(result.ok) << id << ": " << Narrow(result.error);
        EXPECT_LE(std::filesystem::file_size(result.output), before * static_cast<uintmax_t>(100 - smaller) / 100) << id;
        std::filesystem::remove(result.output);
    }
    EXPECT_EQ(CountFiles(f.dir, L".sdpart"), 0u);
}
