#pragma once
#include "Engine.h"

#include <memory>

// Text from anything: "Copy text" puts the text of images and PDFs on the clipboard (the PDF's
// own text layer where it has one, Windows OCR where it doesn't); "Searchable PDF" turns images and
// scanned PDFs into PDFs with an invisible, selectable text layer.
namespace sd::ocr
{
    std::unique_ptr<IEngine> MakeOcrEngine(const EngineServices& services);
}
