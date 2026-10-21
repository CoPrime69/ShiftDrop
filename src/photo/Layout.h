#pragma once
#include "canvas/Geometry.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Add a Background (tool 5) and Create a Collage (tool 8): their settings and the layout maths, in
// whole output pixels. Plain data, tested on its own; Compose.cpp draws the result.
namespace sd::photo
{
    using canvas::RectD;
    using canvas::RectI;
    using canvas::SizeD;

    // Output shapes offered by both tools: "auto" keeps the content's own shape.
    struct AspectChoice
    {
        const char* id;
        const wchar_t* label;
        double ratio;   // width / height; 0 = auto
    };
    const std::vector<AspectChoice>& AspectChoices();
    double AspectRatio(const std::string& id);   // 0 for "auto" and unknown ids

    // ---- Add a Background ---------------------------------------------------------------------------
    enum class Fill { Solid, Gradient, Blur, Image };   // Blur = the photo itself, blurred, behind it

    struct BackgroundParams
    {
        Fill fill = Fill::Gradient;
        uint32_t color1 = 0x6A5ACD;   // 0xRRGGBB
        uint32_t color2 = 0xF08A5D;
        double angle = 135;           // gradient direction, degrees clockwise from "left to right"
        std::wstring image;           // Fill::Image
        double blur = 40;             // 0..100, for Blur and Image
        std::string aspect = "auto";
        double padding = 8;           // 0..40, % of the picture's longer side on every side
        double radius = 12;           // 0..100: corner radius as a share of a quarter of the shorter side
        double shadow = 50;           // 0..100
    };
    BackgroundParams ParseBackground(const nlohmann::json& params);
    nlohmann::json ToJson(const BackgroundParams& p);

    struct BackgroundLayout
    {
        int width = 0, height = 0;   // the output
        RectI picture;               // where the picture goes, at its own size
        double radius = 0;           // corner radius, pixels
        double shadowSigma = 0;      // pixels; 0 = no shadow
        double shadowOffset = 0;     // downwards, pixels
        double shadowOpacity = 0;
    };
    // `maxEdge` bounds the output (Direct2D's bitmap limit); a larger result scales the padding down.
    BackgroundLayout LayoutBackground(int pictureWidth, int pictureHeight, const BackgroundParams& p, int maxEdge = 16384);
    // The background picture's blur, as a sigma in output pixels.
    double BackgroundBlurSigma(const BackgroundLayout& layout, double blur);

    // The part of a `source` picture that covers a `target` shape (centred crop), in source pixels.
    RectD CoverSource(SizeD source, SizeD target);
    // `source` fitted inside `target` (centred, letterboxed), in target coordinates.
    RectD FitInside(SizeD source, const RectD& target);

    // ---- Create a Collage ---------------------------------------------------------------------------
    enum class CollageKind { Grid, Row, Column, Featured };

    struct CollageParams
    {
        CollageKind kind = CollageKind::Grid;
        std::vector<int> order;       // indices into the dropped files; empty = drop order
        int size = 3000;              // the output's longer side, pixels
        std::string aspect = "auto";  // grid and featured only; rows and columns keep every photo whole
        double spacing = 20;          // 0..100: the gap, as a share of 4% of the output's longer side
        double radius = 0;            // 0..100: corner radius as a share of a quarter of a cell's shorter side
        uint32_t background = 0xFFFFFF;
        bool fit = false;             // fit each photo inside its cell instead of filling it
    };
    CollageParams ParseCollage(const nlohmann::json& params);
    nlohmann::json ToJson(const CollageParams& p);
    // `order` made valid for `count` files: every index once, unknown ones dropped, missing ones appended.
    std::vector<int> NormalisedOrder(const std::vector<int>& order, size_t count);

    struct CollageCell
    {
        int image = 0;   // index into the files
        RectI rect;
    };
    struct CollageLayout
    {
        int width = 0, height = 0;
        int gap = 0;
        double radius = 0;
        std::vector<CollageCell> cells;   // in order
    };
    // `sizes` are the pictures' sizes in file order (only their shapes matter).
    CollageLayout LayoutCollage(const std::vector<SizeD>& sizes, const CollageParams& p, int maxEdge = 16384);
    // The cell under a point, or -1.
    int CellAt(const CollageLayout& layout, canvas::PointD at);
}
