#include "RecipeStore.h"
#include "Engine.h"
#include "FileIo.h"
#include "Log.h"
#include "Utf.h"

#include <windows.h>
#include <wil/resource.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>

namespace sd
{
    std::string RecipeStore::BuiltInJson()
    {
        // The plain format slices are one-step recipes: decode, then encode in the target format.
        auto all = nlohmann::json::parse(R"([
  { "id": "to-png",  "label": "PNG",  "description": "PNG, lossless",  "steps": [ { "action": "image.encode", "params": { "format": "png" } } ] },
  { "id": "to-jpg",  "label": "JPG",  "description": "JPEG, quality 92",  "steps": [ { "action": "image.encode", "params": { "format": "jpg", "quality": 92 } } ] },
  { "id": "to-webp", "label": "WebP", "description": "WebP, quality 85", "steps": [ { "action": "image.encode", "params": { "format": "webp", "quality": 85 } } ] },
  { "id": "to-avif", "label": "AVIF", "description": "AVIF, quality 60", "steps": [ { "action": "image.encode", "params": { "format": "avif", "quality": 60 } } ] },
  { "id": "to-heic", "label": "HEIC", "description": "HEIC, quality 80, through Windows' HEIF codec", "steps": [ { "action": "image.encode", "params": { "format": "heic", "quality": 80 } } ] },
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
  { "id": "compress-image", "label": "Compress", "description": "Smaller JPEG (WebP and AVIF stay), quality 75", "ring": "tools", "suffix": "compressed",
    "steps": [ { "action": "image.compress", "params": { "quality": 75 } } ] },
  { "id": "image-under-1mb", "label": "Under 1 MB", "description": "Best quality that fits in 1 MB", "ring": "tools", "suffix": "compressed",
    "steps": [ { "action": "image.compress", "params": { "maxKB": 1024 } } ] },

  { "id": "compress-media", "label": "Compress", "description": "H.264 + AAC at a smaller size, up to 1080p", "ring": "tools", "suffix": "compressed",
    "steps": [ { "action": "media.compress", "params": { "maxHeight": 1080 } } ] },
  { "id": "media-half", "label": "50% size", "description": "Half the file size, guaranteed", "ring": "tools", "suffix": "compressed",
    "steps": [ { "action": "media.compress", "params": { "percent": 50 } } ] },
  { "id": "media-under-10mb", "label": "Under 10 MB", "description": "Fits in 10 MB, for chat apps", "ring": "tools", "suffix": "compressed",
    "steps": [ { "action": "media.compress", "params": { "targetMB": 10 } } ] },
  { "id": "media-under-25mb", "label": "Under 25 MB", "description": "Fits in 25 MB, for email", "ring": "tools", "suffix": "compressed",
    "steps": [ { "action": "media.compress", "params": { "targetMB": 25 } } ] },

  { "id": "copy",      "label": "Copy",      "description": "Copy to the clipboard", "ring": "files", "steps": [ { "action": "file.copy" } ] },
  { "id": "move",      "label": "Move to\u2026", "description": "Move to a folder you pick", "ring": "files", "steps": [ { "action": "file.move" } ] },
  { "id": "copy-path", "label": "Copy path", "description": "Copy the full path", "ring": "files", "steps": [ { "action": "file.copy_path" } ] },
  { "id": "terminal",  "label": "Terminal",  "description": "Open a terminal here", "ring": "files", "steps": [ { "action": "file.terminal" } ] },
  { "id": "duplicate", "label": "Duplicate", "description": "Make a copy next to it", "ring": "files", "suffix": "copy", "steps": [ { "action": "file.duplicate" } ] },
  { "id": "rename",    "label": "Rename\u2026", "description": "Rename with a pattern: {name}, {n}, {date}", "ring": "files", "steps": [ { "action": "file.rename" } ] }
])");
        // Video and audio formats, one Convert slice each, in MediaFormats() order. They share the
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
        // Reduce size: 90% to 20% smaller, for pictures and for video and audio (PDFs: PdfModule). Each
        // result is at most that share of the source's size; the wheel shows which set fits the files.
        for (int smaller = 90; smaller >= 20; smaller -= 10)
        {
            const auto add = [&](const char* kind, const char* action) {
                nlohmann::json recipe = nlohmann::json::object();
                recipe["id"] = std::format("reduce-{}-{}", kind, smaller);
                recipe["label"] = std::format("\xE2\x88\x92{}%", smaller);   // U+2212 minus, as UTF-8
                recipe["description"] = std::format("{}% smaller", smaller);
                recipe["ring"] = "reduce";
                recipe["suffix"] = "reduced";
                nlohmann::json step = nlohmann::json::object();
                step["action"] = action;
                step["params"]["percent"] = 100 - smaller;
                recipe["steps"] = nlohmann::json::array({ step });
                all.push_back(std::move(recipe));
            };
            add("image", "image.compress");
            add("media", "media.compress");
        }
        AppendModuleRecipes(all);   // each feature module's own slices (Modules.cpp)
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
            std::string text;
            ReadFileBytes(path.wstring(), text);   // an unreadable file parses as empty and is reported below
            Recipe r;
            std::string error;
            if (!ParseRecipe(text, registry, r, error))
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
