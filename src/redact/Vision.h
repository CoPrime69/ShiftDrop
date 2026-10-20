#pragma once
#include "canvas/Raster.h"
#include "redact/Pii.h"

#include <string>
#include <vector>

// What Redact Photos and Share-safe find in a picture with Windows' own engines: text through the
// OCR module (src/ocr, Windows.Media.Ocr) and faces through Windows.Media.FaceAnalysis. Both ship
// with Windows: nothing is bundled or downloaded. The face detector (WinFaces.cpp) is in the WinRT
// object library and plugs itself in when it is linked (the app, and the tests on Windows); without
// it, or without an OCR language, the calls say so.
namespace sd::vision
{
    struct FaceProvider
    {
        bool (*available)();
        // Blocking; call on a worker thread (MTA). Boxes are in the raster's pixels.
        std::vector<canvas::RectD> (*find)(const canvas::Raster& image, std::wstring* error);
    };
    void RegisterFaceProvider(const FaceProvider* provider);

    bool CanReadText();
    bool CanFindFaces();
    // Blocking, on a worker thread. Boxes in the raster's pixels.
    std::vector<redact::OcrLine> ReadText(const canvas::Raster& image, std::wstring* error);
    std::vector<canvas::RectD> FindFaces(const canvas::Raster& image, std::wstring* error);
}
