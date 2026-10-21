#pragma once
#include "photo/Adjust.h"
#include "photo/Filters.h"

#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>

#include <string>

// Edit Photos on the GPU: three pixel-shader effects of our own (noise reduction, develop with
// dehaze, finish with clarity, sharpen, vignette and grain), compiled at run time from the same
// text as the CPU path (Shade.h), plus Direct2D's own Gaussian blur and affine transform. The live
// preview and the export both build this graph, so what the editor shows is what gets saved.
namespace sd::photo::fx
{
    using Microsoft::WRL::ComPtr;

    // The effects of one device context (effects belong to the context that made them), kept between
    // renders so moving a slider only changes their constants.
    struct AdjustGraph
    {
        ID2D1DeviceContext* dc = nullptr;
        ComPtr<ID2D1Effect> resample, denoise, develop, smallBlur, largeBlur, finish, hazeScale;
        ComPtr<ID2D1Bitmap1> hazeMap;          // the HazeModel's coefficients, as a float bitmap
        const HazeModel* hazeFor = nullptr;    // which model hazeMap holds
        bool failed = false;                   // the effects can't be made here: use the CPU
        std::wstring why;
    };

    // Sets the context up for the photo effects: float intermediates (16-bit where the device has
    // them), so tone changes don't band. Call before drawing a graph.
    void PrepareContext(ID2D1DeviceContext* dc);

    // The adjusted picture: `source` is the picture at `width` x `height` pixels (premultiplied),
    // which is `scale` times the full picture (`fullWidth` x `fullHeight`). Returns null, with
    // graph.failed and graph.why set, when the device can't run the effects.
    ComPtr<ID2D1Image> Adjust(ID2D1DeviceContext* dc, AdjustGraph& graph, ID2D1Image* source, unsigned width, unsigned height, unsigned fullWidth,
                              unsigned fullHeight, double scale, const Adjustments& a, const HazeModel* haze);

    // The HLSL of one effect, for tests and logs ("denoise", "develop", "finish").
    std::string ShaderSource(const std::string& name);
    // Compiles all three shaders (once per process). False with `error` when the compiler is missing
    // or rejects them.
    bool CompileShaders(std::wstring& error);
}
