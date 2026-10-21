#pragma once
#include "Media.h"
#include "Recipe.h"

#include <cstdint>
#include <string>
#include <vector>

// The arithmetic behind the predicted output shown while a slice is
// highlighted. Pure; Predictor.h runs the samples and probes that feed it.
namespace sd::preview
{
    struct Size2
    {
        uint32_t w = 0, h = 0;
        bool operator==(const Size2&) const = default;
        double Pixels() const { return static_cast<double>(w) * h; }
    };

    // The output's pixel size for a source of `source` pixels (already turned upright by its EXIF
    // orientation, as the pipeline does): resize never upscales, scale only shrinks, a quarter turn
    // swaps the sides. Same rounding as ImagePipeline.cpp.
    Size2 OutputDimensions(const Recipe& recipe, Size2 source);
    // EXIF orientations 5 to 8 store the picture on its side.
    Size2 Upright(Size2 stored, int exifOrientation);

    // What the recipe's rotate and flip steps do to the picture, as an integer 2x2 matrix acting on
    // (x, y) with y pointing down: the thumbnail is drawn through it so it looks like the result.
    struct Orientation
    {
        int a = 1, b = 0, c = 0, d = 1;   // x' = a x + b y, y' = c x + d y
        bool Identity() const { return a == 1 && b == 0 && c == 0 && d == 1; }
        bool SwapsSides() const { return a == 0; }
        bool operator==(const Orientation&) const = default;
    };
    Orientation RecipeOrientation(const Recipe& recipe);

    // The recipe with a downscale to `maxEdge` slotted in just before its encode step: the same
    // pipeline on fewer pixels, for a quick size sample.
    Recipe SampleRecipe(const Recipe& recipe, uint32_t maxEdge);

    // Encoded size grows roughly as pixels^k (k below 1 for photos: more pixels, more redundancy).
    // Two samples give k; it is clamped to [0.5, 1.05] so one odd sample can't run away.
    double FitExponent(double bytes1, double pixels1, double bytes2, double pixels2);
    uint64_t Extrapolate(double bytes, double pixels, double targetPixels, double exponent);

    // "1.2 MB", "860 KB", "12 bytes" (1 KB = 1024 bytes, like Explorer).
    std::wstring SizeText(uint64_t bytes);
    // "4032×3024 → 1920×1440"; empty when nothing changes.
    std::wstring DimensionsChange(Size2 from, Size2 to);

    // What a file's metadata holds that matters when it is lost.
    struct MetadataFacts
    {
        bool gps = false, camera = false, dateTaken = false, author = false, xmp = false;
        bool Any() const { return gps || camera || dateTaken || author || xmp; }
    };
    // "removes GPS, camera, date taken"; empty when there is nothing to lose. Image outputs never
    // carry the source's metadata in this version (ImagePipeline.cpp), so every image recipe drops it.
    std::wstring MetadataLoss(const MetadataFacts& facts);

    // The first free output name, as StagedOutput will pick it: "photo (converted).webp", then
    // "photo (converted 2).webp"... Looks at the disk. In the Subfolder naming style the name starts
    // with "ShiftDrop\".
    std::wstring PredictOutputName(const std::wstring& source, const std::wstring& suffix, const std::wstring& extension);

    // Media: the bitrates in a planned ffmpeg command line (Media.h's OutputArgs / CompressArgs),
    // applied to the input's length.
    struct StreamFacts
    {
        int64_t videoBps = 0, audioBps = 0;   // the input's own, when ffmpeg printed them
        int sampleRate = 0, channels = 0;
    };
    struct MediaEstimate
    {
        bool known = false;         // false: no duration, so no size
        uint64_t bytes = 0;
        bool sameStreams = false;   // every stream copied as it is: about the input's size
        bool video = false, audio = false;
        Size2 size;                 // output picture size (0 for audio)
        double fps = 0;
    };
    MediaEstimate EstimateMedia(const std::vector<std::wstring>& outputArgs, const MediaInfo& info, const StreamFacts& facts,
                                uint64_t sourceBytes);
    // "-b:a 192k" style values: "192k" -> 192000, "6M" -> 6000000, "8000000" -> 8000000; 0 if unreadable.
    int64_t ParseBitrate(const std::wstring& text);
}
