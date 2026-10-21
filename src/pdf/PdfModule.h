#pragma once
#include "Engine.h"

#include <memory>
#include <stop_token>
#include <string>

// The PDF module: the PDF engine (src/pdf), text from anything (src/ocr) and the Share file
// action (src/share), registered with the core as one FeatureModule (Modules.cpp).
namespace sd::pdf
{
    FeatureModule Module();

    // Each area's engine; Module()'s engine hands a recipe to the one whose actions it uses.
    std::unique_ptr<IEngine> MakePdfEngine(const EngineServices& services);

    // ---- Shared by the three engines -----------------------------------------------------------

    bool HasPdfExtension(const std::wstring& path);
    // ".pdf" and "%PDF-" near the start (cheap: reads 1 KB). Used by Accepts, so the wheel can say
    // "not a PDF" for a renamed file.
    bool LooksLikePdf(const std::wstring& path, std::wstring* why);
    // An image the app can read (the same decoders the image converters use), and not a PDF.
    bool IsReadableImage(const std::wstring& path);

    // Records a failed (or skipped) item for a file that never got a staging output.
    ItemResult FailItem(HistoryStore* history, int64_t runId, const std::wstring& source, std::wstring error, bool skipped = false);
    // Records a finished item that wrote no file (Copy text: the result is on the clipboard).
    void RecordNoFileItem(HistoryStore* history, int64_t runId, const std::wstring& source);

    std::wstring FileNameOf(const std::wstring& path);
    std::wstring SizeText(uint64_t bytes);   // "840 KB", "4.2 MB"
}
