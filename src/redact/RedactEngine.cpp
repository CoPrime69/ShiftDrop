#include "redact/RedactEngine.h"
#include "canvas/Covers.h"
#include "canvas/Editor.h"
#include "metadata/Metadata.h"
#include "redact/RedactTool.h"

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
        const ImageFormatInfo* format = nullptr;
        const HRESULT hr = canvas::SaveEdited(image, source, target, error, &format);
        extension = format->extension;
        return hr;
    }

    RedactEngine::RedactEngine(const EngineServices& services, std::shared_ptr<const ImageCodecs> codecs)
        : PictureEngine(services, std::move(codecs))
    {
    }

    bool RedactEngine::Handles(const Recipe& recipe) const
    {
        return recipe.steps.size() == 1 && recipe.steps[0].action == "redact.image";
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
            result.message = L"Covers burned in, all metadata removed";
        return result;
    }

    void RedactEngine::OpenEditor(HWND, const Recipe& recipe, const std::vector<std::wstring>& files,
                                  std::function<void(Recipe adjusted, std::vector<std::wstring> files)> done) const
    {
        canvas::OpenToolEditor(recipe, files, std::make_unique<RedactTool>(files, RedactTool::Mode::Redact), std::move(done));
    }
}
