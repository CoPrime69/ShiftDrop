#include "Recipe.h"
#include "Utf.h"

#include <cctype>
#include <cmath>
#include <format>

namespace sd
{
    namespace
    {
        using nlohmann::json;

        bool CheckParam(const ParamSpec& spec, const json& value, const std::string& where, std::string& error)
        {
            switch (spec.type)
            {
            case ParamType::Bool:
                if (!value.is_boolean())
                {
                    error = where + ": expected true or false";
                    return false;
                }
                return true;
            case ParamType::Enum:
            {
                if (!value.is_string())
                {
                    error = where + ": expected a string";
                    return false;
                }
                const auto s = value.get<std::string>();
                for (const auto& c : spec.choices)
                    if (c == s)
                        return true;
                std::string allowed;
                for (const auto& c : spec.choices)
                    allowed += (allowed.empty() ? "" : ", ") + c;
                error = std::format("{}: '{}' is not one of {}", where, s, allowed);
                return false;
            }
            case ParamType::Int:
            case ParamType::Number:
            {
                if (!value.is_number())
                {
                    error = where + ": expected a number";
                    return false;
                }
                const double v = value.get<double>();
                if (spec.type == ParamType::Int && std::floor(v) != v)
                {
                    error = std::format("{}: {} is not a whole number", where, v);
                    return false;
                }
                if (v < spec.min)
                {
                    error = std::format("{}: {} is below {}", where, v, spec.min);
                    return false;
                }
                if (v > spec.max)
                {
                    error = std::format("{}: {} is above {}", where, v, spec.max);
                    return false;
                }
                return true;
            }
            }
            return false;
        }
    }

    bool ParseRecipe(std::string_view text, const ActionRegistry& registry, Recipe& out, std::string& error)
    {
        json doc = json::parse(text, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
        if (doc.is_discarded() || !doc.is_object())
        {
            error = "not a JSON object";
            return false;
        }

        static const char* const kKnown[] = { "$schema", "id", "label", "description", "ring", "suffix", "steps" };
        for (const auto& [key, _] : doc.items())
        {
            bool known = false;
            for (const char* k : kKnown)
                known = known || key == k;
            if (!known)
            {
                error = std::format("unknown field '{}'", key);
                return false;
            }
        }

        Recipe r;
        if (!doc.contains("id") || !doc["id"].is_string() || doc["id"].get<std::string>().empty())
        {
            error = "id: required, a non-empty string";
            return false;
        }
        r.id = doc["id"].get<std::string>();
        for (const char c : r.id)
        {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.'))
            {
                error = "id: use letters, digits, '-', '_' or '.' only";
                return false;
            }
        }
        if (!doc.contains("label") || !doc["label"].is_string() || doc["label"].get<std::string>().empty())
        {
            error = "label: required, a non-empty string";
            return false;
        }
        r.label = Widen(doc["label"].get<std::string>());
        if (doc.contains("description"))
        {
            if (!doc["description"].is_string())
            {
                error = "description: expected a string";
                return false;
            }
            r.description = Widen(doc["description"].get<std::string>());
        }
        if (doc.contains("ring"))
        {
            const std::string ring = doc["ring"].is_string() ? doc["ring"].get<std::string>() : "";
            if (ring != "convert" && ring != "tools" && ring != "files")
            {
                error = "ring: must be \"convert\", \"tools\" or \"files\"";
                return false;
            }
            r.ring = ring;
        }
        if (doc.contains("suffix"))
        {
            if (!doc["suffix"].is_string() || doc["suffix"].get<std::string>().empty())
            {
                error = "suffix: expected a non-empty string";
                return false;
            }
            r.suffix = Widen(doc["suffix"].get<std::string>());
            if (r.suffix.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
            {
                error = "suffix: contains a character that is not allowed in file names";
                return false;
            }
        }

        if (!doc.contains("steps") || !doc["steps"].is_array() || doc["steps"].empty())
        {
            error = "steps: required, a non-empty array";
            return false;
        }
        const auto& steps = doc["steps"];
        for (size_t i = 0; i < steps.size(); ++i)
        {
            const auto where = std::format("steps[{}]", i);
            const auto& s = steps[i];
            if (!s.is_object() || !s.contains("action") || !s["action"].is_string())
            {
                error = where + ": needs an \"action\" string";
                return false;
            }
            RecipeStep step;
            step.action = s["action"].get<std::string>();
            const auto* desc = registry.Find(step.action);
            if (!desc)
            {
                error = std::format("{}: unknown action '{}'", where, step.action);
                return false;
            }
            for (const auto& [key, _] : s.items())
            {
                if (key != "action" && key != "params")
                {
                    error = std::format("{}: unknown field '{}'", where, key);
                    return false;
                }
            }
            const json given = s.contains("params") ? s["params"] : json::object();
            if (!given.is_object())
            {
                error = where + ".params: expected an object";
                return false;
            }
            for (const auto& [key, _] : given.items())
            {
                bool declared = false;
                for (const auto& p : desc->params)
                    declared = declared || p.key == key;
                if (!declared)
                {
                    error = std::format("{}.params: '{}' is not a parameter of {}", where, key, step.action);
                    return false;
                }
            }
            step.params = json::object();
            for (const auto& p : desc->params)
            {
                const json value = given.contains(p.key) ? given[p.key] : p.defaultValue;
                if (!CheckParam(p, value, std::format("{}.params.{}", where, p.key), error))
                    return false;
                step.params[p.key] = value;
            }

            const bool last = i + 1 == steps.size();
            if (desc->kind == StepKind::FileAction || desc->kind == StepKind::MediaEncode)
            {
                if (steps.size() != 1)
                {
                    error = std::format("{}: {} must be the recipe's only step", where, desc->kind == StepKind::FileAction ? "a file action" : step.action);
                    return false;
                }
                r.steps.push_back(std::move(step));
                continue;
            }
            if (desc->kind == StepKind::ImageEncode && !last)
            {
                error = std::format("{}: {} must be the last step", where, step.action);
                return false;
            }
            if (last && desc->kind != StepKind::ImageEncode)
            {
                error = where + ": the last step must be image.encode (or image.compress)";
                return false;
            }
            r.steps.push_back(std::move(step));
        }

        out = std::move(r);
        return true;
    }

    std::string Recipe::ToJson() const
    {
        nlohmann::json doc;
        doc["id"] = id;
        doc["label"] = Narrow(label);
        if (!description.empty())
            doc["description"] = Narrow(description);
        doc["ring"] = ring;
        doc["suffix"] = Narrow(suffix);
        doc["steps"] = nlohmann::json::array();
        for (const auto& s : steps)
            doc["steps"].push_back({ { "action", s.action }, { "params", s.params } });
        return doc.dump();
    }

    const ImageFormatInfo* Recipe::OutputFormatFor(const std::wstring& ext) const
    {
        if (steps.empty())
            return nullptr;
        if (steps.back().action == "image.compress")
        {
            // WebP stays WebP; everything else becomes JPEG, the format every upload box takes.
            const auto* own = FormatOfExtension(ext);
            return own && std::string_view(own->id) == "webp" ? own : FindImageFormat("jpg");
        }
        if (steps.back().action != "image.encode")
            return nullptr;
        const auto formatId = steps.back().params.value("format", std::string{});
        if (formatId == "same")
        {
            const auto* own = FormatOfExtension(ext);
            return own ? own : FindImageFormat("png");
        }
        return FindImageFormat(formatId);
    }
}

namespace sd
{
    const MediaFormatInfo* Recipe::MediaFormat() const
    {
        return IsMedia() && steps[0].action == "media.convert" ? FindMediaFormat(steps[0].params.value("format", std::string{})) : nullptr;
    }

    std::wstring Recipe::OutputExtensionFor(const std::wstring& ext) const
    {
        if (const auto* media = MediaFormat())
            return media->extension;
        if (IsMediaCompress())
            return MediaKindOfExtension(ext) == MediaKind::Audio ? L".m4a" : L".mp4";
        if (const auto* image = OutputFormatFor(ext))
            return image->extension;
        return {};
    }
}
