#include "mediaeditor/MediaToolsModule.h"
#include "Log.h"
#include "Media.h"
#include "OutputPath.h"
#include "Utf.h"
#include "audiotools/AudioEngine.h"
#include "mediaeditor/EditorApi.h"
#include "videotools/VideoEngine.h"

#include <format>

namespace sd
{
    namespace
    {
        bool IsVideoAction(const Recipe& r) { return r.steps.size() == 1 && r.steps[0].action.starts_with("video."); }
        bool IsAudioAction(const Recipe& r) { return r.steps.size() == 1 && r.steps[0].action.starts_with("audio."); }

        // The editor's tool bar picked another tool: the run becomes that tool's own recipe (its name
        // in history and the notification, its output suffix), with the editor's choices as params.
        void SwitchTool(Recipe& r, const std::string& action)
        {
            mediatools::EditorTool tool{};
            if (!mediatools::ToolForAction(action, tool))
                return;
            const auto t = mediatools::IsVideoTool(tool) ? videotools::ToolRecipe(tool) : audiotools::ToolRecipe(tool);
            if (!t.is_object())
                return;
            r.id = t.value("id", r.id);
            r.label = Widen(t.value("label", std::string()));
            r.description = Widen(t.value("description", std::string()));
            r.suffix = Widen(t.value("suffix", std::string()));
            r.icon = static_cast<wchar_t>(std::stoul(t.value("icon", std::string("0")), nullptr, 16));
            r.builtIn = true;
            r.sourcePath.clear();
            r.steps[0].action = action;
        }

        class MediaToolsEngine final : public IEngine
        {
        public:
            explicit MediaToolsEngine(EngineServices services) : m_s(std::move(services)) {}

            bool Handles(const Recipe& recipe) const override { return IsVideoAction(recipe) || IsAudioAction(recipe); }

            bool Applies(const Recipe& recipe, const std::wstring& path) const override
            {
                const auto kind = MediaKindOfExtension(LowerExtension(path));
                return IsVideoAction(recipe) ? kind == MediaKind::Video : kind == MediaKind::Audio;
            }

            bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const override
            {
                const auto say = [why](const wchar_t* text) {
                    if (why)
                        *why = text;
                    return false;
                };
                if (!m_s.ffmpeg || !m_s.ffmpeg->Available())
                    return say(L"the media engine is missing; reinstall to edit video and audio");
                if (!Applies(recipe, path))
                    return say(IsVideoAction(recipe) ? L"not a video" : L"not an audio file");
                if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                    return say(L"file not found");
                return true;
            }

            JobLane Lane(const Recipe&) const override { return JobLane::Media; }

            ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop, const ProgressFn& progress) const override
            {
                // A per-file editor's choices (each file's trim range or boxes) are keyed by path.
                Recipe forFile = recipe;
                forFile.steps[0].params = mediatools::ParamsForFile(recipe.steps[0].params, source);
                return IsVideoAction(forFile) ? videotools::Run(m_s, forFile, runId, source, stop, progress)
                                              : audiotools::Run(m_s, forFile, runId, source, stop, progress);
            }

            bool IsBatch(const Recipe& recipe) const override { return IsVideoAction(recipe) && videotools::IsBatchAction(recipe.steps[0].action); }

            ItemResult RunBatch(const Recipe& recipe, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token stop,
                                const ProgressFn& progress) const override
            {
                return videotools::RunBatch(m_s, recipe, runId, sources, stop, progress);
            }

            bool NeedsEditor(const Recipe& recipe) const override
            {
                return IsVideoAction(recipe) ? videotools::NeedsEditor(recipe.steps[0]) : audiotools::NeedsEditor(recipe.steps[0]);
            }

            void OpenEditor(HWND owner, const Recipe& recipe, const std::vector<std::wstring>& files,
                            std::function<void(Recipe adjusted, std::vector<std::wstring> files)> done) const override
            {
                if (recipe.steps[0].action == "audio.normalize")
                {
                    double target = -16;
                    if (!audiotools::ChooseLoudness(owner, files.size(), target))
                        return;
                    Recipe adjusted = recipe;
                    adjusted.steps[0].params["target"] = target;
                    adjusted.steps[0].params["ask"] = false;
                    done(std::move(adjusted), files);
                    return;
                }
                mediatools::EditorRequest request;
                if (!mediatools::ToolForAction(recipe.steps[0].action, request.tool))
                    return;
                request.title = recipe.description.empty() ? recipe.label : recipe.description.substr(0, recipe.description.find(L':'));
                request.files = files;
                request.params = recipe.steps[0].params;
                request.ffmpeg = m_s.ffmpeg;
                // The editor's choices go into the recipe's parameters; "ask" is spent.
                mediatools::OpenEditor(owner, std::move(request), [recipe, done](nlohmann::json params, std::vector<std::wstring> chosen) {
                    Recipe adjusted = recipe;
                    if (const auto tool = params.value("tool", std::string()); !tool.empty() && tool != recipe.steps[0].action)
                        SwitchTool(adjusted, tool);
                    params.erase("tool");
                    params["ask"] = false;
                    adjusted.steps[0].params = std::move(params);
                    done(std::move(adjusted), std::move(chosen));
                });
            }

        private:
            EngineServices m_s;
        };

        void RegisterActions(ActionRegistry& registry)
        {
            videotools::RegisterActions(registry);
            audiotools::RegisterActions(registry);
        }

        const char* BuiltInRecipes()
        {
            // One array of both tool sets (videos and songs never see each other's slices).
            static const std::string all = [] {
                auto list = nlohmann::json::parse(videotools::BuiltInRecipes());
                for (auto& r : nlohmann::json::parse(audiotools::BuiltInRecipes()))
                    list.push_back(std::move(r));
                return list.dump();
            }();
            return all.c_str();
        }

        std::unique_ptr<IEngine> MakeEngine(const EngineServices& services) { return std::make_unique<MediaToolsEngine>(services); }
    }

    FeatureModule MediaToolsModule()
    {
        return FeatureModule{ "mediatools", &RegisterActions, &BuiltInRecipes, &MakeEngine };
    }
}
