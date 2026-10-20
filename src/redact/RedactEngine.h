#pragma once
#include "Engine.h"
#include "ImagePipeline.h"

#include <memory>

namespace sd::redact
{
    // Redact Photos (tool 7): redact.image. The editor places the covers on each picture; the output
    // is the flattened picture (the covered pixels are gone, not hidden in a layer) with no metadata
    // at all, checked by reading the result back.
    class RedactEngine final : public IEngine
    {
    public:
        RedactEngine(const EngineServices& services, std::shared_ptr<const ImageCodecs> codecs);

        bool Handles(const Recipe& recipe) const override;
        bool Applies(const Recipe& recipe, const std::wstring& path) const override;
        bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const override;
        ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                       const ProgressFn& progress) const override;
        bool NeedsEditor(const Recipe&) const override { return true; }
        void OpenEditor(HWND owner, const Recipe& recipe, const std::vector<std::wstring>& files,
                        std::function<void(Recipe adjusted, std::vector<std::wstring> files)> done) const override;

    private:
        EngineServices m_services;
        std::shared_ptr<const ImageCodecs> m_codecs;
    };

    // Shared with Share-safe: paints `covers` into the picture at `source` and writes it, with no
    // metadata, to `target` (a staging file). `extension` gets the output's extension.
    HRESULT WriteRedacted(const std::wstring& source, const nlohmann::json& params, const std::wstring& target, std::wstring& extension,
                          size_t& covered, std::stop_token stop, std::wstring& error);
}
