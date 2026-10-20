#include "shelf/ShelfModule.h"

namespace sd
{
    namespace
    {
        void RegisterActions(ActionRegistry& registry)
        {
            // "file." so the core treats it like Copy and Copy path: instant, on the UI thread, any
            // file or folder, nothing written next to the originals.
            registry.Add(ActionDescriptor{ "file.float", L"Float", StepKind::FileAction, {} });
        }

        const char* Recipes()
        {
            // Icon: Pin (Segoe Fluent Icons E718).
            return R"([
  { "id": "float", "label": "Float", "description": "Keep on the Float shelf", "ring": "files", "icon": "E718",
    "steps": [ { "action": "file.float" } ] }
])";
        }
    }

    FeatureModule ShelfModule()
    {
        return FeatureModule{ "shelf", &RegisterActions, &Recipes, nullptr };
    }
}
