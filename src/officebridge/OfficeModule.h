#pragma once
#include "Engine.h"

// Office bridge (OfficeBridge.h): one FeatureModule, one action, "office.convert"
// (format pdf, png or jpg). Its slices appear only when an office app that can do the job is
// installed; the apps are found once, when the engine is made at startup.
namespace sd::office
{
    FeatureModule Module();
}
