#pragma once
#include "Action.h"
#include "HistoryStore.h"
#include "Recipe.h"
#include "StagedOutput.h"
#include "formats/Pixels.h"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

// Files with more than one picture: animated GIFs and multi-page TIFFs. Converting one keeps every
// frame: a GIF to WebP or AVIF stays animated, a GIF to TIFF becomes a multi-page
// TIFF, and anything else becomes a folder "name (frames)" with one file per frame. Images to PDF
// gives every TIFF page its own PDF page (src/pdf/PdfEngine.cpp).
namespace sd::formats
{
    // Frames (GIF) or pages (TIFF) in the file; 1 for any other type, a single picture, or a file
    // that can't be read.
    uint32_t FrameCount(const std::wstring& path);

    // Called once per frame, in order, with the picture as it is seen at that moment (a GIF's frames
    // are drawn onto its canvas in turn, honouring each frame's place and disposal). `delayMs` is
    // how long the frame shows (0 for TIFF pages). Return false to stop early.
    using FrameFn = std::function<bool(const Image& frame, uint32_t index, uint32_t count, int delayMs)>;
    HRESULT ForEachFrame(const std::wstring& path, std::stop_token stop, const FrameFn& each, std::wstring& error);

    // What converting a multi-frame file to `format` makes.
    enum class FramesOutput
    {
        Animation,   // one animated file (GIF to WebP or AVIF)
        Pages,       // one multi-page TIFF
        Folder,      // a folder "name (frames)" with a file per frame
    };
    FramesOutput FramesOutputFor(const std::wstring& lowerSourceExtension, std::string_view formatId);

    // A GIF with more frames than this asks first, before it becomes a folder of pictures: all of
    // them, every Nth (FrameStep), or none.
    constexpr uint32_t kAskAboveFrames = 100;
    // The N of "every Nth frame" that leaves at most kAskAboveFrames of `count` (2 at least).
    uint32_t FrameStep(uint32_t count);

    // Runs a single-encode recipe (Convert wheel) on a multi-frame `source`, written through
    // StagedOutput and recorded in history like every other output.
    ItemResult ConvertFrames(const Recipe& recipe, const ImageFormatInfo& format, HistoryStore* history, int64_t runId,
                             const std::wstring& source, std::stop_token stop, const std::function<void(double)>& progress);
}
