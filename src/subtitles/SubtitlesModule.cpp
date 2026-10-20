#include "subtitles/SubtitlesModule.h"
#include "subtitles/Subtitles.h"
#include "docs/Bytes.h"
#include "OutputPath.h"

#include <windows.h>

namespace sd::subtitles
{
    namespace
    {
        // Subtitle files are small; anything bigger is not one.
        constexpr uint64_t kMaxBytes = 64ull * 1024 * 1024;

        std::string TargetOf(const Recipe& r)
        {
            return r.steps.empty() ? std::string() : r.steps[0].params.value("format", std::string());
        }

        const wchar_t* ExtensionOf(std::string_view target)
        {
            if (target == "srt")
                return L".srt";
            if (target == "vtt")
                return L".vtt";
            if (target == "txt")
                return L".txt";
            return nullptr;
        }

        class SubtitlesEngine final : public IEngine
        {
        public:
            explicit SubtitlesEngine(const EngineServices& services) : m_history(services.history) {}

            bool Handles(const Recipe& recipe) const override
            {
                return recipe.steps.size() == 1 && recipe.steps[0].action == "subtitles.convert";
            }

            bool Applies(const Recipe& recipe, const std::wstring& path) const override
            {
                return TargetFits(path, TargetOf(recipe));
            }

            bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const override
            {
                if (!TargetFits(path, TargetOf(recipe)))
                {
                    if (why)
                        *why = L"not a subtitle file";
                    return false;
                }
                WIN32_FILE_ATTRIBUTE_DATA data{};
                if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
                {
                    if (why)
                        *why = L"file not found";
                    return false;
                }
                if ((static_cast<uint64_t>(data.nFileSizeHigh) << 32 | data.nFileSizeLow) > kMaxBytes)
                {
                    if (why)
                        *why = L"too large for a subtitle file";
                    return false;
                }
                return true;
            }

            ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop, const ProgressFn& progress) const override
            {
                const auto target = TargetOf(recipe);
                StagedOutput out(m_history, runId, source);
                if (!out.Ready())
                    return out.Fail(out.Error());
                std::string bytes;
                std::wstring error;
                if (!docs::ReadFileBytes(source, bytes, &error))
                    return out.Fail(error);
                if (stop.stop_requested())
                    return out.Fail(L"cancelled", true);
                if (progress)
                    progress(0.3);

                const auto track = Parse(docs::DecodeText(bytes));
                if (track.cues.empty())
                    return out.Fail(L"no subtitles found in the file");
                std::string result;
                if (target == "srt")
                    result = WriteSrt(track);
                else if (target == "vtt")
                    result = WriteVtt(track);
                else
                    result = WriteText(track);
                if (!docs::WriteFileBytes(out.Path(), result, &error))
                    return out.Fail(error);
                if (progress)
                    progress(1.0);
                return out.Commit(recipe.suffix, ExtensionOf(target));
            }

        private:
            HistoryStore* m_history;
        };

        void RegisterActions(ActionRegistry& registry)
        {
            registry.Add(ActionDescriptor{ "subtitles.convert", L"Convert subtitles", StepKind::Engine,
                                           { ParamSpec{ "format", ParamType::Enum, 0, 0, { "srt", "vtt", "txt" }, "srt" } } });
        }

        const char* Recipes()
        {
            // Icons (Segoe Fluent Icons): ClosedCaption E7F0, the text icon the documents area uses E8D2.
            return R"([
  { "id": "subs-to-srt", "label": "SRT", "description": "SubRip subtitles", "icon": "E7F0",
    "steps": [ { "action": "subtitles.convert", "params": { "format": "srt" } } ] },
  { "id": "subs-to-vtt", "label": "VTT", "description": "WebVTT subtitles for the web", "icon": "E7F0",
    "steps": [ { "action": "subtitles.convert", "params": { "format": "vtt" } } ] },
  { "id": "subs-to-txt", "label": "TXT", "description": "Just the words, one line per subtitle", "icon": "E8D2",
    "steps": [ { "action": "subtitles.convert", "params": { "format": "txt" } } ] }
])";
        }

        std::unique_ptr<IEngine> MakeEngine(const EngineServices& services)
        {
            return std::make_unique<SubtitlesEngine>(services);
        }
    }

    bool TargetFits(const std::wstring& path, std::string_view target)
    {
        const auto ext = LowerExtension(path);
        if (ext != L".srt" && ext != L".vtt")
            return false;
        if (!ExtensionOf(target))
            return false;
        return ext != (target == "srt" ? L".srt" : target == "vtt" ? L".vtt" : L".txt");
    }

    FeatureModule Module()
    {
        return FeatureModule{ "subtitles", &RegisterActions, &Recipes, &MakeEngine };
    }
}
