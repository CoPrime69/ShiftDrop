#pragma once
#include "Engine.h"

namespace sd
{
    // The Float shelf as a feature module: the "file.float" action and its slice on the File-actions
    // wheel. It is an instant file action (no engine): the shelf window (ShelfWindow.h) registers the
    // handler with SetInstantFileAction when the app creates it.
    FeatureModule ShelfModule();
}
