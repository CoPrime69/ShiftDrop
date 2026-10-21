#pragma once
#include "canvas/PictureEngine.h"

#include <memory>

namespace sd::crop
{
    // Crop Images (tool 6): crop.image. The editor picks the box on the first picture; every picture
    // of the drop gets the same crop (centred on the others by default), saved in its own format.
    class CropEngine final : public canvas::PictureEngine
    {
    public:
        CropEngine(const EngineServices& services, std::shared_ptr<const ImageCodecs> codecs);

        bool Handles(const Recipe& recipe) const override;
        ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                       const ProgressFn& progress) const override;
        bool NeedsEditor(const Recipe&) const override { return true; }
        void OpenEditor(HWND owner, const Recipe& recipe, const std::vector<std::wstring>& files,
                        std::function<void(Recipe adjusted, std::vector<std::wstring> files)> done) const override;
    };
}
