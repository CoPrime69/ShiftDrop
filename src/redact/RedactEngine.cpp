#include "redact/RedactEngine.h"
#include "Media.h"
#include "OutputPath.h"
#include "canvas/Covers.h"
#include "canvas/Editor.h"
#include "metadata/Metadata.h"
#include "redact/RedactTool.h"

#include <format>

namespace sd::redact
{
    HRESULT WriteRedacted(const std::wstring& source, const nlohmann::json& params, const std::wstring& target, std::wstring& extension,
                          size_t& covered, std::stop_token stop, std::wstring& error)
    {
        covered = 0;
        canvas::Raster image;
        if (const HRESULT hr = canvas::LoadRaster(source, image, error); FAILED(hr))
            return hr;
        if (stop.stop_requested())
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        const auto covers = ToPixelCovers(CoversFor(params, source), static_cast<int>(image.width), static_cast<int>(image.height));
        canvas::ApplyCovers(image, covers);
        covered = covers.size();
        const auto& format = canvas::EditedFormatFor(LowerExtension(source));
        extension = format.extension;
        return canvas::SaveRasterToFile(image, format, canvas::EditedQuality(format), target, error);
    }

    RedactEngine::RedactEngine(const EngineServices& services, std::shared_ptr<const ImageCodecs> codecs)
        : m_services(services), m_codecs(std::move(codecs))
    {
    }

    bool RedactEngine::Handles(const Recipe& recipe) const
    {
        return recipe.steps.size() == 1 && recipe.steps[0].action == "redact.image";
    }

    bool RedactEngine::Applies(const Recipe&, const std::wstring& path) const
    {
        const auto ext = LowerExtension(path);
        return MediaKindOfExtension(ext) == MediaKind::None && m_codecs && m_codecs->CanDecode(ext);
    }

    bool RedactEngine::Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const
    {
        if (Applies(recipe, path))
            return true;
        if (why)
            *why = std::format(L"no image decoder for '{}' on this PC", LowerExtension(path));
        return false;
    }

    ItemResult RedactEngine::Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                                 const ProgressFn& progress) const
    {
        StagedOutput out(m_services.history, runId, source);
        if (!out.Ready())
            return out.Fail(out.Error());
        const auto& params = recipe.steps[0].params;
        if (CoversFor(params, source).empty())
            return out.Fail(L"nothing was covered on this picture", true);
        std::wstring extension, error;
        size_t covered = 0;
        const HRESULT hr = WriteRedacted(source, params, out.Path(), extension, covered, stop, error);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            return out.Fail(L"cancelled", true);
        if (FAILED(hr))
            return out.Fail(error);
        if (progress)
            progress(0.8);
        const auto check = metadata::VerifyNoMetadata(out.Path());
        if (!check.opens)
            return out.Fail(L"the redacted copy did not open again");
        if (!check.clean)
            return out.Fail(L"metadata was left in the copy: " + (check.leftovers.empty() ? std::wstring(L"unknown") : check.leftovers.front()));
        if (progress)
            progress(1.0);
        auto result = out.Commit(recipe.suffix, extension);
        if (result.ok)
            result.note = L"Covers burned in · all metadata removed";
        return result;
    }

    void RedactEngine::OpenEditor(HWND, const Recipe& recipe, const std::vector<std::wstring>& files,
                                  std::function<void(Recipe adjusted, std::vector<std::wstring> files)> done) const
    {
        canvas::OpenToolEditor(recipe, files, std::make_unique<RedactTool>(files, RedactTool::Mode::Redact), std::move(done));
    }
}
