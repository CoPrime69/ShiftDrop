#pragma once
#include "Engine.h"
#include "ImagePipeline.h"
#include "photo/Compose.h"

#include <cstdint>
#include <memory>
#include <string>

// The photo tools' engine: Edit Photos (photo.adjust), Annotate Photos (photo.annotate), Add a
// Background (photo.background) and Create a Collage (photo.collage). Each opens its editor on the
// image canvas (src/canvas) unless its recipe says `"ask": false`, then renders the full-size result
// with Direct2D on the GPU (WARP without one), keeps the format, carries the EXIF (src/metadata) and
// writes through StagedOutput.
namespace sd::photo
{
    // How a render went, for logs and the timing tests.
    struct RenderInfo
    {
        std::wstring device;   // "GPU", "WARP", "CPU"
        int64_t ms = 0;
        std::wstring note;     // why the CPU drew it, when the GPU couldn't
    };

    // The renders the engine runs, exposed for tests. Each fills `out` at full size.
    HRESULT RenderAdjusted(const canvas::Raster& in, const Adjustments& a, canvas::Raster& out, std::wstring& error, RenderInfo* info = nullptr,
                           bool allowGpu = true);
    HRESULT RenderAnnotated(const canvas::Raster& in, const Annotations& marks, const std::map<std::wstring, Picture>& images, canvas::Raster& out,
                            std::wstring& error, RenderInfo* info = nullptr);
    HRESULT RenderBackground(const canvas::Raster& in, const BackgroundParams& p, const canvas::Raster* backgroundImage, canvas::Raster& out,
                             std::wstring& error, RenderInfo* info = nullptr);
    HRESULT RenderCollage(const std::vector<Picture>& pictures, const CollageParams& p, canvas::Raster& out, std::wstring& error,
                          RenderInfo* info = nullptr);

    // One file's params of a per-file tool: the shared ones with params["perFile"][path] over them.
    nlohmann::json ParamsForFile(const nlohmann::json& params, const std::wstring& path);

    // For tests and the module: the actions and the built-in recipes.
    void RegisterActions(ActionRegistry& registry);
    const char* BuiltInRecipes();

    class PhotoEngine final : public IEngine
    {
    public:
        explicit PhotoEngine(const EngineServices& services);

        bool Handles(const Recipe& recipe) const override;
        bool Applies(const Recipe& recipe, const std::wstring& path) const override;
        bool AppliesToDrop(const Recipe& recipe, size_t fileCount) const override;
        bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const override;
        ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                       const ProgressFn& progress) const override;
        bool IsBatch(const Recipe& recipe) const override;
        ItemResult RunBatch(const Recipe& recipe, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token stop,
                            const ProgressFn& progress) const override;
        bool NeedsEditor(const Recipe& recipe) const override;
        void OpenEditor(HWND owner, const Recipe& recipe, const std::vector<std::wstring>& files,
                        std::function<void(Recipe adjusted, std::vector<std::wstring> files)> done) const override;

    private:
        EngineServices m_services;
        std::shared_ptr<const ImageCodecs> m_codecs;
    };
}
