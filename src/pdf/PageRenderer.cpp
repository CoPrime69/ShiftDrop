#include "pdf/PageRenderer.h"

#include <algorithm>
#include <cmath>

namespace sd::pdf
{
    RenderHooks& Renderer()
    {
        static RenderHooks hooks;   // WinPdfRenderer.cpp fills it in from a static initialiser
        return hooks;
    }

    bool RenderingAvailable() { return Renderer().open != nullptr; }

    std::unique_ptr<IRenderedPdf> OpenForRendering(const std::wstring& path, std::wstring& error)
    {
        if (!Renderer().open)
        {
            error = L"Windows' PDF renderer is not available in this build";
            return nullptr;
        }
        return Renderer().open(path, error);
    }

    void PixelSize(double widthPt, double heightPt, double dpi, uint32_t maxSide, uint32_t& widthPx, uint32_t& heightPx)
    {
        double w = std::max(1.0, widthPt / 72.0 * dpi), h = std::max(1.0, heightPt / 72.0 * dpi);
        const double over = std::max(w, h) / std::max<uint32_t>(maxSide, 1);
        if (over > 1.0)
            w /= over, h /= over;
        widthPx = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(w)));
        heightPx = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(h)));
    }
}
