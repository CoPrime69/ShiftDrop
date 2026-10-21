#pragma once
#include "photo/Shade.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// Edit Photos' settings (tool 3): what the sliders hold, how they turn into shader constants, and
// the presets. Plain data, so the tool, the engine and the tests share one definition.
namespace sd::photo
{
    // Slider values as the user sees them: exposure in stops, the rest -100..100 (or 0..100).
    struct Adjustments
    {
        double exposure = 0;      // -3..3 EV
        double contrast = 0;
        double highlights = 0;
        double shadows = 0;
        double whites = 0;
        double blacks = 0;
        double temperature = 0;
        double tint = 0;
        double vibrance = 0;
        double saturation = 0;
        double sharpen = 0;       // 0..100
        double clarity = 0;
        double dehaze = 0;
        double denoise = 0;       // 0..100
        double vignette = 0;
        double grain = 0;         // 0..100
        double grainSize = 1.5;   // 1..4 pixels (at 12 MP; scales with the picture)

        bool IsIdentity() const;
        friend bool operator==(const Adjustments&, const Adjustments&) = default;
    };

    // One slider: its key in the params, label, range and group, in panel order.
    struct Slider
    {
        const char* key;
        const wchar_t* label;
        const wchar_t* group;   // "Light", "Colour", "Detail", "Effects"
        double min, max;
        int decimals;
        double Adjustments::*field;
    };
    const std::vector<Slider>& Sliders();
    const Slider* FindSlider(std::string_view key);

    // {"exposure": 0.5, ...}; unknown keys are ignored and values are clamped to their slider's range.
    Adjustments ParseAdjustments(const nlohmann::json& params);
    nlohmann::json ToJson(const Adjustments& a);   // every key, so a recipe file shows them all

    // One-click starting points ("Auto" is computed per picture, see AutoAdjust in Filters.h).
    struct Preset
    {
        const wchar_t* label;
        Adjustments values;
    };
    const std::vector<Preset>& Presets();

    // Pixel radii of the neighbourhood filters for a picture of this size (in full-size pixels).
    // Clarity looks at about one percent of the picture, so it reads the same in the preview and
    // in the export; sharpen at about a pixel.
    struct Radii
    {
        double sharpenSigma = 1;
        double claritySigma = 20;
        double denoiseSpatial = 2;   // the bilateral filter's sigma, pixels
    };
    Radii RadiiFor(unsigned width, unsigned height, const Adjustments& a);

    // The shaders' constants. `scale` is output pixels per full-size pixel (a preview renders smaller).
    shade::DevelopK DevelopConstants(const Adjustments& a, const float atmosphere[3]);
    shade::FinishK FinishConstants(const Adjustments& a, unsigned outWidth, unsigned outHeight, double scale);
    shade::DenoiseK DenoiseConstants(const Adjustments& a, unsigned fullWidth, unsigned fullHeight, double scale);
    // Whether a stage has anything to do.
    bool NeedsDenoise(const Adjustments& a);
    bool NeedsDevelop(const Adjustments& a);
    bool NeedsFinish(const Adjustments& a);
    bool NeedsBlur(const Adjustments& a);   // clarity or sharpen
}
