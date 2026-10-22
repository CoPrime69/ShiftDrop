#include "Engine.h"
#include "archive/ArchiveModule.h"
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

#include <algorithm>

// The list of feature modules. Each module is its own files (src/<area>/...) exposing one
// FeatureModule; adding a feature is one include and one line here.
namespace sd
{
    const std::vector<FeatureModule>& FeatureModules()
    {
        static const std::vector<FeatureModule> modules = {
            inspect::Module(),   // Tools: Inspect (metadata)
            ShelfModule(),   // Float shelf (file action), src/shelf
            pdf::Module(),   // PDF tools, text from anything (OCR), Share (src/pdf, src/ocr, src/share)
            docs::Module(),        // Documents: DOCX, Markdown, HTML, TXT, RTF, EPUB, PDF (src/docs)
            sheets::Module(),      // Spreadsheets: XLSX, XLS, ODS, CSV, TSV (src/sheets)
            subtitles::Module(),   // Subtitles: SRT, VTT, TXT (src/subtitles)
            office::Module(),      // Office bridge: exact PDFs and slide pictures (src/officebridge)
            MediaToolsModule(),   // video tools and audio tools (src/mediaeditor)
            ImageToolsModule(),   // Crop, Redact, Share-safe, Remove metadata, src/imagetools
            photo::Module(),   // Edit Photos, Annotate, Background, Collage (src/photo)
            archive::Module(),   // Zip, Extract, convert archives; parsed in the sandboxed worker (src/archive, src/sandbox)
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
                {
                    // "after": a slice with a long label goes right after the one named, so on a full
                    // wheel it lands where its two lines fit (the end of the list is the outer ring).
                    auto at = builtIns.end();
                    if (const auto after = recipe.find("after"); after != recipe.end())
                    {
                        const auto id = after->get<std::string>();
                        at = std::find_if(builtIns.begin(), builtIns.end(), [&](const nlohmann::json& r) { return r.value("id", "") == id; });
                        if (at != builtIns.end())
                            ++at;
                        recipe.erase("after");
                    }
                    builtIns.insert(at, std::move(recipe));
                }
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
