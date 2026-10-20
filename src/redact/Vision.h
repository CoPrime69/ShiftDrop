#pragma once
#include "canvas/Raster.h"
#include "redact/Pii.h"

#include <string>
#include <vector>

// Windows' built-in text recognition (Windows.Media.Ocr) and face detection
// (Windows.Media.FaceAnalysis), behind a plain C++ interface. Both ship with Windows: nothing is
// bundled or downloaded. The WinRT implementation (WinRtVision.cpp) is its own library and
// registers itself when it is linked in (the app, and the tests on Windows); without it every
// call says it is unavailable.
namespace sd::vision
{
    struct Provider
    {
        bool (*textAvailable)();   // an OCR language is installed
        bool (*facesAvailable)();
        // Blocking; call on a worker thread (MTA). Boxes are in the raster's pixels.
        std::vector<redact::OcrLine> (*readText)(const canvas::Raster& image, std::wstring* error);
        std::vector<canvas::RectD> (*findFaces)(const canvas::Raster& image, std::wstring* error);
    };

    void RegisterProvider(const Provider* provider);

    bool CanReadText();
    bool CanFindFaces();
    std::vector<redact::OcrLine> ReadText(const canvas::Raster& image, std::wstring* error);
    std::vector<canvas::RectD> FindFaces(const canvas::Raster& image, std::wstring* error);
}
