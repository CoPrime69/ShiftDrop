#pragma once
#include "Engine.h"

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

// The "Inspect" slice on the Tools wheel opens a read-only window listing every metadata field of
// the dropped files. Editing fields is not built yet.
//
// The engine lives in the core library; the window is UI code in the app, so the app hands the
// engine a function that opens it (InspectorWindow.h). Without one (tests) the slice only logs.
namespace sd::inspect
{
    inline constexpr char kActionId[] = "inspect.open";

    FeatureModule Module();

    using OpenWindow = void (*)(HWND owner, const std::vector<std::wstring>& files, std::shared_ptr<const FfmpegEngine> ffmpeg);
    void SetWindowOpener(OpenWindow open);
}
