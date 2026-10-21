#pragma once
#include <algorithm>
#include <cmath>

// The per-pixel maths of Edit Photos, written once and compiled twice: as C++ for the CPU path (the
// reference the tests check, and the fallback when the GPU can't run our effects) and as HLSL for
// the Direct2D effects that render the live preview and the export (Effects.cpp stringises the same
// text into its pixel shaders). So the GPU and the CPU can't drift apart.
//
// The shared text is the subset HLSL and C++ have in common: float and float3 values, the operators,
// dot / lerp / saturate / pow / exp2 / smoothstep / frac / floor / sin, structs of plain floats (the
// shader constants, packed four to a register exactly as C++ lays them out), if / else and the ternary
// operator. Inside the macros only /* */ comments work, because a // comment would swallow the rest
// of the spliced line.
//
// Colours are unpremultiplied sRGB values in 0..1.
namespace sd::photo::shade
{
    struct float3
    {
        float x = 0, y = 0, z = 0;
        float3() = default;
        constexpr float3(float a, float b, float c) : x(a), y(b), z(c) {}
    };
    inline float3 operator+(float3 a, float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline float3 operator-(float3 a, float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline float3 operator*(float3 a, float3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
    inline float3 operator/(float3 a, float3 b) { return { a.x / b.x, a.y / b.y, a.z / b.z }; }
    inline float3 operator+(float3 a, float s) { return { a.x + s, a.y + s, a.z + s }; }
    inline float3 operator-(float3 a, float s) { return { a.x - s, a.y - s, a.z - s }; }
    inline float3 operator*(float3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    inline float3 operator/(float3 a, float s) { return { a.x / s, a.y / s, a.z / s }; }
    inline float3 operator*(float s, float3 a) { return a * s; }
    inline float3 operator+(float s, float3 a) { return a + s; }
    inline float3 operator-(float s, float3 a) { return { s - a.x, s - a.y, s - a.z }; }

    inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline float saturate(float v) { return std::clamp(v, 0.f, 1.f); }
    inline float3 saturate(float3 a) { return { saturate(a.x), saturate(a.y), saturate(a.z) }; }
    inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
    inline float3 lerp(float3 a, float3 b, float t) { return a + (b - a) * t; }
    inline float max(float a, float b) { return a > b ? a : b; }
    inline float min(float a, float b) { return a < b ? a : b; }
    inline float abs(float v) { return std::fabs(v); }
    inline float pow(float a, float b) { return std::pow(a, b); }
    inline float3 pow(float3 a, float b) { return { std::pow(a.x, b), std::pow(a.y, b), std::pow(a.z, b) }; }
    inline float exp(float v) { return std::exp(v); }
    inline float exp2(float v) { return std::exp2(v); }
    inline float sqrt(float v) { return std::sqrt(v); }
    inline float sin(float v) { return std::sin(v); }
    inline float floor(float v) { return std::floor(v); }
    inline float frac(float v) { return v - std::floor(v); }
    inline float smoothstep(float a, float b, float v)
    {
        const float t = saturate((v - a) / (b - a));
        return t * t * (3.f - 2.f * t);
    }
}

// ---- The shared text ---------------------------------------------------------------------------

// Shader constants. Plain floats only, 16 to a struct, so HLSL's register packing and C++'s layout
// agree byte for byte.
#define SD_PHOTO_SHADE_CONSTANTS                                                                         \
    struct DevelopK                                                                                      \
    {                                                                                                    \
        float exposure;    /* stops */                                                                   \
        float contrast;    /* -1..1, and the same for the rest */                                        \
        float highlights;                                                                                \
        float shadows;                                                                                   \
        float whites;                                                                                    \
        float blacks;                                                                                    \
        float temperature; /* + warmer */                                                                \
        float tint;        /* + magenta */                                                               \
        float vibrance;                                                                                  \
        float saturation;                                                                                \
        float dehaze;      /* + clears haze, - adds it */                                                \
        float hazeR;       /* the atmospheric light (dark channel prior) */                              \
        float hazeG;                                                                                     \
        float hazeB;                                                                                     \
        float pad0;                                                                                      \
        float pad1;                                                                                      \
    };                                                                                                   \
    struct FinishK                                                                                       \
    {                                                                                                    \
        float sharpen;     /* 0..1 */                                                                    \
        float clarity;     /* -1..1 */                                                                   \
        float vignette;    /* -1..1, + darker corners */                                                 \
        float grain;       /* 0..1 */                                                                    \
        float grainSize;   /* pixels of the full-size picture */                                         \
        float width;       /* the output being rendered, in its own pixels */                            \
        float height;                                                                                    \
        float scale;       /* output pixels per pixel of the full-size picture */                        \
        float seed;                                                                                      \
        float pad0;                                                                                      \
        float pad1;                                                                                      \
        float pad2;                                                                                      \
        float pad3;                                                                                      \
        float pad4;                                                                                      \
        float pad5;                                                                                      \
        float pad6;                                                                                      \
    };                                                                                                   \
    struct DenoiseK                                                                                      \
    {                                                                                                    \
        float step;        /* whole output pixels between the 7 x 7 taps */                              \
        float invSpatial;  /* 1 / (2 sigma_s^2), sigma in taps */                                        \
        float invRange;    /* 1 / (2 sigma_r^2), sigma in luma */                                        \
        float pad0;                                                                                      \
    };

#define SD_PHOTO_SHADE_FUNCTIONS                                                                         \
    inline float Luma(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }                    \
                                                                                                         \
    /* A soft S-curve (contrast > 0) or its flattening (< 0); 0 and 1 stay put. */                       \
    inline float Contrast(float l, float k) { return l + k * (l * l * (3.0f - 2.0f * l) - l); }          \
                                                                                                         \
    /* Tone on luminance: shadows and highlights each move their part of the range (peaks at 1/3 and    \
       2/3), whites and blacks the two ends. */                                                          \
    inline float Tone(float l, DevelopK k)                                                               \
    {                                                                                                    \
        float u = 1.0f - l;                                                                              \
        l = l + k.shadows * 0.25f * (l * u * u * 6.75f);                                                 \
        l = l + k.highlights * 0.25f * (l * l * u * 6.75f);                                              \
        l = l + k.whites * 0.2f * (l * l * l * l);                                                       \
        l = l + k.blacks * 0.2f * (u * u * u * u);                                                       \
        return saturate(Contrast(saturate(l), k.contrast));                                              \
    }                                                                                                    \
                                                                                                         \
    /* Dark channel prior: `haze` is the refined dark channel of the picture over the atmospheric       \
       light (0 clear .. 1 all haze); clearing divides the haze out, J = (I - A) / t + A. */             \
    inline float3 Dehaze(float3 c, float haze, DevelopK k)                                               \
    {                                                                                                    \
        float3 a = float3(k.hazeR, k.hazeG, k.hazeB);                                                    \
        if (k.dehaze > 0.0f)                                                                             \
        {                                                                                                \
            float t = max(1.0f - k.dehaze * 0.95f * saturate(haze), 0.1f);                              \
            return saturate((c - a) / t + a);                                                            \
        }                                                                                                \
        return lerp(c, a, -k.dehaze * 0.6f * (1.0f - 0.5f * saturate(haze)));                            \
    }                                                                                                    \
                                                                                                         \
    inline float3 Develop(float3 c, float haze, DevelopK k)                                              \
    {                                                                                                    \
        if (k.dehaze != 0.0f)                                                                            \
            c = Dehaze(c, haze, k);                                                                      \
        /* White balance and exposure in linear light. */                                                \
        float3 lin = pow(saturate(c), 2.2f);                                                             \
        float3 wb = float3(1.0f + 0.3f * k.temperature + 0.12f * k.tint, 1.0f - 0.25f * k.tint,          \
                           1.0f - 0.3f * k.temperature + 0.12f * k.tint);                                \
        lin = lin * (wb / max(Luma(wb), 0.01f)) * exp2(k.exposure);                                      \
        c = pow(saturate(lin), 1.0f / 2.2f);                                                             \
        /* Tone curves on luminance, keeping the colour's ratios. */                                     \
        float l = Luma(c);                                                                               \
        float l2 = Tone(l, k);                                                                           \
        c = l > 0.0005f ? c * (l2 / l) : c + (l2 - l);                                                   \
        c = saturate(c);                                                                                 \
        /* Saturation everywhere; vibrance mostly where colour is weak. */                               \
        float m = Luma(c);                                                                               \
        float s = max(c.x, max(c.y, c.z)) - min(c.x, min(c.y, c.z));                                     \
        float gain = max(0.0f, 1.0f + k.saturation + k.vibrance * (1.0f - s));                           \
        return saturate(m + (c - m) * gain);                                                             \
    }                                                                                                    \
                                                                                                         \
    inline float Hash(float x, float y, float seed) { return frac(sin(x * 127.1f + y * 311.7f + seed) * 43758.5453f); } \
                                                                                                         \
    /* Smooth value noise in -0.5..0.5, one cell per `size` pixels of the full picture. */              \
    inline float Grain(float x, float y, FinishK k)                                                      \
    {                                                                                                    \
        float gx = x / (k.scale * k.grainSize);                                                          \
        float gy = y / (k.scale * k.grainSize);                                                          \
        float ix = floor(gx);                                                                            \
        float iy = floor(gy);                                                                            \
        float fx = gx - ix;                                                                              \
        float fy = gy - iy;                                                                              \
        fx = fx * fx * (3.0f - 2.0f * fx);                                                               \
        fy = fy * fy * (3.0f - 2.0f * fy);                                                               \
        float top = lerp(Hash(ix, iy, k.seed), Hash(ix + 1.0f, iy, k.seed), fx);                         \
        float bottom = lerp(Hash(ix, iy + 1.0f, k.seed), Hash(ix + 1.0f, iy + 1.0f, k.seed), fx);        \
        return lerp(top, bottom, fy) - 0.5f;                                                             \
    }                                                                                                    \
                                                                                                         \
    /* Detail and finishing: `fine` and `broad` are the picture blurred by about one pixel and by     \
       about one percent of its size. Clarity adds back the large-scale local contrast in the           \
       midtones, sharpen the fine detail; then the vignette and grain. (x, y) is the pixel centre. */   \
    inline float3 Finish(float3 c, float3 fine, float3 broad, float x, float y, FinishK k)               \
    {                                                                                                    \
        float l = Luma(c);                                                                               \
        float mid = 1.0f - (2.0f * l - 1.0f) * (2.0f * l - 1.0f);                                        \
        c = c + (l - Luma(broad)) * (k.clarity * 1.5f * mid);                                            \
        c = c + (l - Luma(fine)) * (k.sharpen * 2.0f);                                                  \
        c = saturate(c);                                                                                 \
        if (k.vignette != 0.0f)                                                                          \
        {                                                                                                \
            float nx = (x / k.width - 0.5f) * 2.0f;                                                      \
            float ny = (y / k.height - 0.5f) * 2.0f;                                                     \
            float r = sqrt((nx * nx + ny * ny) * 0.5f);                                                  \
            float v = smoothstep(0.35f, 1.0f, r);                                                        \
            c = k.vignette > 0.0f ? c * (1.0f - k.vignette * 0.8f * v) : lerp(c, float3(1.0f, 1.0f, 1.0f), -k.vignette * 0.6f * v); \
        }                                                                                                \
        if (k.grain > 0.0f)                                                                              \
        {                                                                                                \
            float g = l * (1.0f - l) * 4.0f;                                                             \
            c = c + Grain(x, y, k) * (k.grain * 0.3f * (0.4f + 0.6f * g));                               \
        }                                                                                                \
        return saturate(c);                                                                              \
    }                                                                                                    \
                                                                                                         \
    /* The edge-preserving filter's weight for a tap `d2` taps^2 away whose luma differs by `dl`. */    \
    inline float BilateralWeight(float d2, float dl, DenoiseK k) { return exp(-d2 * k.invSpatial - dl * dl * k.invRange); }

namespace sd::photo::shade
{
    SD_PHOTO_SHADE_CONSTANTS
    SD_PHOTO_SHADE_FUNCTIONS
    static_assert(sizeof(DevelopK) == 64 && sizeof(FinishK) == 64 && sizeof(DenoiseK) == 16, "shader constants are whole registers");
}
