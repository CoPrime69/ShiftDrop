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
    // A tool's own built-in recipe (id, label, suffix, icon, action), for one picked in the editor's
    // tabs. Null for a tool of the other kind.
    nlohmann::json ToolRecipe(mediatools::EditorTool tool);

    bool NeedsEditor(const RecipeStep& step);

    ItemResult Run(const EngineServices& services, const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                   const ProgressFn& progress);
}
