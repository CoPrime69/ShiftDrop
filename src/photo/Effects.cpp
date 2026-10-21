#include "photo/Effects.h"
#include "Log.h"
#include "photo/Gpu.h"

#include <d2d1effectauthor.h>
#include <d2d1effects.h>
#include <d3dcompiler.h>

#include <array>
#include <cmath>
#include <new>
#include <cstring>
#include <mutex>

// The shared per-pixel text as one string, for the shaders.
#define SD_STRINGIZE_ALL(...) #__VA_ARGS__
#define SD_STRINGIZE(...) SD_STRINGIZE_ALL(__VA_ARGS__)

namespace sd::photo::fx
{
    namespace
    {
        // ---- The shaders ------------------------------------------------------------------------
        // Direct2D's vertex shader hands each pixel shader the pixel's scene position and one texture
        // coordinate per input; inputs are premultiplied, so each shader works on the straight colour
        // and premultiplies its result again.
        const char* const kShared = SD_STRINGIZE(SD_PHOTO_SHADE_CONSTANTS SD_PHOTO_SHADE_FUNCTIONS);

        const char* const kDenoise = R"(
Texture2D InputTexture0 : register(t0);
SamplerState InputSampler0 : register(s0);
cbuffer Constants : register(b0) { DenoiseK K; };
float4 main(float4 pos : SV_POSITION, float4 scene : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_Target
{
    float w, h;
    InputTexture0.GetDimensions(w, h);
    float2 texel = float2(1.0f / w, 1.0f / h);
    float4 c0 = InputTexture0.Sample(InputSampler0, uv0.xy);
    if (c0.a <= 0.0f)
        return c0;
    float l0 = Luma(c0.rgb / c0.a);
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float total = 0.0f;
    [unroll] for (int j = -3; j <= 3; ++j)
    {
        [unroll] for (int i = -3; i <= 3; ++i)
        {
            float4 s = InputTexture0.Sample(InputSampler0, uv0.xy + float2(i, j) * K.step * texel);
            float3 c = s.a > 0.0f ? s.rgb / s.a : float3(0.0f, 0.0f, 0.0f);
            float wt = BilateralWeight(float(i * i + j * j), Luma(c) - l0, K) * s.a;
            sum += c * wt;
            total += wt;
        }
    }
    return float4(sum / max(total, 0.000001f) * c0.a, c0.a);
}
)";

        const char* const kDevelop = R"(
Texture2D InputTexture0 : register(t0);
Texture2D InputTexture1 : register(t1);
SamplerState InputSampler0 : register(s0);
SamplerState InputSampler1 : register(s1);
cbuffer Constants : register(b0) { DevelopK K; };
float4 main(float4 pos : SV_POSITION, float4 scene : SCENE_POSITION, float4 uv0 : TEXCOORD0, float4 uv1 : TEXCOORD1) : SV_Target
{
    float4 c = InputTexture0.Sample(InputSampler0, uv0.xy);
    if (c.a <= 0.0f)
        return c;
    float3 rgb = c.rgb / c.a;
    float haze = 0.0f;
    if (K.dehaze != 0.0f)
    {
        float4 m = InputTexture1.Sample(InputSampler1, uv1.xy);
        float2 ab = m.a > 0.0f ? m.rg / m.a : float2(0.0f, 0.0f);
        haze = saturate(ab.x * Luma(rgb) + ab.y);
    }
    return float4(Develop(rgb, haze, K) * c.a, c.a);
}
)";

        const char* const kFinish = R"(
Texture2D InputTexture0 : register(t0);
Texture2D InputTexture1 : register(t1);
Texture2D InputTexture2 : register(t2);
SamplerState InputSampler0 : register(s0);
SamplerState InputSampler1 : register(s1);
SamplerState InputSampler2 : register(s2);
cbuffer Constants : register(b0) { FinishK K; };
float4 main(float4 pos : SV_POSITION, float4 scene : SCENE_POSITION, float4 uv0 : TEXCOORD0, float4 uv1 : TEXCOORD1, float4 uv2 : TEXCOORD2) : SV_Target
{
    float4 c = InputTexture0.Sample(InputSampler0, uv0.xy);
    if (c.a <= 0.0f)
        return c;
    float3 rgb = c.rgb / c.a;
    float4 s = InputTexture1.Sample(InputSampler1, uv1.xy);
    float4 l = InputTexture2.Sample(InputSampler2, uv2.xy);
    float3 fine = s.a > 0.0f ? s.rgb / s.a : rgb;
    float3 broad = l.a > 0.0f ? l.rgb / l.a : rgb;
    return float4(Finish(rgb, fine, broad, scene.x, scene.y, K) * c.a, c.a);
}
)";

        enum Kind { kDenoiseFx, kDevelopFx, kFinishFx, kKinds };

        struct KindInfo
        {
            GUID clsid;
            GUID shader;
            UINT inputs;
            const char* name;
            const char* body;
        };
        // Our own effect ids (random GUIDs, registered only in this process).
        const KindInfo kInfo[kKinds] = {
            { { 0x7d1c5a20, 0x3f4e, 0x4b8a, { 0x9e, 0x61, 0x2c, 0x05, 0x8b, 0x3d, 0x71, 0xa1 } },
              { 0x7d1c5a21, 0x3f4e, 0x4b8a, { 0x9e, 0x61, 0x2c, 0x05, 0x8b, 0x3d, 0x71, 0xa1 } }, 1, "denoise", kDenoise },
            { { 0x7d1c5a22, 0x3f4e, 0x4b8a, { 0x9e, 0x61, 0x2c, 0x05, 0x8b, 0x3d, 0x71, 0xa1 } },
              { 0x7d1c5a23, 0x3f4e, 0x4b8a, { 0x9e, 0x61, 0x2c, 0x05, 0x8b, 0x3d, 0x71, 0xa1 } }, 2, "develop", kDevelop },
            { { 0x7d1c5a24, 0x3f4e, 0x4b8a, { 0x9e, 0x61, 0x2c, 0x05, 0x8b, 0x3d, 0x71, 0xa1 } },
              { 0x7d1c5a25, 0x3f4e, 0x4b8a, { 0x9e, 0x61, 0x2c, 0x05, 0x8b, 0x3d, 0x71, 0xa1 } }, 3, "finish", kFinish },
        };

        // ---- Compiling ------------------------------------------------------------------------------
        // d3dcompiler_47.dll ships with Windows 10 and 11 (System32), so it is loaded from there at run
        // time rather than linked.
        struct Compiled
        {
            std::once_flag once;
            bool ok = false;
            std::wstring error;
            std::array<std::vector<BYTE>, kKinds> code;
        };

        Compiled& Shaders()
        {
            static Compiled compiled;
            std::call_once(compiled.once, [] {
                // Windows' own copy only (System32), never one planted next to the app.
                HMODULE dll = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                const auto compile = dll ? reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(dll, "D3DCompile"))) : nullptr;
                if (!compile)
                {
                    compiled.error = L"the shader compiler (d3dcompiler_47.dll) is missing";
                    return;
                }
                for (int k = 0; k < kKinds; ++k)
                {
                    const std::string source = std::string(kShared) + "\n" + kInfo[k].body;
                    ComPtr<ID3DBlob> code, errors;
                    const HRESULT hr = compile(source.data(), source.size(), kInfo[k].name, nullptr, nullptr, "main", "ps_4_0",
                                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
                    if (FAILED(hr) || !code)
                    {
                        const std::string text = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
                        compiled.error = std::wstring(L"shader ") + std::wstring(kInfo[k].name, kInfo[k].name + std::strlen(kInfo[k].name)) +
                                         L": " + std::wstring(text.begin(), text.end());
                        return;
                    }
                    const auto* bytes = static_cast<const BYTE*>(code->GetBufferPointer());
                    compiled.code[k].assign(bytes, bytes + code->GetBufferSize());
                }
                compiled.ok = true;
            });
            return compiled;
        }

        // ---- The effect ---------------------------------------------------------------------------
        // One class for the three effects: a single draw transform running the kind's pixel shader with
        // up to 64 bytes of constants, set through four vector4 properties C0..C3.
        class ShaderEffect final : public ID2D1EffectImpl, public ID2D1DrawTransform
        {
        public:
            explicit ShaderEffect(Kind kind) : m_kind(kind) {}

            template <int K>
            static HRESULT CALLBACK Create(IUnknown** out)
            {
                *out = static_cast<ID2D1EffectImpl*>(new (std::nothrow) ShaderEffect(static_cast<Kind>(K)));
                return *out ? S_OK : E_OUTOFMEMORY;
            }

            // Property callbacks get the IUnknown the factory returned: our ID2D1EffectImpl.
            template <int I>
            static HRESULT CALLBACK SetC(IUnknown* effect, const BYTE* data, UINT32 size)
            {
                if (size != 16)
                    return E_INVALIDARG;
                auto* self = static_cast<ShaderEffect*>(reinterpret_cast<ID2D1EffectImpl*>(effect));
                std::memcpy(self->m_constants + I * 4, data, 16);
                return S_OK;
            }
            template <int I>
            static HRESULT CALLBACK GetC(const IUnknown* effect, BYTE* data, UINT32 size, UINT32* actual)
            {
                if (actual)
                    *actual = 16;
                if (!data || size == 0)
                    return S_OK;
                if (size < 16)
                    return E_NOT_SUFFICIENT_BUFFER;
                const auto* self = static_cast<const ShaderEffect*>(reinterpret_cast<const ID2D1EffectImpl*>(effect));
                std::memcpy(data, self->m_constants + I * 4, 16);
                return S_OK;
            }

            // IUnknown
            STDMETHOD(QueryInterface)(REFIID iid, void** out) override
            {
                if (!out)
                    return E_POINTER;
                if (iid == __uuidof(IUnknown) || iid == __uuidof(ID2D1EffectImpl))
                    *out = static_cast<ID2D1EffectImpl*>(this);
                else if (iid == __uuidof(ID2D1DrawTransform) || iid == __uuidof(ID2D1Transform) || iid == __uuidof(ID2D1TransformNode))
                    *out = static_cast<ID2D1DrawTransform*>(this);
                else
                {
                    *out = nullptr;
                    return E_NOINTERFACE;
                }
                AddRef();
                return S_OK;
            }
            STDMETHOD_(ULONG, AddRef)() override { return static_cast<ULONG>(InterlockedIncrement(&m_refs)); }
            STDMETHOD_(ULONG, Release)() override
            {
                const ULONG left = static_cast<ULONG>(InterlockedDecrement(&m_refs));
                if (left == 0)
                    delete this;
                return left;
            }

            // ID2D1EffectImpl
            STDMETHOD(Initialize)(ID2D1EffectContext* context, ID2D1TransformGraph* graph) override
            {
                const auto& code = Shaders().code[m_kind];
                if (code.empty())
                    return E_FAIL;
                if (HRESULT hr = context->LoadPixelShader(kInfo[m_kind].shader, code.data(), static_cast<UINT32>(code.size())); FAILED(hr))
                    return hr;
                return graph->SetSingleTransformNode(static_cast<ID2D1DrawTransform*>(this));
            }
            STDMETHOD(PrepareForRender)(D2D1_CHANGE_TYPE) override
            {
                return m_info ? m_info->SetPixelShaderConstantBuffer(reinterpret_cast<const BYTE*>(m_constants), sizeof(m_constants)) : S_OK;
            }
            STDMETHOD(SetGraph)(ID2D1TransformGraph*) override { return E_NOTIMPL; }

            // ID2D1TransformNode
            STDMETHOD_(UINT32, GetInputCount)() const override { return kInfo[m_kind].inputs; }

            // ID2D1Transform: noise reduction reads up to three taps of `step` pixels around each pixel.
            STDMETHOD(MapOutputRectToInputRects)(const D2D1_RECT_L* output, D2D1_RECT_L* inputs, UINT32 count) const override
            {
                const LONG r = Reach();
                for (UINT32 i = 0; i < count; ++i)
                    inputs[i] = { output->left - r, output->top - r, output->right + r, output->bottom + r };
                return S_OK;
            }
            STDMETHOD(MapInputRectsToOutputRect)(const D2D1_RECT_L* inputs, const D2D1_RECT_L*, UINT32 count, D2D1_RECT_L* output,
                                                 D2D1_RECT_L* opaque) override
            {
                *output = count ? inputs[0] : D2D1_RECT_L{};
                *opaque = {};
                return S_OK;
            }
            STDMETHOD(MapInvalidRect)(UINT32, D2D1_RECT_L input, D2D1_RECT_L* output) const override
            {
                const LONG r = Reach();
                *output = { input.left - r, input.top - r, input.right + r, input.bottom + r };
                return S_OK;
            }

            // ID2D1DrawTransform
            STDMETHOD(SetDrawInfo)(ID2D1DrawInfo* info) override
            {
                m_info = info;
                // Exact texels for the filter taps.
                for (UINT32 i = 0; i < kInfo[m_kind].inputs; ++i)
                    info->SetInputDescription(i, D2D1_INPUT_DESCRIPTION{ D2D1_FILTER_MIN_MAG_MIP_POINT, 0 });
                return info->SetPixelShader(kInfo[m_kind].shader, D2D1_PIXEL_OPTIONS_NONE);
            }

        private:
            LONG Reach() const { return m_kind == kDenoiseFx ? static_cast<LONG>(std::ceil(m_constants[0])) * 3 + 1 : 0; }

            Kind m_kind;
            LONG m_refs = 1;
            ComPtr<ID2D1DrawInfo> m_info;
            float m_constants[16]{};
        };

        const wchar_t* const kXml[kKinds] = {
            LR"(<?xml version='1.0'?>
<Effect>
  <Property name='DisplayName' type='string' value='Photo noise reduction'/>
  <Property name='Author' type='string' value='ShiftDrop'/>
  <Property name='Category' type='string' value='Photo'/>
  <Property name='Description' type='string' value='Edge-preserving smoothing'/>
  <Inputs><Input name='Source'/></Inputs>
  <Property name='C0' type='vector4'><Property name='DisplayName' type='string' value='C0'/></Property>
  <Property name='C1' type='vector4'><Property name='DisplayName' type='string' value='C1'/></Property>
  <Property name='C2' type='vector4'><Property name='DisplayName' type='string' value='C2'/></Property>
  <Property name='C3' type='vector4'><Property name='DisplayName' type='string' value='C3'/></Property>
</Effect>)",
            LR"(<?xml version='1.0'?>
<Effect>
  <Property name='DisplayName' type='string' value='Photo develop'/>
  <Property name='Author' type='string' value='ShiftDrop'/>
  <Property name='Category' type='string' value='Photo'/>
  <Property name='Description' type='string' value='Dehaze, white balance, exposure, tone and colour'/>
  <Inputs><Input name='Source'/><Input name='Haze'/></Inputs>
  <Property name='C0' type='vector4'><Property name='DisplayName' type='string' value='C0'/></Property>
  <Property name='C1' type='vector4'><Property name='DisplayName' type='string' value='C1'/></Property>
  <Property name='C2' type='vector4'><Property name='DisplayName' type='string' value='C2'/></Property>
  <Property name='C3' type='vector4'><Property name='DisplayName' type='string' value='C3'/></Property>
</Effect>)",
            LR"(<?xml version='1.0'?>
<Effect>
  <Property name='DisplayName' type='string' value='Photo finish'/>
  <Property name='Author' type='string' value='ShiftDrop'/>
  <Property name='Category' type='string' value='Photo'/>
  <Property name='Description' type='string' value='Clarity, sharpening, vignette and grain'/>
  <Inputs><Input name='Source'/><Input name='Small'/><Input name='Large'/></Inputs>
  <Property name='C0' type='vector4'><Property name='DisplayName' type='string' value='C0'/></Property>
  <Property name='C1' type='vector4'><Property name='DisplayName' type='string' value='C1'/></Property>
  <Property name='C2' type='vector4'><Property name='DisplayName' type='string' value='C2'/></Property>
  <Property name='C3' type='vector4'><Property name='DisplayName' type='string' value='C3'/></Property>
</Effect>)",
        };

        const D2D1_PROPERTY_BINDING kBindings[] = {
            { L"C0", &ShaderEffect::SetC<0>, &ShaderEffect::GetC<0> },
            { L"C1", &ShaderEffect::SetC<1>, &ShaderEffect::GetC<1> },
            { L"C2", &ShaderEffect::SetC<2>, &ShaderEffect::GetC<2> },
            { L"C3", &ShaderEffect::SetC<3>, &ShaderEffect::GetC<3> },
        };
        const PD2D1_EFFECT_FACTORY kFactories[kKinds] = { &ShaderEffect::Create<0>, &ShaderEffect::Create<1>, &ShaderEffect::Create<2> };

        // Makes one of our effects, registering them with the context's factory the first time.
        HRESULT MakeEffect(ID2D1DeviceContext* dc, Kind kind, ComPtr<ID2D1Effect>& out)
        {
            if (!Shaders().ok)
                return E_FAIL;
            HRESULT hr = dc->CreateEffect(kInfo[kind].clsid, &out);
            if (hr != D2DERR_EFFECT_IS_NOT_REGISTERED)
                return hr;
            ComPtr<ID2D1Factory> base;
            dc->GetFactory(&base);
            ComPtr<ID2D1Factory1> factory;
            if (FAILED(hr = base.As(&factory)))
                return hr;
            for (int k = 0; k < kKinds; ++k)
                if (FAILED(hr = factory->RegisterEffectFromString(kInfo[k].clsid, kXml[k], kBindings, 4, kFactories[k])))
                    return hr;
            return dc->CreateEffect(kInfo[kind].clsid, &out);
        }

        template <typename K>
        HRESULT SetConstants(ID2D1Effect* effect, const K& k)
        {
            static_assert(sizeof(K) % 16 == 0 && sizeof(K) <= 64);
            const auto* f = reinterpret_cast<const float*>(&k);
            for (UINT32 i = 0; i < sizeof(K) / 16; ++i)
                if (HRESULT hr = effect->SetValue(i, D2D1_PROPERTY_TYPE_VECTOR4, reinterpret_cast<const BYTE*>(f + i * 4), 16); FAILED(hr))
                    return hr;
            return S_OK;
        }

        HRESULT SetBlur(ID2D1Effect* blur, double sigma)
        {
            const float sd = static_cast<float>(std::max(0.0, sigma));
            const UINT32 hard = D2D1_BORDER_MODE_HARD, balanced = 1;   // D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED
            HRESULT hr = blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, D2D1_PROPERTY_TYPE_FLOAT, reinterpret_cast<const BYTE*>(&sd), 4);
            if (SUCCEEDED(hr))
                hr = blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_PROPERTY_TYPE_ENUM, reinterpret_cast<const BYTE*>(&hard), 4);
            if (SUCCEEDED(hr))
                hr = blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_PROPERTY_TYPE_ENUM, reinterpret_cast<const BYTE*>(&balanced), 4);
            return hr;
        }

        ComPtr<ID2D1Image> Fail(AdjustGraph& graph, const std::wstring& why)
        {
            if (!graph.failed)
                log::Warn(L"photo: the GPU effects are not available here ({}); using the CPU", why);
            graph.failed = true;
            graph.why = why;
            return nullptr;
        }
    }

    std::string ShaderSource(const std::string& name)
    {
        for (int k = 0; k < kKinds; ++k)
            if (name == kInfo[k].name)
                return std::string(kShared) + "\n" + kInfo[k].body;
        return {};
    }

    bool CompileShaders(std::wstring& error)
    {
        const auto& s = Shaders();
        error = s.error;
        return s.ok;
    }

    void PrepareContext(ID2D1DeviceContext* dc)
    {
        D2D1_RENDERING_CONTROLS controls{};
        dc->GetRenderingControls(&controls);
        if (dc->IsBufferPrecisionSupported(D2D1_BUFFER_PRECISION_16BPC_FLOAT))
            controls.bufferPrecision = D2D1_BUFFER_PRECISION_16BPC_FLOAT;
        else if (dc->IsBufferPrecisionSupported(D2D1_BUFFER_PRECISION_32BPC_FLOAT))
            controls.bufferPrecision = D2D1_BUFFER_PRECISION_32BPC_FLOAT;
        dc->SetRenderingControls(controls);
    }

    ComPtr<ID2D1Image> Adjust(ID2D1DeviceContext* dc, AdjustGraph& graph, ID2D1Image* source, unsigned width, unsigned height, unsigned fullWidth,
                              unsigned fullHeight, double scale, const Adjustments& a, const HazeModel* haze)
    {
        if (graph.dc != dc)
            graph = AdjustGraph{ dc };
        if (graph.failed)
            return nullptr;
        if (a.IsIdentity())
            return source;
        std::wstring error;
        if (!CompileShaders(error))
            return Fail(graph, error);

        ComPtr<ID2D1Image> image = source;
        HRESULT hr = S_OK;
        if (NeedsDenoise(a))
        {
            if (!graph.denoise && FAILED(hr = MakeEffect(dc, kDenoiseFx, graph.denoise)))
                return Fail(graph, L"noise reduction effect " + gpu::HrText(hr));
            graph.denoise->SetInput(0, image.Get());
            SetConstants(graph.denoise.Get(), DenoiseConstants(a, fullWidth, fullHeight, scale));
            graph.denoise->GetOutput(&image);
        }

        if (NeedsDevelop(a))
        {
            if (!graph.develop && FAILED(hr = MakeEffect(dc, kDevelopFx, graph.develop)))
                return Fail(graph, L"develop effect " + gpu::HrText(hr));
            const bool dehaze = a.dehaze != 0 && haze && !haze->Empty();
            ComPtr<ID2D1Image> hazeImage = image;
            if (dehaze)
            {
                if (graph.hazeFor != haze || !graph.hazeMap)
                {
                    graph.hazeMap = gpu::UploadFloat(dc, haze->a.width, haze->a.height, haze->a.v.data(), haze->b.v.data(), nullptr);
                    graph.hazeFor = haze;
                }
                if (!graph.hazeScale && FAILED(hr = dc->CreateEffect(CLSID_D2D12DAffineTransform, &graph.hazeScale)))
                    return Fail(graph, L"scale effect " + gpu::HrText(hr));
                if (!graph.hazeMap)
                    return Fail(graph, L"no float bitmaps");
                // Stretched over the picture with linear filtering (D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR).
                const auto m = D2D1::Matrix3x2F::Scale(float(width) / haze->a.width, float(height) / haze->a.height);
                const UINT32 linear = 1, hard = D2D1_BORDER_MODE_HARD;
                graph.hazeScale->SetValue(0, D2D1_PROPERTY_TYPE_ENUM, reinterpret_cast<const BYTE*>(&linear), 4);
                graph.hazeScale->SetValue(1, D2D1_PROPERTY_TYPE_ENUM, reinterpret_cast<const BYTE*>(&hard), 4);
                graph.hazeScale->SetValue(2, D2D1_PROPERTY_TYPE_MATRIX_3X2, reinterpret_cast<const BYTE*>(&m), sizeof(m));
                graph.hazeScale->SetInput(0, graph.hazeMap.Get());
                graph.hazeScale->GetOutput(&hazeImage);
            }
            graph.develop->SetInput(0, image.Get());
            graph.develop->SetInput(1, hazeImage.Get());
            auto k = DevelopConstants(a, dehaze ? haze->atmosphere : nullptr);
            if (!dehaze) k.dehaze = 0;
            SetConstants(graph.develop.Get(), k);
            graph.develop->GetOutput(&image);
        }

        if (NeedsFinish(a))
        {
            if (!graph.finish && FAILED(hr = MakeEffect(dc, kFinishFx, graph.finish)))
                return Fail(graph, L"finish effect " + gpu::HrText(hr));
            ComPtr<ID2D1Image> fine = image, broad = image;
            const Radii radii = RadiiFor(fullWidth, fullHeight, a);
            if (a.sharpen != 0)
            {
                if (!graph.smallBlur && FAILED(hr = dc->CreateEffect(CLSID_D2D1GaussianBlur, &graph.smallBlur)))
                    return Fail(graph, L"blur effect " + gpu::HrText(hr));
                graph.smallBlur->SetInput(0, image.Get());
                SetBlur(graph.smallBlur.Get(), radii.sharpenSigma * scale);
                graph.smallBlur->GetOutput(&fine);
            }
            if (a.clarity != 0)
            {
                if (!graph.largeBlur && FAILED(hr = dc->CreateEffect(CLSID_D2D1GaussianBlur, &graph.largeBlur)))
                    return Fail(graph, L"blur effect " + gpu::HrText(hr));
                graph.largeBlur->SetInput(0, image.Get());
                SetBlur(graph.largeBlur.Get(), radii.claritySigma * scale);
                graph.largeBlur->GetOutput(&broad);
            }
            graph.finish->SetInput(0, image.Get());
            graph.finish->SetInput(1, fine.Get());
            graph.finish->SetInput(2, broad.Get());
            SetConstants(graph.finish.Get(), FinishConstants(a, width, height, scale));
            graph.finish->GetOutput(&image);
        }
        return image;
    }
}
