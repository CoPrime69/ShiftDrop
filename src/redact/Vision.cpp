#include "redact/Vision.h"

#include <atomic>

namespace sd::vision
{
    namespace
    {
        std::atomic<const Provider*>& Current()
        {
            static std::atomic<const Provider*> provider{ nullptr };
            return provider;
        }
    }

    void RegisterProvider(const Provider* provider) { Current() = provider; }

    bool CanReadText()
    {
        const auto* p = Current().load();
        return p && p->textAvailable && p->textAvailable();
    }

    bool CanFindFaces()
    {
        const auto* p = Current().load();
        return p && p->facesAvailable && p->facesAvailable();
    }

    std::vector<redact::OcrLine> ReadText(const canvas::Raster& image, std::wstring* error)
    {
        const auto* p = Current().load();
        if (!p || !p->readText)
        {
            if (error)
                *error = L"text recognition is not available";
            return {};
        }
        return p->readText(image, error);
    }

    std::vector<canvas::RectD> FindFaces(const canvas::Raster& image, std::wstring* error)
    {
        const auto* p = Current().load();
        if (!p || !p->findFaces)
        {
            if (error)
                *error = L"face detection is not available";
            return {};
        }
        return p->findFaces(image, error);
    }
}
