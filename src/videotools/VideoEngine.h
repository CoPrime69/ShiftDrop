#pragma once
#include "Action.h"
#include "Engine.h"
#include "Recipe.h"
#include "mediaeditor/EditorApi.h"

#include <stop_token>
#include <string>
#include <vector>

// The video tools' actions (video.*), their built-in slices, and how each one runs. The feature
// module (mediaeditor/MediaToolsModule.cpp) wires them into the app next to the audio tools.
namespace sd::videotools
{
    void RegisterActions(ActionRegistry& registry);
    const char* BuiltInRecipes();   // JSON array of Tools-wheel slices

    bool IsBatchAction(const std::string& action);         // video.join: one output from every file
    bool NeedsEditor(const RecipeStep& step);              // its "ask" parameter, or always (frames, redact)
    mediatools::EditorTool EditorToolFor(const std::string& action);

    ItemResult Run(const EngineServices& services, const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                   const ProgressFn& progress);
    ItemResult RunBatch(const EngineServices& services, const Recipe& recipe, int64_t runId, const std::vector<std::wstring>& sources,
                        std::stop_token stop, const ProgressFn& progress);
}
