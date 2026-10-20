#pragma once
#include "Engine.h"

namespace sd
{
    // Crop Images, Redact Photos, Share-safe and Remove metadata (M6): the image canvas tools and the
    // metadata scrub, as one feature module (src/canvas, src/crop, src/redact, src/sharesafe,
    // src/metadata).
    FeatureModule ImageToolsModule();
}
