#pragma once
#include "Engine.h"
#include "pdf/PdfContent.h"

#include <stop_token>
#include <string>

// Compress PDF: pictures are the bulk of most large PDFs, so each image is re-encoded as JPEG at a
// quality preset and, when it is stored at more than the target resolution for the size it is drawn
// at, scaled down to it. The structure is then packed with object streams and every lossless stream
// re-deflated. Images that would not get smaller (or that JPEG can't carry faithfully: masks, odd
// colour spaces, 16-bit) are left exactly as they were. The engine keeps the result only if the
// whole file got smaller.
namespace sd::pdf
{
    struct CompressOptions
    {
        int dpi = 150;      // target resolution for pictures
        int quality = 70;   // JPEG quality 1-100
    };

    struct CompressStats
    {
        int images = 0;         // image objects seen
        int recompressed = 0;   // replaced by a smaller JPEG
    };

    bool CompressPdf(const std::wstring& source, const std::wstring& out, const CompressOptions& options, std::stop_token stop,
                     const ProgressFn& progress, CompressStats& stats, std::wstring& error);

    // The pixel size an image of w x h should have so it is no sharper than `dpi` where it is drawn
    // largest. Without a placement (an image only used in a pattern or an annotation) the size stays.
    // Shrinks only when that saves real pixels (below 80% of the width), never enlarges.
    void TargetSize(uint32_t w, uint32_t h, const Placement* placement, int dpi, uint32_t& outW, uint32_t& outH);
}
