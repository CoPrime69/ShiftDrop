#include "photo/PhotoEngine.h"
#include "ErrorText.h"
#include "Log.h"
#include "OutputPath.h"
#include "Utf.h"
#include "canvas/Editor.h"
#include "photo/Gpu.h"
#include "photo/PhotoTools.h"

#include <d2d1_1.h>

#include <chrono>
#include <format>
#include <set>

namespace sd::photo
{
    using canvas::Raster;

    namespace
    {
        enum class Tool { None, Adjust, Annotate, Background, Collage };

        Tool ToolOf(const Recipe& r)
        {
            if (r.steps.size() != 1)
                return Tool::None;
            const auto& a = r.steps[0].action;
            return a == "photo.adjust" ? Tool::Adjust : a == "photo.annotate" ? Tool::Annotate : a == "photo.background" ? Tool::Background
                                                     : a == "photo.collage"                         ? Tool::Collage
                                                                                                     : Tool::None;
        }

        std::shared_ptr<const Raster> Borrow(const Raster& r) { return std::shared_ptr<const Raster>(&r, [](const Raster*) {}); }

        int64_t Since(std::chrono::steady_clock::time_point start)
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        }

        // Draws `w` x `h` on the shared Direct2D device and reads it back; makes the device again once if
        // it was lost.
        HRESULT OnGpu(unsigned w, unsigned h, const std::function<HRESULT(ID2D1DeviceContext*)>& draw, Raster& out, std::wstring& error,
                      std::wstring* device)
        {
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                const auto gpu = gpu::Device::Get();
                if (!gpu)
                {
                    error = L"Direct2D isn't available on this PC";
                    return E_NOINTERFACE;
                }
                auto dc = gpu->NewContext();
                if (!dc)
                {
                    error = L"no Direct2D context";
                    return E_FAIL;
                }
                if (std::max(w, h) > gpu::MaxEdge(dc.Get()))
                {
                    error = std::format(L"the result would be {}x{}, larger than the {} pixels the GPU can draw", w, h, gpu::MaxEdge(dc.Get()));
                    return E_INVALIDARG;
                }
                const HRESULT hr = gpu::RenderToRaster(dc.Get(), w, h, draw, out);
                if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET))
                {
                    gpu::Device::Lost(gpu.get());
                    continue;
                }
                if (FAILED(hr))
                    error = L"drawing failed (" + HexCode(hr) + L")";
                if (device)
                    *device = gpu->Name();
                return hr;
            }
            error = L"the GPU was reset twice while drawing";
            return E_FAIL;
        }

        void Keep(const Raster& from, Raster& to)
        {
            to.dpiX = from.dpiX;
            to.dpiY = from.dpiY;
            to.icc = from.icc;
        }

        std::vector<ParamSpec> AdjustSpecs()
        {
            std::vector<ParamSpec> specs;
            for (const auto& s : Sliders())
                specs.push_back(ParamSpec{ s.key, ParamType::Number, s.min, s.max, {}, std::string_view(s.key) == "grainSize" ? 1.5 : 0.0 });
            specs.push_back(ParamSpec{ "ask", ParamType::Bool, 0, 0, {}, true });
            return specs;
        }

        std::vector<std::string> AspectIds(bool fitToPicture = false)
        {
            std::vector<std::string> ids;
            for (const auto& c : AspectChoices())
                ids.push_back(c.id);
            if (fitToPicture)
                ids.push_back(kFitToPicture);   // the Background tool's own
            return ids;
        }
    }

    // ---- Renders ------------------------------------------------------------------------------------

    HRESULT RenderAdjusted(const Raster& in, const Adjustments& a, Raster& out, std::wstring& error, RenderInfo* info, bool allowGpu)
    {
        const auto start = std::chrono::steady_clock::now();
        HazeModel haze;
        if (a.dehaze != 0)
            haze = AnalyseHaze(in);
        std::wstring device = L"CPU", note;
        bool done = false;
        if (allowGpu && gpu::Device::Get())
        {
            fx::AdjustGraph graph;
            Bitmaps bitmaps;
            CpuCopy cpu;
            const Picture pic = Picture::Of(Borrow(in));
            Keep(in, out);
            std::wstring gpuDevice;
            const HRESULT hr = OnGpu(in.width, in.height, [&](ID2D1DeviceContext* dc) { return DrawAdjusted(dc, 0, 1.0, pic, a, &haze, graph, bitmaps, cpu); },
                                     out, error, &gpuDevice);
            if (SUCCEEDED(hr))
            {
                done = true;
                device = graph.failed ? std::wstring(L"CPU") : gpuDevice;
                note = graph.why;
            }
            else
            {
                log::Warn(L"photo: the GPU render failed ({}); using the CPU", error);
                note = error;
            }
        }
        if (!done)
        {
            out = in;
            ApplyAdjustments(out, a, a.dehaze != 0 ? &haze : nullptr, 1.0);
            error.clear();
        }
        if (info)
            *info = { device, Since(start), note };
        return S_OK;
    }

    HRESULT RenderAnnotated(const Raster& in, const Annotations& marks, const std::map<std::wstring, Picture>& images, Raster& out, std::wstring& error,
                            RenderInfo* info)
    {
        const auto start = std::chrono::steady_clock::now();
        Annotations copy = marks;
        Bitmaps bitmaps;
        const Picture pic = Picture::Of(Borrow(in));
        Keep(in, out);
        std::wstring device;
        const HRESULT hr = OnGpu(in.width, in.height, [&](ID2D1DeviceContext* dc) { return DrawAnnotated(dc, 0, 1.0, pic, copy, images, bitmaps); }, out,
                                 error, &device);
        if (info)
            *info = { device, Since(start) };
        return hr;
    }

    HRESULT RenderBackground(const Raster& in, const BackgroundParams& p, const Raster* backgroundImage, Raster& out, std::wstring& error,
                             RenderInfo* info)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto layout = LayoutBackground(static_cast<int>(in.width), static_cast<int>(in.height), p);
        Bitmaps bitmaps;
        BackgroundCache cache;
        const Picture pic = Picture::Of(Borrow(in));
        const Picture back = backgroundImage ? Picture::Of(Borrow(*backgroundImage)) : Picture{};
        Keep(in, out);
        std::wstring device;
        const HRESULT hr = OnGpu(static_cast<unsigned>(layout.width), static_cast<unsigned>(layout.height),
                                 [&](ID2D1DeviceContext* dc) { return DrawBackground(dc, 0, 1.0, pic, p, layout, back, bitmaps, cache); }, out, error,
                                 &device);
        if (info)
            *info = { device, Since(start) };
        return hr;
    }

    HRESULT RenderCollage(const std::vector<Picture>& pictures, const CollageParams& p, Raster& out, std::wstring& error, RenderInfo* info)
    {
        const auto start = std::chrono::steady_clock::now();
        std::vector<SizeD> sizes;
        for (const auto& pic : pictures)
            sizes.push_back({ double(pic.width), double(pic.height) });
        const auto layout = LayoutCollage(sizes, p);
        Bitmaps bitmaps;
        std::wstring device;
        const HRESULT hr = OnGpu(static_cast<unsigned>(layout.width), static_cast<unsigned>(layout.height),
                                 [&](ID2D1DeviceContext* dc) { return DrawCollage(dc, 0, 1.0, pictures, p, layout, bitmaps); }, out, error, &device);
        if (info)
            *info = { device, Since(start) };
        return hr;
    }

    // ---- Actions and recipes ------------------------------------------------------------------------

    void RegisterActions(ActionRegistry& registry)
    {
        registry.Add(ActionDescriptor{ "photo.adjust", L"Edit Photos", StepKind::Engine, AdjustSpecs() });
        registry.Add(ActionDescriptor{ "photo.annotate", L"Annotate Photos", StepKind::Engine, {} });
        registry.Add(ActionDescriptor{
            "photo.background",
            L"Add a Background",
            StepKind::Engine,
            {
                ParamSpec{ "fill", ParamType::Enum, 0, 0, { "solid", "gradient", "blur", "image" }, "gradient" },
                ParamSpec{ "color1", ParamType::Int, 0, 0xFFFFFF, {}, 0x6A5ACD },
                ParamSpec{ "color2", ParamType::Int, 0, 0xFFFFFF, {}, 0xF08A5D },
                ParamSpec{ "angle", ParamType::Number, 0, 360, {}, 135 },
                ParamSpec{ "blur", ParamType::Number, 0, 100, {}, 40 },
                ParamSpec{ "aspect", ParamType::Enum, 0, 0, AspectIds(true), "auto" },
                ParamSpec{ "padding", ParamType::Number, 0, 40, {}, 8 },
                ParamSpec{ "radius", ParamType::Number, 0, 100, {}, 12 },
                ParamSpec{ "shadow", ParamType::Number, 0, 100, {}, 50 },
                ParamSpec{ "ask", ParamType::Bool, 0, 0, {}, true },
            } });
        registry.Add(ActionDescriptor{ "photo.collage",
                                       L"Create a Collage",
                                       StepKind::Engine,
                                       {
                                           ParamSpec{ "layout", ParamType::Enum, 0, 0, { "grid", "row", "column", "featured" }, "grid" },
                                           ParamSpec{ "size", ParamType::Int, 256, 16384, {}, 3000 },
                                           ParamSpec{ "aspect", ParamType::Enum, 0, 0, AspectIds(), "auto" },
                                           ParamSpec{ "spacing", ParamType::Number, 0, 100, {}, 20 },
                                           ParamSpec{ "radius", ParamType::Number, 0, 100, {}, 0 },
                                           ParamSpec{ "background", ParamType::Int, 0, 0xFFFFFF, {}, 0xFFFFFF },
                                           ParamSpec{ "fit", ParamType::Bool, 0, 0, {}, false },
                                           ParamSpec{ "ask", ParamType::Bool, 0, 0, {}, true },
                                       } });
    }

    const char* BuiltInRecipes()
    {
        // Icons (Segoe Fluent Icons): Brightness E706, Edit E70F, Color E790, ViewAll E80A.
        return R"([
  { "id": "edit-photos", "label": "Edit", "description": "Light, colour and detail: dehaze, clarity, grain, noise", "ring": "tools",
    "suffix": "edited", "icon": "E706", "steps": [ { "action": "photo.adjust" } ] },
  { "id": "annotate-photos", "label": "Mark up", "description": "Arrows, text, shapes, numbers, magnifiers, drawing", "ring": "tools",
    "suffix": "annotated", "icon": "E70F", "steps": [ { "action": "photo.annotate" } ] },
  { "id": "add-background", "label": "Frame", "description": "Put the picture on a backdrop with a shadow", "ring": "tools",
    "suffix": "background", "icon": "E790", "steps": [ { "action": "photo.background" } ] },
  { "id": "collage", "label": "Collage", "description": "One picture from all of them: grid, row, column or featured", "ring": "tools",
    "suffix": "collage", "icon": "E80A", "steps": [ { "action": "photo.collage" } ] }
])";
    }

    // ---- The engine ---------------------------------------------------------------------------------

    PhotoEngine::PhotoEngine(const EngineServices& services) : PictureEngine(services, std::make_shared<const ImageCodecs>()) {}

    bool PhotoEngine::Handles(const Recipe& recipe) const { return ToolOf(recipe) != Tool::None; }

    bool PhotoEngine::AppliesToDrop(const Recipe& recipe, size_t fileCount) const { return ToolOf(recipe) != Tool::Collage || fileCount >= 2; }

    bool PhotoEngine::IsBatch(const Recipe& recipe) const { return ToolOf(recipe) == Tool::Collage; }

    bool PhotoEngine::NeedsEditor(const Recipe& recipe) const
    {
        const auto tool = ToolOf(recipe);
        return tool == Tool::Annotate || recipe.steps[0].params.value("ask", true);
    }

    std::map<std::wstring, Picture> LoadInsertedPictures(const Annotations& marks)
    {
        std::map<std::wstring, Picture> images;
        for (const auto& m : marks.Items())
            if (m.kind == Mark::Image && !images.contains(m.image))
            {
                auto r = std::make_shared<Raster>();
                std::wstring e;
                if (SUCCEEDED(canvas::LoadRaster(m.image, *r, e)))
                    images[m.image] = Picture::Of(std::move(r));
                else
                    log::Warn(L"photo: the inserted picture {} could not be opened: {}", m.image, e);
            }
        return images;
    }

    ItemResult PhotoEngine::Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop, const ProgressFn& progress) const
    {
        StagedOutput out(m_services.history, runId, source);
        if (!out.Ready())
            return out.Fail(out.Error());
        const Tool tool = ToolOf(recipe);
        const nlohmann::json params = ParamsForFile(recipe.steps[0].params, source);
        Annotations marks;
        if (tool == Tool::Annotate)
        {
            marks = Annotations::FromJson(params);
            if (marks.Items().empty())
                return out.Fail(L"nothing was added to this picture", true);
        }

        Raster image;
        std::wstring error;
        if (FAILED(canvas::LoadRaster(source, image, error)))
            return out.Fail(error);
        if (stop.stop_requested())
            return out.Fail(L"cancelled", true);
        if (progress)
            progress(0.3);

        Raster result;
        RenderInfo info;
        HRESULT hr = E_FAIL;
        const wchar_t* what = L"";
        switch (tool)
        {
        case Tool::Adjust:
            what = L"edit";
            hr = RenderAdjusted(image, ParseAdjustments(params), result, error, &info);
            break;
        case Tool::Annotate:
        {
            what = L"annotate";
            hr = RenderAnnotated(image, marks, LoadInsertedPictures(marks), result, error, &info);
            break;
        }
        case Tool::Background:
        {
            what = L"background";
            const auto p = ParseBackground(params);
            Raster back;
            bool haveBack = false;
            if (p.fill == Fill::Image)
            {
                std::wstring e;
                haveBack = SUCCEEDED(canvas::LoadRaster(p.image, back, e, 4096));
                if (!haveBack)
                    log::Warn(L"photo: the background picture {} could not be opened: {}", p.image, e);
            }
            hr = RenderBackground(image, p, haveBack ? &back : nullptr, result, error, &info);
            break;
        }
        default:
            return out.Fail(L"not a photo tool");
        }
        if (FAILED(hr))
            return out.Fail(error.empty() ? std::wstring(L"the picture could not be drawn") : error);
        log::Info(L"photo: {} {}x{} -> {}x{} on {} in {} ms", what, image.width, image.height, result.width, result.height, info.device, info.ms);
        image = {};
        if (stop.stop_requested())
            return out.Fail(L"cancelled", true);
        if (progress)
            progress(0.8);
        auto r = canvas::CommitEdited(out, result, source, recipe.suffix, true);
        if (progress)
            progress(1.0);
        return r;
    }

    ItemResult PhotoEngine::RunBatch(const Recipe& recipe, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token stop,
                                     const ProgressFn& progress) const
    {
        const auto p = ParseCollage(recipe.steps[0].params);
        const auto order = NormalisedOrder(p.order, sources.size());
        StagedOutput out(m_services.history, runId, sources.at(static_cast<size_t>(order.front())));
        if (!out.Ready())
            return out.Fail(out.Error());
        if (sources.size() < 2)
            return out.Fail(L"drop two or more pictures to make a collage");

        // First only the shapes (a tiny decode), to lay the collage out; then each picture at the size
        // its cell needs, so twenty camera photos don't all sit in memory at full size.
        std::vector<SizeD> sizes(sources.size());
        std::wstring error;
        for (size_t i = 0; i < sources.size(); ++i)
        {
            Raster tiny;
            if (FAILED(canvas::LoadRaster(sources[i], tiny, error, 64)))
                return out.Fail(std::format(L"{}: {}", sources[i], error));
            sizes[i] = { double(tiny.width), double(tiny.height) };
            if (stop.stop_requested())
                return out.Fail(L"cancelled", true);
        }
        const auto layout = LayoutCollage(sizes, p);
        std::vector<Picture> pictures(sources.size());
        for (size_t c = 0; c < layout.cells.size(); ++c)
        {
            const auto& cell = layout.cells[c];
            const size_t i = static_cast<size_t>(cell.image);
            const double k = p.fit ? std::min(cell.rect.w / sizes[i].w, cell.rect.h / sizes[i].h) : std::max(cell.rect.w / sizes[i].w, cell.rect.h / sizes[i].h);
            const UINT edge = static_cast<UINT>(std::ceil(std::max(sizes[i].w, sizes[i].h) * k)) + 2;
            auto r = std::make_shared<Raster>();
            if (FAILED(canvas::LoadRaster(sources[i], *r, error, edge)))
                return out.Fail(std::format(L"{}: {}", sources[i], error));
            pictures[i] = Picture::Of(std::move(r));
            if (stop.stop_requested())
                return out.Fail(L"cancelled", true);
            if (progress)
                progress(0.7 * (c + 1) / layout.cells.size());
        }
        Raster result;
        RenderInfo info;
        if (FAILED(RenderCollage(pictures, p, result, error, &info)))
            return out.Fail(error);
        log::Info(L"photo: collage of {} -> {}x{} on {} in {} ms", sources.size(), result.width, result.height, info.device, info.ms);
        pictures.clear();
        if (progress)
            progress(0.9);
        // A new picture: the first photo's format, none of the photos' metadata.
        auto r = canvas::CommitEdited(out, result, sources[static_cast<size_t>(order.front())], recipe.suffix, false);
        if (progress)
            progress(1.0);
        return r;
    }

    void PhotoEngine::OpenEditor(HWND, const Recipe& recipe, const std::vector<std::wstring>& files,
                                 std::function<void(Recipe adjusted, std::vector<std::wstring> files)> done) const
    {
        std::unique_ptr<canvas::ICanvasTool> tool;
        const auto& params = recipe.steps[0].params;
        switch (ToolOf(recipe))
        {
        case Tool::Adjust: tool = MakeEditTool(files, params); break;
        case Tool::Annotate: tool = MakeAnnotateTool(files); break;
        case Tool::Background: tool = MakeBackgroundTool(files, params); break;
        case Tool::Collage: tool = MakeCollageTool(files, params); break;
        default: return;
        }
        canvas::OpenToolEditor(recipe, files, std::move(tool), std::move(done));
    }
}
