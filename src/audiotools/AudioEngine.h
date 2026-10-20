#pragma once
#include "Action.h"
#include "Engine.h"
#include "Recipe.h"
#include "mediaeditor/EditorApi.h"

#include <stop_token>
#include <string>

// The audio tools' actions (audio.*), their built-in slices, and how each one runs.
namespace sd::audiotools
{
    void RegisterActions(ActionRegistry& registry);
    const char* BuiltInRecipes();   // JSON array of Tools-wheel slices

    bool NeedsEditor(const RecipeStep& step);
    mediatools::EditorTool EditorToolFor(const std::string& action);

    ItemResult Run(const EngineServices& services, const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                   const ProgressFn& progress);
}
