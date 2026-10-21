#pragma once
#include "Action.h"
#include "Media.h"

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
        std::string ring = "convert";   // which section shows it: "convert", "tools", "reduce" or "files"
        std::wstring suffix = L"converted";   // output "name (suffix).ext"
        wchar_t icon = 0;            // optional Segoe Fluent Icons glyph for the slice ("icon": "E7AD")
        std::vector<RecipeStep> steps;
        bool builtIn = false;
        std::wstring sourcePath;     // file it came from; empty for built-ins

        std::string ToJson() const;  // canonical form, snapshotted into history
        // The format the encode step writes for this source (resolves "same"). nullptr for file actions.
        const ImageFormatInfo* OutputFormatFor(const std::wstring& lowerSourceExtension) const;
        // The media format a media.convert recipe writes; nullptr for image recipes and file actions.
        const MediaFormatInfo* MediaFormat() const;
        // The output file's extension for this source: the encoded image or media format. Empty for
        // file actions.
        std::wstring OutputExtensionFor(const std::wstring& lowerSourceExtension) const;
        bool IsSingleEncode() const { return steps.size() == 1 && steps[0].action == "image.encode"; }
        bool IsMedia() const { return steps.size() == 1 && steps[0].action.starts_with("media."); }   // runs through ffmpeg
        bool IsMediaCompress() const { return steps.size() == 1 && steps[0].action == "media.compress"; }
        // Compress / Fit to… recipes: their results are for sending somewhere, so they can be carried.
        bool IsCompress() const { return IsMediaCompress() || (!steps.empty() && steps.back().action == "image.compress"); }
        bool IsFileAction() const { return steps.size() == 1 && steps[0].action.starts_with("file."); }
        // Its wheel slice is its label alone, in bold (format names and media presets say it best);
        // every other slice has an icon above its label.
        bool NameOnlySlice() const { return !icon && (IsSingleEncode() || IsMedia() || steps.empty()); }
        // The first step's text param `key` ("format": what a document or office recipe makes), or empty.
        std::string FirstParam(const char* key) const;
    };

    // Parses and validates a recipe against the registry. On failure returns false and sets `error`
    // to a message that names the offending field, e.g. "steps[1].params.quality: 140 is above 100".
    bool ParseRecipe(std::string_view json, const ActionRegistry& registry, Recipe& out, std::string& error);

    // Readers for a step's params that never throw: a missing or non-number value gives `fallback`.
    // The clamped form also gives `fallback` for NaN and infinity.
    double JsonNumber(const nlohmann::json& j, const char* key, double fallback);
    double JsonNumber(const nlohmann::json& j, const char* key, double fallback, double lo, double hi);

    // A step's params for one of its files: the shared params with that file's own entry of
    // "perFile" ({"perFile": {path: {...}}}) merged over them, so one run carries per-file edits.
    nlohmann::json ParamsForFile(const nlohmann::json& params, const std::wstring& path);

    // The recipe in a JSON list whose first step runs `action` (an editor's tool recipes), or null.
    nlohmann::json FindToolRecipe(const nlohmann::json& list, std::string_view action);
}
