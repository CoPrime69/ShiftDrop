#pragma once
#include "Engine.h"

namespace sd
{
    // Video tools (M9) and audio tools (M10) as one feature module: video.* and audio.* actions,
    // their Tools-wheel slices, and one engine on the media lane that runs them and opens the
    // timeline editor for the ones that need a choice first.
    FeatureModule MediaToolsModule();
}
