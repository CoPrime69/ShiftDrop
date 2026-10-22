#pragma once
#include "Ffmpeg.h"
#include "HistoryStore.h"
#include "Recipe.h"
#include "StagedOutput.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

// Feature engines: everything beyond the built-in image, media and file paths plugs in here (PDF,
// archives, video and audio tools, image editors, documents...). An engine owns the actions it
// registers (StepKind::Engine) and runs the recipes made of them. Each feature lives in its own
// files; Modules.cpp is the one place that lists them.
namespace sd
{
    using ProgressFn = std::function<void(double)>;   // 0..1 within the current file, from any thread

    // What every engine is given: shared services, owned by the app and outliving every engine.
    struct EngineServices
    {
        HistoryStore* history = nullptr;            // may be null (tests, or the database failed to open)
        std::shared_ptr<const FfmpegEngine> ffmpeg; // may be null or unavailable
    };

    // Which worker runs an engine's jobs.
    enum class JobLane
    {
        Pool,    // the parallel MTA pool (CPU work: images, PDF pages)
        Media,   // the single media worker (anything that runs ffmpeg on video: one run uses every core)
        Audio,   // ffmpeg on sound only: one run uses one core, so several files run at once
        Shell,   // the STA shell thread (IFileOperation, shell COM objects)
        Office,  // an STA thread of its own for Word, Excel and PowerPoint through COM, so a long
                 // export never holds up the shell thread's moves and renames
    };

    class IEngine
    {
    public:
        virtual ~IEngine() = default;

        // Does this engine run this recipe? (Usually: its single step is one of this engine's actions.)
        virtual bool Handles(const Recipe& recipe) const = 0;
        // Is the recipe meant for this kind of file at all? The wheel shows only recipes that apply.
        virtual bool Applies(const Recipe& recipe, const std::wstring& path) const = 0;
        // Is it meant for a drop of this many files at all (a collage needs two or more)? Slices that
        // aren't are left off the wheel.
        virtual bool AppliesToDrop(const Recipe&, size_t fileCount) const
        {
            (void)fileCount;
            return true;
        }
        // Can it run on this file now? `why` explains a "no" (shown in the wheel's centre).
        virtual bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const = 0;
        // Would running it change nothing (already that format)? Such slices are dimmed.
        virtual bool IsNoOp(const Recipe&, const std::wstring&) const { return false; }

        virtual JobLane Lane(const Recipe&) const { return JobLane::Pool; }

        // Per-file work: one output per source, written through StagedOutput. Called on a worker.
        virtual ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                               const ProgressFn& progress) const = 0;

        // Whole-batch work (join videos, merge PDFs, a collage, a zip of everything): one output from
        // all the files, in drop order. Return true from IsBatch to get RunBatch instead of Run.
        virtual bool IsBatch(const Recipe&) const { return false; }
        virtual ItemResult RunBatch(const Recipe&, int64_t, const std::vector<std::wstring>&, std::stop_token, const ProgressFn&) const
        {
            return ItemResult{ false, false, {}, L"not a batch action", 0 };
        }

        // Tools that need the user first (trim range, crop box, annotations, page order) open a window
        // on the UI thread. `done` gets the recipe with the user's choices in its params, or is never
        // called if they cancel. The window is the engine's own; `owner` is the app's hidden window.
        virtual bool NeedsEditor(const Recipe&) const { return false; }
        virtual void OpenEditor(HWND owner, const Recipe& recipe, const std::vector<std::wstring>& files,
                                std::function<void(Recipe adjusted, std::vector<std::wstring> files)> done) const
        {
            (void)owner, (void)recipe, (void)files, (void)done;
        }

        // An editor that opens on something made from the files instead of the files themselves (Edit
        // lays a Word document out as PDF pages first). The recipe that makes it: the app runs it on the
        // files as a batch of its own, with the top panel's progress and ×, and a file it fails on is
        // reported as any failed run is. Each file's output (the file itself when nothing needed making)
        // then goes to OpenEditorOn. No value: OpenEditor gets the files as they are.
        virtual std::optional<Recipe> EditorInput(const Recipe& recipe, const std::vector<std::wstring>& files) const
        {
            (void)recipe, (void)files;
            return std::nullopt;
        }
        // After EditorInput's batch, on the UI thread: `made[i]` is what it made from `files[i]`, empty
        // where that failed. `cancelled` (the ×): only remove what was made; nothing opens.
        virtual void OpenEditorOn(HWND owner, const Recipe& recipe, const std::vector<std::wstring>& files, const std::vector<std::wstring>& made,
                                  bool cancelled, std::function<void(Recipe adjusted, std::vector<std::wstring> files)> done) const
        {
            (void)owner, (void)recipe, (void)files, (void)made, (void)cancelled, (void)done;
        }
    };

    // Everything Modules.cpp knows: the engines' actions and built-in recipes, and the engines.
    struct FeatureModule
    {
        const char* name;                                       // "pdf", for logs
        void (*registerActions)(ActionRegistry& registry);      // ActionRegistry::Add for each action
        const char* (*builtInRecipes)();                        // a JSON array, like RecipeStore::BuiltInJson; may be null
        std::unique_ptr<IEngine> (*makeEngine)(const EngineServices& services);   // may be null (actions handled elsewhere)
    };
    const std::vector<FeatureModule>& FeatureModules();

    // Used by the core (ActionRegistry, RecipeStore, PipelineRunner) to pull every module in.
    void RegisterModuleActions(ActionRegistry& registry);
    void AppendModuleRecipes(nlohmann::json& builtIns);
    std::vector<std::unique_ptr<IEngine>> MakeModuleEngines(const EngineServices& services);
}
