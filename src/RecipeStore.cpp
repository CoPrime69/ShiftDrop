#include "RecipeStore.h"
#include "Log.h"
#include "Utf.h"

#include <windows.h>
#include <wil/resource.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sd
{
    std::string RecipeStore::BuiltInJson()
    {
        // One-step recipes reproduce the M1 slices exactly (plus WebP, now that libwebp is linked).
        auto all = nlohmann::json::parse(R"([
  { "id": "to-png",  "label": "PNG",  "description": "PNG, lossless",  "steps": [ { "action": "image.encode", "params": { "format": "png" } } ] },
  { "id": "to-jpg",  "label": "JPG",  "description": "JPEG, quality 92",  "steps": [ { "action": "image.encode", "params": { "format": "jpg", "quality": 92 } } ] },
  { "id": "to-webp", "label": "WebP", "description": "WebP, quality 85", "steps": [ { "action": "image.encode", "params": { "format": "webp", "quality": 85 } } ] },
  { "id": "to-tiff", "label": "TIFF", "description": "TIFF, lossless", "steps": [ { "action": "image.encode", "params": { "format": "tiff" } } ] },
  { "id": "to-bmp",  "label": "BMP",  "description": "Bitmap, uncompressed",  "steps": [ { "action": "image.encode", "params": { "format": "bmp" } } ] },

  { "id": "rotate-right", "label": "Rotate \u21bb", "description": "Rotate 90\u00b0 clockwise", "ring": "tools", "suffix": "edited",
    "steps": [ { "action": "image.rotate", "params": { "degrees": "90" } }, { "action": "image.encode", "params": { "format": "same" } } ] },
  { "id": "max-1920", "label": "1920 px", "description": "Fit within 1920 px", "ring": "tools", "suffix": "edited",
    "steps": [ { "action": "image.resize", "params": { "maxEdge": 1920 } }, { "action": "image.encode", "params": { "format": "same" } } ] },
  { "id": "flip", "label": "Flip", "description": "Mirror left to right", "ring": "tools", "suffix": "edited",
    "steps": [ { "action": "image.flip", "params": { "axis": "horizontal" } }, { "action": "image.encode", "params": { "format": "same" } } ] },
  { "id": "half-size", "label": "50%", "description": "Half the width and height", "ring": "tools", "suffix": "edited",
    "steps": [ { "action": "image.scale", "params": { "percent": 50 } }, { "action": "image.encode", "params": { "format": "same" } } ] },
  { "id": "rotate-left", "label": "Rotate \u21ba", "description": "Rotate 90\u00b0 counter-clockwise", "ring": "tools", "suffix": "edited",
    "steps": [ { "action": "image.rotate", "params": { "degrees": "270" } }, { "action": "image.encode", "params": { "format": "same" } } ] },
  { "id": "max-1080", "label": "1080 px", "description": "Fit within 1080 px", "ring": "tools", "suffix": "edited",
    "steps": [ { "action": "image.resize", "params": { "maxEdge": 1080 } }, { "action": "image.encode", "params": { "format": "same" } } ] },

  { "id": "copy",      "label": "Copy",      "description": "Copy to the clipboard", "ring": "files", "steps": [ { "action": "file.copy" } ] },
  { "id": "move",      "label": "Move to\u2026", "description": "Move to a folder you pick", "ring": "files", "steps": [ { "action": "file.move" } ] },
  { "id": "copy-path", "label": "Copy path", "description": "Copy the full path", "ring": "files", "steps": [ { "action": "file.copy_path" } ] },
  { "id": "terminal",  "label": "Terminal",  "description": "Open a terminal here", "ring": "files", "steps": [ { "action": "file.terminal" } ] },
  { "id": "duplicate", "label": "Duplicate", "description": "Make a copy next to it", "ring": "files", "suffix": "copy", "steps": [ { "action": "file.duplicate" } ] }
])");
        // Video and audio formats (M3), one Convert slice each, in MediaFormats() order. They share the
        // Convert wheel with the image formats; the wheel shows only those that fit the dragged files.
        for (const auto& f : MediaFormats())
        {
            nlohmann::json step = nlohmann::json::object();
            step["action"] = "media.convert";
            step["params"]["format"] = f.id;
            nlohmann::json recipe = nlohmann::json::object();
            recipe["id"] = std::string("to-") + f.id;
            recipe["label"] = Narrow(f.label);
            recipe["description"] = Narrow(f.description);
            recipe["steps"] = nlohmann::json::array({ step });
            all.push_back(std::move(recipe));
        }
        return all.dump();
    }

    const char* RecipeStore::ExampleJson()
    {
        return R"({
  // Example recipe. Edit or copy this file; ShiftDrop reads every *.json in this folder at startup.
  // Reference: docs/recipes.md in the ShiftDrop repository.
  "id": "web-ready",
  "label": "Web",
  "description": "Resize to at most 2048 px, encode WebP quality 80, no metadata",
  "steps": [
    { "action": "image.resize", "params": { "maxEdge": 2048 } },
    { "action": "image.strip_metadata" },
    { "action": "image.encode", "params": { "format": "webp", "quality": 80 } }
  ]
}
)";
    }

    void RecipeStore::Load(const ActionRegistry& registry, const std::wstring& folder)
    {
        m_recipes.clear();
        m_errors.clear();

        const auto builtIns = nlohmann::json::parse(BuiltInJson());
        for (const auto& item : builtIns)
        {
            Recipe r;
            std::string error;
            if (ParseRecipe(item.dump(), registry, r, error))
            {
                r.builtIn = true;
                m_recipes.push_back(std::move(r));
            }
            else
            {
                log::Error(L"recipes: built-in is invalid: {}", Widen(error));   // a bug, caught by tests
            }
        }

        std::error_code ec;
        if (!std::filesystem::exists(folder, ec))
        {
            std::filesystem::create_directories(folder, ec);
            std::ofstream(std::filesystem::path(folder) / L"web-ready.json", std::ios::binary) << ExampleJson();
            log::Info(L"recipes: created {} with an example recipe", folder);
        }

        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
            if (entry.is_regular_file() && entry.path().extension() == L".json")
                files.push_back(entry.path());
        std::sort(files.begin(), files.end());

        for (const auto& path : files)
        {
            std::ifstream in(path, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            Recipe r;
            std::string error;
            if (!ParseRecipe(text.str(), registry, r, error))
            {
                m_errors.push_back(std::format(L"{}: {}", path.filename().wstring(), Widen(error)));
                log::Warn(L"recipes: {} ignored: {}", path.wstring(), Widen(error));
                continue;
            }
            r.sourcePath = path.wstring();
            auto same = std::find_if(m_recipes.begin(), m_recipes.end(), [&](const Recipe& x) { return x.id == r.id; });
            if (same != m_recipes.end())
            {
                log::Info(L"recipes: {} replaces '{}'", path.filename().wstring(), Widen(r.id));
                *same = std::move(r);
            }
            else
            {
                m_recipes.push_back(std::move(r));
            }
        }
        log::Info(L"recipes: {} loaded, {} with errors", m_recipes.size(), m_errors.size());
    }

    const Recipe* RecipeStore::Find(std::string_view id) const
    {
        for (const auto& r : m_recipes)
            if (r.id == id)
                return &r;
        return nullptr;
    }
}
