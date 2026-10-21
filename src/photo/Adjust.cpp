#include "photo/Adjust.h"

#include <algorithm>
#include <cmath>

namespace sd::photo
{
    bool Adjustments::IsIdentity() const
    {
        Adjustments none;
        none.grainSize = grainSize;   // the grain's size alone changes nothing
        return *this == none;
    }

    const std::vector<Slider>& Sliders()
    {
        static const std::vector<Slider> sliders = {
            { "exposure", L"Exposure", L"Light", -3, 3, 2, &Adjustments::exposure },
            { "contrast", L"Contrast", L"Light", -100, 100, 0, &Adjustments::contrast },
            { "highlights", L"Highlights", L"Light", -100, 100, 0, &Adjustments::highlights },
            { "shadows", L"Shadows", L"Light", -100, 100, 0, &Adjustments::shadows },
            { "whites", L"Whites", L"Light", -100, 100, 0, &Adjustments::whites },
            { "blacks", L"Blacks", L"Light", -100, 100, 0, &Adjustments::blacks },
            { "temperature", L"Temperature", L"Colour", -100, 100, 0, &Adjustments::temperature },
            { "tint", L"Tint", L"Colour", -100, 100, 0, &Adjustments::tint },
            { "vibrance", L"Vibrance", L"Colour", -100, 100, 0, &Adjustments::vibrance },
            { "saturation", L"Saturation", L"Colour", -100, 100, 0, &Adjustments::saturation },
            { "sharpen", L"Sharpen", L"Detail", 0, 100, 0, &Adjustments::sharpen },
            { "clarity", L"Clarity", L"Detail", -100, 100, 0, &Adjustments::clarity },
            { "dehaze", L"Dehaze", L"Detail", -100, 100, 0, &Adjustments::dehaze },
            { "denoise", L"Noise reduction", L"Detail", 0, 100, 0, &Adjustments::denoise },
            { "vignette", L"Vignette", L"Effects", -100, 100, 0, &Adjustments::vignette },
            { "grain", L"Grain", L"Effects", 0, 100, 0, &Adjustments::grain },
            { "grainSize", L"Grain size", L"Effects", 1, 4, 1, &Adjustments::grainSize },
        };
        return sliders;
    }

    const Slider* FindSlider(std::string_view key)
    {
        for (const auto& s : Sliders())
            if (key == s.key)
                return &s;
        return nullptr;
    }

    Adjustments ParseAdjustments(const nlohmann::json& params)
    {
        Adjustments a;
        if (!params.is_object())
            return a;
        for (const auto& s : Sliders())
            if (const auto it = params.find(s.key); it != params.end() && it->is_number())
            {
                const double v = it->get<double>();
                a.*s.field = std::isfinite(v) ? std::clamp(v, s.min, s.max) : a.*s.field;
            }
        return a;
    }

    nlohmann::json ToJson(const Adjustments& a)
    {
        auto j = nlohmann::json::object();
        for (const auto& s : Sliders())
            j[s.key] = a.*s.field;
        return j;
    }

    const std::vector<Preset>& Presets()
    {
        static const std::vector<Preset> presets = [] {
            std::vector<Preset> p;
            Adjustments vivid;
            vivid.contrast = 20;
            vivid.vibrance = 35;
            vivid.saturation = 8;
            vivid.clarity = 15;
            p.push_back({ L"Vivid", vivid });
            Adjustments soft;
            soft.contrast = -15;
            soft.highlights = -25;
            soft.shadows = 20;
            soft.clarity = -20;
            soft.vibrance = -10;
            p.push_back({ L"Soft", soft });
            Adjustments warm;
            warm.temperature = 25;
            warm.tint = 4;
            warm.vibrance = 10;
            p.push_back({ L"Warm", warm });
            Adjustments cool;
            cool.temperature = -25;
            cool.vibrance = 5;
            p.push_back({ L"Cool", cool });
            Adjustments mono;
            mono.saturation = -100;
            mono.contrast = 25;
            mono.clarity = 10;
            p.push_back({ L"Mono", mono });
            Adjustments film;
            film.contrast = -10;
            film.blacks = 30;
            film.saturation = -15;
            film.temperature = 8;
            film.vignette = 25;
            film.grain = 35;
            p.push_back({ L"Film", film });
            return p;
        }();
        return presets;
    }

    Radii RadiiFor(unsigned width, unsigned height, const Adjustments& a)
    {
        Radii r;
        const double longEdge = std::max(1u, std::max(width, height));
        r.sharpenSigma = std::max(0.7, longEdge / 4000.0);
        r.claritySigma = std::max(2.0, longEdge * 0.01);
        r.denoiseSpatial = (0.6 + 2.4 * a.denoise / 100.0) * std::max(1.0, longEdge / 4000.0);
        return r;
    }

    shade::DevelopK DevelopConstants(const Adjustments& a, const float atmosphere[3])
    {
        shade::DevelopK k{};
        k.exposure = static_cast<float>(a.exposure);
        k.contrast = static_cast<float>(a.contrast / 100);
        k.highlights = static_cast<float>(a.highlights / 100);
        k.shadows = static_cast<float>(a.shadows / 100);
        k.whites = static_cast<float>(a.whites / 100);
        k.blacks = static_cast<float>(a.blacks / 100);
        k.temperature = static_cast<float>(a.temperature / 100);
        k.tint = static_cast<float>(a.tint / 100);
        k.vibrance = static_cast<float>(a.vibrance / 100);
        k.saturation = static_cast<float>(a.saturation / 100);
        k.dehaze = static_cast<float>(a.dehaze / 100);
        k.hazeR = atmosphere ? atmosphere[0] : 1.f;
        k.hazeG = atmosphere ? atmosphere[1] : 1.f;
        k.hazeB = atmosphere ? atmosphere[2] : 1.f;
        return k;
    }

    shade::FinishK FinishConstants(const Adjustments& a, unsigned outWidth, unsigned outHeight, double scale)
    {
        shade::FinishK k{};
        k.sharpen = static_cast<float>(a.sharpen / 100);
        k.clarity = static_cast<float>(a.clarity / 100);
        k.vignette = static_cast<float>(a.vignette / 100);
        k.grain = static_cast<float>(a.grain / 100);
        const double fullLong = std::max(outWidth, outHeight) / std::max(1e-6, scale);
        k.grainSize = static_cast<float>(a.grainSize * std::max(1.0, fullLong / 4000.0));
        k.width = static_cast<float>(std::max(1u, outWidth));
        k.height = static_cast<float>(std::max(1u, outHeight));
        k.scale = static_cast<float>(std::max(1e-6, scale));
        k.seed = 17.f;
        return k;
    }

    shade::DenoiseK DenoiseConstants(const Adjustments& a, unsigned fullWidth, unsigned fullHeight, double scale)
    {
        shade::DenoiseK k{};
        const double sigma = std::max(0.3, RadiiFor(fullWidth, fullHeight, a).denoiseSpatial * scale);   // output pixels
        // Seven taps a side span about +-2 sigma; the taps sit on whole pixels so the GPU and the CPU
        // read exactly the same values.
        const double step = std::max(1.0, std::round(sigma * 2.0 / 3.0));
        const double sigmaTaps = sigma / step;
        const double range = 0.02 + 0.10 * a.denoise / 100.0;
        k.step = static_cast<float>(step);
        k.invSpatial = static_cast<float>(1.0 / (2.0 * sigmaTaps * sigmaTaps));
        k.invRange = static_cast<float>(1.0 / (2.0 * range * range));
        return k;
    }

    bool NeedsDenoise(const Adjustments& a) { return a.denoise > 0; }

    bool NeedsDevelop(const Adjustments& a)
    {
        return a.exposure != 0 || a.contrast != 0 || a.highlights != 0 || a.shadows != 0 || a.whites != 0 || a.blacks != 0 ||
               a.temperature != 0 || a.tint != 0 || a.vibrance != 0 || a.saturation != 0 || a.dehaze != 0;
    }

    bool NeedsBlur(const Adjustments& a) { return a.sharpen != 0 || a.clarity != 0; }

    bool NeedsFinish(const Adjustments& a) { return NeedsBlur(a) || a.vignette != 0 || a.grain > 0; }
}
