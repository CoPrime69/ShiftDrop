#include "pdf/PdfModule.h"
#include "ImagePipeline.h"
#include "OutputPath.h"
#include "PathUtil.h"
#include "ocr/OcrEngine.h"
#include "pdf/PdfEditors.h"
#include "share/Share.h"

#include <windows.h>

#include <filesystem>
#include <fstream>

namespace sd::pdf
{
    EditorHooks& Editors()
    {
        static EditorHooks hooks;   // MergeWindow.cpp / OrganizeWindow.cpp fill these in at startup
        return hooks;
    }

    namespace
    {
        void RegisterActions(ActionRegistry& registry)
        {
            registry.Add(ActionDescriptor{ "pdf.to_images", L"PDF to images", StepKind::Engine,
                                           { ParamSpec{ "format", ParamType::Enum, 0, 0, { "png", "jpg" }, "png" },
                                             ParamSpec{ "dpi", ParamType::Int, 36, 600, {}, 300 },
                                             ParamSpec{ "quality", ParamType::Int, 1, 100, {}, 90 } } });
            registry.Add(ActionDescriptor{ "pdf.to_text", L"PDF to text", StepKind::Engine,
                                           { ParamSpec{ "format", ParamType::Enum, 0, 0, { "txt", "md" }, "txt" },
                                             ParamSpec{ "ocr", ParamType::Bool, 0, 0, {}, true } } });
            registry.Add(ActionDescriptor{ "pdf.from_images", L"Images to PDF", StepKind::Engine, {} });
            registry.Add(ActionDescriptor{ "pdf.merge", L"Merge PDFs", StepKind::Engine, {} });
            // The page plan is added by the Organize window, not written in recipes.
            registry.Add(ActionDescriptor{ "pdf.organize", L"Organize PDF pages", StepKind::Engine, {} });
            registry.Add(ActionDescriptor{ "pdf.compress", L"Compress PDF", StepKind::Engine,
                                           { ParamSpec{ "dpi", ParamType::Int, 36, 600, {}, 150 },
                                             ParamSpec{ "quality", ParamType::Int, 10, 100, {}, 70 },
                                             // A target, as a percentage of the source's size: dpi and
                                             // quality step down until it fits (Reduce size)
                                             ParamSpec{ "percent", ParamType::Int, 0, 99, {}, 0 } } });
            registry.Add(ActionDescriptor{ "ocr.copy_text", L"Copy text", StepKind::Engine, {} });
            registry.Add(ActionDescriptor{ "ocr.searchable_pdf", L"Searchable PDF", StepKind::Engine, {} });
            registry.Add(ActionDescriptor{ "share.files", L"Share", StepKind::Engine, {} });
        }

        const char* BuiltInRecipes()
        {
            // Icons are Segoe Fluent Icons / MDL2 code points: Photo2, Document, Code, PDF, Page,
            // ViewAll, BackToWindow, Font, Search, Share.
            return R"([
  { "id": "pdf-to-jpg", "label": "JPG", "description": "Every page as a JPEG, 300 dpi", "icon": "E91B",
    "steps": [ { "action": "pdf.to_images", "params": { "format": "jpg" } } ] },
  { "id": "pdf-to-png", "label": "PNG", "description": "Every page as a PNG, 300 dpi", "icon": "E91B",
    "steps": [ { "action": "pdf.to_images", "params": { "format": "png" } } ] },
  { "id": "pdf-to-txt", "label": "TXT", "description": "The text; scanned pages read with OCR", "icon": "E8A5",
    "steps": [ { "action": "pdf.to_text", "params": { "format": "txt" } } ] },
  { "id": "pdf-to-md", "label": "Markdown", "description": "Text with headings and lists kept", "icon": "E943",
    "steps": [ { "action": "pdf.to_text", "params": { "format": "md" } } ] },
  { "id": "images-to-pdf", "label": "PDF", "description": "One PDF, a page per image", "icon": "EA90",
    "steps": [ { "action": "pdf.from_images" } ] },

  { "id": "pdf-merge", "label": "Merge", "description": "One PDF, in the order you choose", "ring": "tools", "suffix": "merged", "icon": "E7C3",
    "steps": [ { "action": "pdf.merge" } ] },
  { "id": "pdf-organize", "label": "Organize", "description": "Reorder, turn, copy or remove pages", "ring": "tools", "suffix": "organized", "icon": "E8A9",
    "steps": [ { "action": "pdf.organize" } ] },
  { "id": "pdf-compress", "label": "Compress", "description": "Smaller PDF, pictures at 150 dpi", "ring": "tools", "suffix": "compressed", "icon": "E73F",
    "steps": [ { "action": "pdf.compress", "params": { "dpi": 150, "quality": 70 } } ] },
  { "id": "pdf-compress-max", "label": "Smallest", "description": "Smallest PDF, pictures at 96 dpi", "ring": "tools", "suffix": "compressed", "icon": "E73F",
    "steps": [ { "action": "pdf.compress", "params": { "dpi": 96, "quality": 50 } } ] },
  { "id": "ocr-copy-text", "label": "Copy text", "description": "Read the text and copy it", "ring": "tools", "icon": "E8D2",
    "steps": [ { "action": "ocr.copy_text" } ] },
  { "id": "ocr-searchable-pdf", "label": "Searchable", "description": "PDF with selectable, searchable text", "ring": "tools", "suffix": "searchable", "icon": "E721",
    "steps": [ { "action": "ocr.searchable_pdf" } ] },

  { "id": "reduce-pdf-90", "label": "\u221290%", "description": "90% smaller, as far as it goes", "ring": "reduce", "suffix": "reduced", "steps": [ { "action": "pdf.compress", "params": { "percent": 10 } } ] },
  { "id": "reduce-pdf-80", "label": "\u221280%", "description": "80% smaller, as far as it goes", "ring": "reduce", "suffix": "reduced", "steps": [ { "action": "pdf.compress", "params": { "percent": 20 } } ] },
  { "id": "reduce-pdf-70", "label": "\u221270%", "description": "70% smaller, as far as it goes", "ring": "reduce", "suffix": "reduced", "steps": [ { "action": "pdf.compress", "params": { "percent": 30 } } ] },
  { "id": "reduce-pdf-60", "label": "\u221260%", "description": "60% smaller, as far as it goes", "ring": "reduce", "suffix": "reduced", "steps": [ { "action": "pdf.compress", "params": { "percent": 40 } } ] },
  { "id": "reduce-pdf-50", "label": "\u221250%", "description": "50% smaller, as far as it goes", "ring": "reduce", "suffix": "reduced", "steps": [ { "action": "pdf.compress", "params": { "percent": 50 } } ] },
  { "id": "reduce-pdf-40", "label": "\u221240%", "description": "40% smaller, as far as it goes", "ring": "reduce", "suffix": "reduced", "steps": [ { "action": "pdf.compress", "params": { "percent": 60 } } ] },
  { "id": "reduce-pdf-30", "label": "\u221230%", "description": "30% smaller, as far as it goes", "ring": "reduce", "suffix": "reduced", "steps": [ { "action": "pdf.compress", "params": { "percent": 70 } } ] },
  { "id": "reduce-pdf-20", "label": "\u221220%", "description": "20% smaller, as far as it goes", "ring": "reduce", "suffix": "reduced", "steps": [ { "action": "pdf.compress", "params": { "percent": 80 } } ] },

  { "id": "share", "label": "Share", "description": "Nearby sharing, Phone Link, mail, apps", "ring": "files", "icon": "E72D",
    "steps": [ { "action": "share.files" } ] }
])";
        }

        // One engine for the module: each recipe goes to the area engine that owns its action.
        class ModuleEngine final : public IEngine
        {
        public:
            explicit ModuleEngine(const EngineServices& services)
            {
                m_engines.push_back(MakePdfEngine(services));
                m_engines.push_back(ocr::MakeOcrEngine(services));
                m_engines.push_back(share::MakeShareEngine(services));
            }

            bool Handles(const Recipe& recipe) const override { return For(recipe) != nullptr; }
            bool Applies(const Recipe& r, const std::wstring& path) const override { return For(r)->Applies(r, path); }
            bool Accepts(const Recipe& r, const std::wstring& path, std::wstring* why) const override { return For(r)->Accepts(r, path, why); }
            bool IsNoOp(const Recipe& r, const std::wstring& path) const override { return For(r)->IsNoOp(r, path); }
            JobLane Lane(const Recipe& r) const override { return For(r)->Lane(r); }
            ItemResult Run(const Recipe& r, int64_t runId, const std::wstring& source, std::stop_token stop, const ProgressFn& progress) const override
            {
                return For(r)->Run(r, runId, source, stop, progress);
            }
            bool IsBatch(const Recipe& r) const override { return For(r)->IsBatch(r); }
            ItemResult RunBatch(const Recipe& r, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token stop,
                                const ProgressFn& progress) const override
            {
                return For(r)->RunBatch(r, runId, sources, stop, progress);
            }
            bool NeedsEditor(const Recipe& r) const override { return For(r)->NeedsEditor(r); }
            void OpenEditor(HWND owner, const Recipe& r, const std::vector<std::wstring>& files,
                            std::function<void(Recipe, std::vector<std::wstring>)> done) const override
            {
                For(r)->OpenEditor(owner, r, files, std::move(done));
            }

        private:
            const IEngine* For(const Recipe& recipe) const
            {
                for (const auto& e : m_engines)
                    if (e && e->Handles(recipe))
                        return e.get();
                return nullptr;
            }
            std::vector<std::unique_ptr<IEngine>> m_engines;
        };

        std::unique_ptr<IEngine> MakeEngine(const EngineServices& services) { return std::make_unique<ModuleEngine>(services); }
    }

    FeatureModule Module()
    {
        return FeatureModule{ "pdf", &RegisterActions, &BuiltInRecipes, &MakeEngine };
    }

    bool HasPdfExtension(const std::wstring& path) { return LowerExtension(path) == L".pdf"; }

    bool LooksLikePdf(const std::wstring& path, std::wstring* why)
    {
        if (!IsFile(path))
        {
            if (why)
                *why = L"file not found";
            return false;
        }
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        char head[1024] = {};
        in.read(head, sizeof(head));
        // The header may follow some junk (PDF readers allow up to 1 KB of it).
        const std::string_view text(head, static_cast<size_t>(in.gcount()));
        if (text.find("%PDF-") == std::string_view::npos)
        {
            if (why)
                *why = L"not a PDF file";
            return false;
        }
        return true;
    }

    bool IsReadableImage(const std::wstring& path)
    {
        // The same decoder list the image converters use (WIC at runtime, plus libwebp).
        static const ImageCodecs codecs;
        const auto ext = LowerExtension(path);
        return ext != L".pdf" && codecs.CanDecode(ext);
    }
}
