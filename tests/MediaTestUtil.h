#pragma once
#include "HistoryStore.h"
#include "ImagePipeline.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "audiotools/AudioEngine.h"
#include "mediaeditor/EditorApi.h"
#include "mediaeditor/MediaToolsModule.h"
#include "videotools/VideoEngine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <wil/resource.h>

// Shared by the video, audio, media and sandbox tests: ffmpeg argument checks, a video or audio
// tool's recipe, and a run of the tools engine with history.
namespace sd::test
{
    // Is `a` followed by `b` somewhere in the arguments?
    inline bool Has(const std::vector<std::wstring>& args, std::wstring_view a, std::wstring_view b)
    {
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == a && args[i + 1] == b)
                return true;
        return false;
    }

    // The value after `key`, or empty.
    inline std::wstring After(const std::vector<std::wstring>& args, std::wstring_view key)
    {
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == key)
                return args[i + 1];
        return {};
    }

    // Runs the test ffmpeg with `args` (quietly, overwriting) to make an input file.
    inline bool Make(const std::vector<std::wstring>& args)
    {
        std::stop_source never;
        std::vector<std::wstring> a{ L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-y" };
        a.insert(a.end(), args.begin(), args.end());
        return TestFfmpeg()->Run(a, nullptr, never.get_token()).ok;
    }

    // A video or audio tool's recipe by id, whether it has a slice or is reached from the editor's
    // tool bar.
    inline Recipe ToolById(const Recipes& recipes, std::string_view id)
    {
        if (const auto* r = recipes.store.Find(id))
            return *r;
        using mediatools::EditorTool;
        for (const auto first : { EditorTool::TrimVideo, EditorTool::TrimAudio })
            for (const auto tool : mediatools::SiblingTools(first))
            {
                const auto j = mediatools::IsVideoTool(tool) ? videotools::ToolRecipe(tool) : audiotools::ToolRecipe(tool);
                if (j.value("id", std::string()) != id)
                    continue;
                Recipe r;
                std::string error;
                EXPECT_TRUE(ParseRecipe(j.dump(), recipes.registry, r, error)) << error;
                return r;
            }
        ADD_FAILURE() << "no recipe " << std::string(id);
        return {};
    }

    // The video and audio tools engine with its own history, as the app runs it.
    struct MediaToolsFixture
    {
        TempDir dir;
        Recipes recipes{ dir };
        std::unique_ptr<HistoryStore> history;
        std::unique_ptr<IEngine> engine;
        MediaToolsFixture()
        {
            std::wstring error;
            history = HistoryStore::Open(dir / L"history.db", error);
            engine = MediaToolsModule().makeEngine(EngineServices{ history.get(), TestFfmpeg() });
        }
        // Runs tool `id` with `params` over its defaults on `source`, recorded as one run.
        ItemResult Run(std::string_view id, nlohmann::json params, const std::wstring& source, double* lastProgress = nullptr)
        {
            Recipe r = ToolById(recipes, id);
            for (auto& [k, v] : params.items())
                r.steps[0].params[k] = v;
            const auto run = history->BeginRun(r, 1);
            double last = 0;
            auto out = engine->Run(r, run, source, std::stop_token{}, [&](double p) { last = std::max(last, p); });
            history->FinishRun(run);
            if (lastProgress)
                *lastProgress = last;
            return out;
        }
    };

    // Starts a long to-webm conversion (in the media sandbox when `sandboxed`), stops it once ffmpeg
    // reports progress, and checks that nothing is left: no output and no staging file.
    inline void ExpectCancelLeavesNothing(const std::shared_ptr<const FfmpegEngine>& engine, const TempDir& dir, bool sandboxed = false)
    {
        Recipes recipes(dir);
        // Long enough that it is still running when the stop comes.
        const auto source = dir / L"long.avi";
        std::stop_source never;
        ASSERT_TRUE(engine->Run({ L"-hide_banner", L"-nostdin", L"-f", L"lavfi", L"-i", L"testsrc=s=640x360:r=30:d=60", L"-c:v", L"mpeg4",
                                  L"-q:v", L"31", L"-y", L"-f", L"avi", source }, nullptr, never.get_token()).ok);
        sandbox::SetMediaSandbox(sandboxed);
        const auto restore = wil::scope_exit([] { sandbox::SetMediaSandbox(false); });
        const PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr, engine);
        std::stop_source stop;
        std::atomic<bool> started{ false };
        std::jthread canceller([&](std::stop_token own) {   // `own` ends the wait if Run returns first
            while (!started && !own.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            stop.request_stop();
        });
        const auto r = runner.Run(recipes.Get("to-webm"), 0, source, stop.get_token(), [&](double) { started = true; });
        EXPECT_FALSE(r.ok);
        EXPECT_TRUE(r.skipped) << Narrow(r.error);
        EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
        EXPECT_EQ(CountFiles(dir, L".webm"), 0u);
    }
}
