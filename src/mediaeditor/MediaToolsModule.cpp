#include "mediaeditor/MediaToolsModule.h"
#include "Log.h"
#include "Media.h"
#include "OutputPath.h"
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
                mediatools::EditorRequest request;
                const auto& action = recipe.steps[0].action;
                request.tool = IsVideoAction(recipe) ? videotools::EditorToolFor(action) : audiotools::EditorToolFor(action);
                request.title = recipe.description.empty() ? recipe.label : recipe.description.substr(0, recipe.description.find(L':'));
                request.files = files;
                request.params = recipe.steps[0].params;
                request.ffmpeg = m_s.ffmpeg;
                // The editor's choices go into the recipe's parameters; "ask" is spent.
                mediatools::OpenEditor(owner, std::move(request), [recipe, done](nlohmann::json params, std::vector<std::wstring> chosen) {
                    Recipe adjusted = recipe;
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
