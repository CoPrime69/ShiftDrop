#pragma once
#include "Action.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace sd
{
    struct RecipeStep
    {
        std::string action;        // ActionDescriptor::id
        nlohmann::json params;     // object; every declared param present after ParseRecipe (defaults filled in)
    };

    // A named chain of steps that appears as one slice. Stored as JSON (see docs/recipes.md).
    struct Recipe
    {
        std::string id;
        std::wstring label;          // slice text
        std::wstring description;
        std::string ring = "convert";   // which wheel shows it: "convert" (Shift), "tools" (Ctrl), "files" (Alt)
        std::wstring suffix = L"converted";   // output "name (suffix).ext"
        std::vector<RecipeStep> steps;
        bool builtIn = false;
        std::wstring sourcePath;     // file it came from; empty for built-ins

        std::string ToJson() const;  // canonical form, snapshotted into history
        // The format the encode step writes for this source (resolves "same"). nullptr for file actions.
        const ImageFormatInfo* OutputFormatFor(const std::wstring& lowerSourceExtension) const;
        bool IsSingleEncode() const { return steps.size() == 1 && steps[0].action == "image.encode"; }
        bool IsFileAction() const { return steps.size() == 1 && steps[0].action.starts_with("file."); }
    };

    // Parses and validates a recipe against the registry. On failure returns false and sets `error`
    // to a message that names the offending field, e.g. "steps[1].params.quality: 140 is above 100".
    bool ParseRecipe(std::string_view json, const ActionRegistry& registry, Recipe& out, std::string& error);
}
