#include "canvas/Editor.h"
#include "Log.h"

#include <atomic>

namespace sd::canvas
{
    namespace
    {
        std::atomic<EditorWindowFactory>& Factory()
        {
            static std::atomic<EditorWindowFactory> factory{ nullptr };   // a function-local static: safe from other initialisers
            return factory;
        }
    }

    void RegisterEditorWindow(EditorWindowFactory factory) { Factory() = factory; }

    bool HasEditorWindow() { return Factory().load() != nullptr; }

    bool OpenEditorWindow(EditorRequest request)
    {
        const auto factory = Factory().load();
        return factory && factory(std::move(request));
    }

    void OpenToolEditor(const Recipe& recipe, const std::vector<std::wstring>& files, std::unique_ptr<ICanvasTool> tool,
                        std::function<void(Recipe, std::vector<std::wstring>)> done)
    {
        EditorRequest request;
        request.files = files;
        request.tool = std::move(tool);
        request.apply = [recipe, files, done = std::move(done)](nlohmann::json params) {
            Recipe adjusted = recipe;
            if (!adjusted.steps.empty() && params.is_object())
                for (auto& [key, value] : params.items())
                    adjusted.steps[0].params[key] = value;
            done(std::move(adjusted), files);
        };
        if (!OpenEditorWindow(std::move(request)))
            log::Error(L"editor: no editor window in this build");
    }
}
