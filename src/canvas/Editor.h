#pragma once
#include "Recipe.h"
#include "canvas/CanvasTool.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// How an engine (in shiftdrop_core) opens the image editor window (in the app): the window
// registers itself here when the app starts, so engines and their tests never link against the UI.
namespace sd::canvas
{
    struct EditorRequest
    {
        std::vector<std::wstring> files;            // the dropped files, in drop order
        std::unique_ptr<ICanvasTool> tool;
        // Called once on the UI thread with the tool's Params() when the user applies (never on Cancel).
        std::function<void(nlohmann::json params)> apply;
    };

    using EditorWindowFactory = bool (*)(EditorRequest request);

    // EditorWindow.cpp registers its factory from a static initialiser in the app.
    void RegisterEditorWindow(EditorWindowFactory factory);
    bool HasEditorWindow();
    // Opens the window (UI thread). False when the app has no editor window (the unit tests).
    bool OpenEditorWindow(EditorRequest request);

    // For IEngine::OpenEditor: opens `tool` on `files`; on Apply calls `done` with `recipe`'s single
    // step's params updated from the tool's params.
    void OpenToolEditor(const Recipe& recipe, const std::vector<std::wstring>& files, std::unique_ptr<ICanvasTool> tool,
                        std::function<void(Recipe, std::vector<std::wstring>)> done);
}
