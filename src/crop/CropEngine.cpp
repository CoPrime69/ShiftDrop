#include "crop/CropEngine.h"
#include "FileIo.h"
#include "Media.h"
#include "OutputPath.h"
#include "canvas/Editor.h"
#include "crop/CropMath.h"
#include "crop/CropTool.h"
#include "metadata/Container.h"
#include "metadata/Metadata.h"

#include <format>

namespace sd::crop
{
    CropEngine::CropEngine(const EngineServices& services, std::shared_ptr<const ImageCodecs> codecs)
        : m_services(services), m_codecs(std::move(codecs))
    {
    }

    bool CropEngine::Handles(const Recipe& recipe) const
    {
        return recipe.steps.size() == 1 && recipe.steps[0].action == "crop.image";
    }

    bool CropEngine::Applies(const Recipe&, const std::wstring& path) const
    {
        const auto ext = LowerExtension(path);
        return MediaKindOfExtension(ext) == MediaKind::None && m_codecs && m_codecs->CanDecode(ext);
    }

    bool CropEngine::Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const
    {
        if (Applies(recipe, path))
            return true;
        if (why)
            *why = std::format(L"no image decoder for '{}' on this PC", LowerExtension(path));
        return false;
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

        const auto& format = canvas::EditedFormatFor(LowerExtension(source));
        std::vector<uint8_t> bytes;
        if (FAILED(canvas::EncodeRaster(cropped, format, canvas::EditedQuality(format), bytes, error)))
            return out.Fail(error);
        // The picture's dates and camera stay with it; its old thumbnail (the uncropped picture) does not.
        std::vector<uint8_t> original;
        if (ReadFileBytes(source, original))
            metadata::CarryEditedExif(original, bytes, static_cast<int>(cropped.width), static_cast<int>(cropped.height));
        if (!WriteFileBytes(out.Path(), bytes, WriteMode::Any))
            return out.Fail(L"cannot write next to the source");
        if (progress)
            progress(1.0);
        return out.Commit(recipe.suffix, format.extension);
    }

    void CropEngine::OpenEditor(HWND, const Recipe& recipe, const std::vector<std::wstring>& files,
                                std::function<void(Recipe adjusted, std::vector<std::wstring> files)> done) const
    {
        canvas::OpenToolEditor(recipe, files, std::make_unique<CropTool>(files.size()), std::move(done));
    }
}
