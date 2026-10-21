#include "Engine.h"
#include "inspect/InspectModule.h"
#include "imagetools/ImageToolsModule.h"
#include "shelf/ShelfModule.h"
#include "pdf/PdfModule.h"
#include "docs/DocsModule.h"
#include "sheets/SheetsModule.h"
#include "subtitles/SubtitlesModule.h"
#include "officebridge/OfficeModule.h"
#include "mediaeditor/MediaToolsModule.h"
#include "photo/PhotoModule.h"

// The list of feature modules. Each module is its own files (src/<area>/...) exposing one
// FeatureModule; adding a feature is one include and one line here.
namespace sd
{
    const std::vector<FeatureModule>& FeatureModules()
    {
        static const std::vector<FeatureModule> modules = {
            inspect::Module(),   // Tools: Inspect (metadata, M4)
            ShelfModule(),   // Float shelf (file action), src/shelf
            pdf::Module(),   // PDF tools, text from anything (OCR), Share (src/pdf, src/ocr, src/share)
            docs::Module(),        // Documents: DOCX, Markdown, HTML, TXT, RTF, EPUB, PDF (M11, src/docs)
            sheets::Module(),      // Spreadsheets: XLSX, XLS, ODS, CSV, TSV (M11, src/sheets)
            subtitles::Module(),   // Subtitles: SRT, VTT, TXT (M11, src/subtitles)
            office::Module(),      // Office bridge: exact PDFs and slide pictures (M11, src/officebridge)
            MediaToolsModule(),   // video tools (M9) and audio tools (M10)
            ImageToolsModule(),   // Crop, Redact, Share-safe, Remove metadata, src/imagetools
            photo::Module(),   // Edit Photos, Annotate, Background, Collage (M8), src/photo
        };
        return modules;
    }

    void RegisterModuleActions(ActionRegistry& registry)
    {
        for (const auto& m : FeatureModules())
            if (m.registerActions)
                m.registerActions(registry);
    }

    void AppendModuleRecipes(nlohmann::json& builtIns)
    {
        for (const auto& m : FeatureModules())
            if (m.builtInRecipes)
                for (auto& recipe : nlohmann::json::parse(m.builtInRecipes()))
                    builtIns.push_back(std::move(recipe));
    }

    std::vector<std::unique_ptr<IEngine>> MakeModuleEngines(const EngineServices& services)
    {
        std::vector<std::unique_ptr<IEngine>> engines;
        for (const auto& m : FeatureModules())
            if (m.makeEngine)
                if (auto e = m.makeEngine(services))
                    engines.push_back(std::move(e));
        return engines;
    }
}
