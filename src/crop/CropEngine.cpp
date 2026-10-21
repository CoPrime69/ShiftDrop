#include "crop/CropEngine.h"
#include "canvas/Editor.h"
#include "crop/CropMath.h"
#include "crop/CropTool.h"

namespace sd::crop
{
    CropEngine::CropEngine(const EngineServices& services, std::shared_ptr<const ImageCodecs> codecs)
        : PictureEngine(services, std::move(codecs))
    {
    }

    bool CropEngine::Handles(const Recipe& recipe) const
    {
        return recipe.steps.size() == 1 && recipe.steps[0].action == "crop.image";
    }

    ItemResult CropEngine::Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                               const ProgressFn& progress) const
    {
        StagedOutput out(m_services.history, runId, source);
        if (!out.Ready())
            return out.Fail(out.Error());
        const CropParams params = ParseParams(recipe.steps[0].params);
        if (params.box.Empty())
            return out.Fail(L"no crop box was chosen");

        canvas::Raster image;
        std::wstring error;
        if (FAILED(canvas::LoadRaster(source, image, error)))
            return out.Fail(error);
        if (stop.stop_requested())
            return out.Fail(L"cancelled", true);
        if (progress)
            progress(0.4);

        const CropPlan plan = PlanCrop(params, static_cast<int>(image.width), static_cast<int>(image.height));
        canvas::Raster cropped;
        if (plan.box.w <= 0 || plan.box.h <= 0 || !RenderCrop(image, plan, cropped))
            return out.Fail(L"not enough memory to crop this picture");
        image = {};
        if (stop.stop_requested())
            return out.Fail(L"cancelled", true);
        if (progress)
            progress(0.7);

        auto result = canvas::CommitEdited(out, cropped, source, recipe.suffix, true);
        if (progress)
            progress(1.0);
        return result;
    }

    void CropEngine::OpenEditor(HWND, const Recipe& recipe, const std::vector<std::wstring>& files,
                                std::function<void(Recipe adjusted, std::vector<std::wstring> files)> done) const
    {
        canvas::OpenToolEditor(recipe, files, std::make_unique<CropTool>(files.size()), std::move(done));
    }
}
