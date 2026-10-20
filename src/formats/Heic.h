#pragma once
#include "ImageHooks.h"

#include <optional>
#include <string>

// HEIC through Windows' own HEIF codec (the "HEIF Image Extensions" Store package, with "HEVC Video
// Extensions" for the HEVC pictures inside). Nothing is bundled: the open HEVC encoder (x265) is GPL.
// Reading HEIC is plain WIC once the codec is there; this hook writes HEIC and explains what to
// install when Windows can't.
namespace sd::formats
{
    enum class HeicSupport
    {
        Checking,     // the trial encode is still running (a moment after start)
        Ready,        // HEIF + HEVC encoders present: HEIC can be written
        NoHeif,       // no HEIF codec at all
        NoHevc,       // HEIF codec, but no HEVC encoder behind it
    };

    // Is HEIC output possible here? The first call starts a background trial encode (a few tiny
    // pictures through Media Foundation's HEVC encoder) and answers Checking until it finishes.
    HeicSupport HeicEncoderSupport();
    // What to tell the user for a `support` other than Ready.
    std::wstring HeicMissingReason(HeicSupport support);
    // Tests: pretend the codec is (or isn't) installed; nullopt goes back to the real answer.
    void OverrideHeicSupportForTests(std::optional<HeicSupport> support);

    const IImageFormatHook& HeicHook();
}
