#pragma once
#include "Engine.h"

#include <string>
#include <string_view>

// Subtitles (M11): SRT and WebVTT in; SRT, WebVTT and plain text out. One FeatureModule, one
// action, "subtitles.convert".
namespace sd::subtitles
{
    FeatureModule Module();

    // Is this a subtitle file we read, and is `target` ("srt", "vtt", "txt") another format?
    bool TargetFits(const std::wstring& path, std::string_view target);
}
