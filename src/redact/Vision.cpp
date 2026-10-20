#include "redact/Vision.h"
#include "ocr/Ocr.h"

#include <atomic>

namespace sd::vision
{
    namespace
    {
        std::atomic<const FaceProvider*>& Faces()
        {
            static std::atomic<const FaceProvider*> provider{ nullptr };
            return provider;
        }
    }

    void RegisterFaceProvider(const FaceProvider* provider) { Faces() = provider; }

    bool CanReadText() { return ocr::Available(nullptr); }

    bool CanFindFaces()
    {
        const auto* p = Faces().load();
        return p && p->available && p->available();
    }

    std::vector<redact::OcrLine> ReadText(const canvas::Raster& image, std::wstring* error)
    {
        std::vector<redact::OcrLine> lines;
        pdf::Bitmap bitmap;
        bitmap.width = image.width;
        bitmap.height = image.height;
        bitmap.bgra = image.px;
        pdf::FlattenOnWhite(bitmap);   // OCR reads dark text on a light page
        ocr::Result result;
        std::wstring why;
        if (!ocr::Recognize(bitmap, result, why))
        {
            if (error)
                *error = why.empty() ? std::wstring(L"text recognition is not available") : why;
            return lines;
        }
        for (const auto& line : result.lines)
        {
            redact::OcrLine out;
            for (const auto& w : line.words)
                out.words.push_back({ w.text, { w.x, w.y, w.w, w.h } });
            if (!out.words.empty())
                lines.push_back(std::move(out));
        }
        return lines;
    }

    std::vector<canvas::RectD> FindFaces(const canvas::Raster& image, std::wstring* error)
    {
        const auto* p = Faces().load();
        if (!p || !p->find)
        {
            if (error)
                *error = L"face detection is not available";
            return {};
        }
        return p->find(image, error);
    }
}
