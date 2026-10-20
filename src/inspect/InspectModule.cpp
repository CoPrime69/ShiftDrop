#include "inspect/InspectModule.h"
#include "Log.h"

#include <atomic>

namespace sd::inspect
{
    namespace
    {
        std::atomic<OpenWindow> g_open{ nullptr };

        class InspectEngine final : public IEngine
        {
        public:
            explicit InspectEngine(std::shared_ptr<const FfmpegEngine> ffmpeg) : m_ffmpeg(std::move(ffmpeg)) {}

            bool Handles(const Recipe& recipe) const override
            {
                return recipe.steps.size() == 1 && recipe.steps[0].action == kActionId;
            }
            // Every file has metadata (folders are left to the File-actions wheel).
            bool Applies(const Recipe&, const std::wstring& path) const override
            {
                const DWORD attrs = GetFileAttributesW(path.c_str());
                return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
            }
            bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const override
            {
                if (Applies(recipe, path))
                    return true;
                if (why)
                    *why = L"not a file";
                return false;
            }
            ItemResult Run(const Recipe&, int64_t, const std::wstring&, std::stop_token, const ProgressFn&) const override
            {
                return ItemResult{ false, true, {}, L"the inspector writes nothing", 0 };
            }
            bool NeedsEditor(const Recipe&) const override { return true; }
            // Opens the window and never calls `done`: there is nothing to run afterwards (read-only).
            void OpenEditor(HWND owner, const Recipe&, const std::vector<std::wstring>& files,
                            std::function<void(Recipe, std::vector<std::wstring>)>) const override
            {
                if (const auto open = g_open.load())
                    open(owner, files, m_ffmpeg);
                else
                    log::Warn(L"inspect: no window available to show {} file(s)", files.size());
            }

        private:
            std::shared_ptr<const FfmpegEngine> m_ffmpeg;
        };

        void RegisterActions(ActionRegistry& registry)
        {
            registry.Add(ActionDescriptor{ kActionId, L"Inspect metadata", StepKind::Engine, {} });
        }

        const char* BuiltInRecipes()
        {
            // E946 = Info (Segoe Fluent Icons / MDL2).
            return R"([
  { "id": "inspect", "label": "Inspect", "description": "Every metadata field, searchable", "ring": "tools", "icon": "E946",
    "steps": [ { "action": "inspect.open" } ] }
])";
        }

        std::unique_ptr<IEngine> MakeEngine(const EngineServices& services)
        {
            return std::make_unique<InspectEngine>(services.ffmpeg);
        }
    }

    FeatureModule Module()
    {
        return FeatureModule{ "inspect", &RegisterActions, &BuiltInRecipes, &MakeEngine };
    }

    void SetWindowOpener(OpenWindow open) { g_open = open; }
}
