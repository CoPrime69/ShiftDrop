#pragma once
#include "canvas/Geometry.h"
#include "canvas/Raster.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

// Crop Images (tool 6): the maths, separate from the editor so it is unit-tested. Coordinates are
// pixels of the picture as shown (EXIF orientation applied), after the crop's own quarter turns.
namespace sd::crop
{
    using canvas::RectD;
    using canvas::RectI;
    using canvas::SizeD;

    struct AspectPreset
    {
        const char* id;          // "free", "original", "1:1", "4:5", "3:2", "16:9", "9:16", "custom"
        const wchar_t* label;
        double w, h;             // the ratio; 0 for free, original and custom (worked out per image)
    };
    const std::vector<AspectPreset>& AspectPresets();
    const AspectPreset* FindAspect(std::string_view id);

    // The width/height ratio a preset asks for on a picture of this size; 0 = free.
    double AspectRatio(std::string_view preset, double customW, double customH, double imageW, double imageH);

    // The largest rectangle of `aspect` (0: any, so all of `bounds`) centred in `bounds`.
    RectD LargestCentred(const RectD& bounds, double aspect);

    // The picture's size after `quarterTurns` clockwise.
    SizeD RotatedSize(double w, double h, int quarterTurns);

    // Where a crop box may go when the picture is straightened by `degrees`: the largest centred
    // rectangle with the picture's own shape that stays inside the rotated picture, so no corner of
    // the output is ever empty. All of it when `degrees` is 0.
    RectD StraightenBounds(double w, double h, double degrees);

    // The crop.image step's params, as the editor writes them.
    struct CropParams
    {
        int quarterTurns = 0;            // clockwise, applied first
        double straighten = 0;           // degrees, clockwise, -45..45
        RectD box;                       // in the first picture's (rotated) pixels
        double refW = 0, refH = 0;       // the first picture's rotated size, for the rest of a batch
        std::string aspect = "free";
        double ratioW = 1, ratioH = 1;   // the custom ratio
        bool exactSize = false;          // typed pixel size: every picture gets that size if it fits
        bool samePlace = false;          // the rest of a batch: same relative place (default: centred)
    };
    CropParams ParseParams(const nlohmann::json& params);
    nlohmann::json ToJson(const CropParams& params);

    struct CropPlan
    {
        int quarterTurns = 0;
        double degrees = 0;
        int rotatedW = 0, rotatedH = 0;   // the picture after the quarter turns
        RectI box;                        // in those pixels; the output is box.w x box.h
    };

    // The crop for a picture of `width` x `height` (oriented). The first picture of a batch gets the
    // box exactly as drawn; others of another size get the same shape, scaled with the picture (or
    // the same pixel size when it was typed), centred unless `samePlace`, and always inside.
    CropPlan PlanCrop(const CropParams& params, int width, int height);

    // Renders the crop of `source` (oriented pixels): exact pixel copies when there is no
    // straighten angle, bilinear resampling when there is. Keeps resolution and colour profile.
    bool RenderCrop(const canvas::Raster& source, const CropPlan& plan, canvas::Raster& out);
}
