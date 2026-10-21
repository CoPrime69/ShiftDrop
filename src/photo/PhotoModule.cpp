#include "photo/PhotoModule.h"
#include "photo/PhotoEngine.h"

namespace sd::photo
{
    namespace
    {
        std::unique_ptr<IEngine> MakeEngine(const EngineServices& services) { return std::make_unique<PhotoEngine>(services); }
    }

    FeatureModule Module() { return FeatureModule{ "photo", &RegisterActions, &BuiltInRecipes, &MakeEngine }; }
}
