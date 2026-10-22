#include "imagetools/ImageToolsModule.h"
#include "ImagePipeline.h"
#include "crop/CropEngine.h"
#include "redact/RedactEngine.h"
#include "sharesafe/ShareSafeEngine.h"

namespace sd
{
    namespace
    {
        void RegisterActions(ActionRegistry& registry)
        {
            registry.Add(ActionDescriptor{ "crop.image", L"Crop", StepKind::Engine, {} });
            registry.Add(ActionDescriptor{ "redact.image", L"Redact", StepKind::Engine, {} });
            // findDetails: look for emails, phone numbers, card numbers and IBANs written in pictures.
            registry.Add(ActionDescriptor{ "sharesafe.clean", L"Share-safe", StepKind::Engine,
                                           { ParamSpec{ "findDetails", ParamType::Bool, 0, 0, {}, true } } });
            registry.Add(ActionDescriptor{ "metadata.remove", L"Remove metadata", StepKind::Engine, {} });
        }

        const char* Recipes()
        {
            // Icons (Segoe Fluent Icons): Crop E7A8, Hide ED1A, Shield EA18, Tag E8EC.
            return R"([
  { "id": "crop", "label": "Crop", "description": "Crop to a box, a shape or an exact size", "ring": "tools", "suffix": "cropped",
    "icon": "E7A8", "steps": [ { "action": "crop.image" } ] },
  { "id": "redact", "label": "Redact", "description": "Cover faces, text and details for good", "ring": "tools", "suffix": "redacted",
    "icon": "ED1A", "steps": [ { "action": "redact.image" } ] },
  { "id": "share-safe", "label": "Private", "description": "A copy without location, camera or personal details", "ring": "tools",
    "suffix": "share-safe", "icon": "EA18", "steps": [ { "action": "sharesafe.clean" } ] },
  { "id": "remove-metadata", "label": "Remove metadata", "description": "A copy without EXIF, GPS or tags", "ring": "files",
    "suffix": "no metadata", "icon": "E8EC", "steps": [ { "action": "metadata.remove" } ] }
])";
        }

        // One engine for the module: each request goes to the tool that owns its action.
        class ImageToolsEngine final : public IEngine
        {
        public:
            explicit ImageToolsEngine(const EngineServices& services)
            {
                auto codecs = std::make_shared<const ImageCodecs>();
                m_tools.push_back(std::make_unique<crop::CropEngine>(services, codecs));
                m_tools.push_back(std::make_unique<redact::RedactEngine>(services, codecs));
                m_tools.push_back(std::make_unique<sharesafe::ShareSafeEngine>(services, codecs));
            }

            bool Handles(const Recipe& r) const override { return For(r) != nullptr; }
            bool Applies(const Recipe& r, const std::wstring& p) const override { return For(r)->Applies(r, p); }
            bool Accepts(const Recipe& r, const std::wstring& p, std::wstring* why) const override { return For(r)->Accepts(r, p, why); }
            bool IsNoOp(const Recipe& r, const std::wstring& p) const override { return For(r)->IsNoOp(r, p); }
            JobLane Lane(const Recipe& r) const override { return For(r)->Lane(r); }
            ItemResult Run(const Recipe& r, int64_t runId, const std::wstring& source, std::stop_token stop, const ProgressFn& progress) const override
            {
                return For(r)->Run(r, runId, source, stop, progress);
            }
            bool NeedsEditor(const Recipe& r) const override { return For(r)->NeedsEditor(r); }
            void OpenEditor(HWND owner, const Recipe& r, const std::vector<std::wstring>& files,
                            std::function<void(Recipe, std::vector<std::wstring>)> done) const override
            {
                For(r)->OpenEditor(owner, r, files, std::move(done));
            }

        private:
            const IEngine* For(const Recipe& r) const
            {
                for (const auto& t : m_tools)
                    if (t->Handles(r))
                        return t.get();
                return nullptr;
            }
            std::vector<std::unique_ptr<IEngine>> m_tools;
        };

        std::unique_ptr<IEngine> MakeEngine(const EngineServices& services) { return std::make_unique<ImageToolsEngine>(services); }
    }

    FeatureModule ImageToolsModule()
    {
        return FeatureModule{ "imagetools", &RegisterActions, &Recipes, &MakeEngine };
    }
}
