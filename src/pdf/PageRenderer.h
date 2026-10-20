#pragma once
#include "pdf/Bitmap.h"

#include <cstdint>
#include <memory>
#include <string>

// Page rendering through Windows' own PDF renderer, Windows.Data.Pdf (part of Windows, nothing
// bundled). The WinRT code is in WinPdfRenderer.cpp, built only by MSVC; it installs itself here at
// startup. Without it (MinGW tests) rendering reports that it is unavailable.
namespace sd::pdf
{
    class IRenderedPdf
    {
    public:
        virtual ~IRenderedPdf() = default;
        virtual uint32_t PageCount() const = 0;
        // The page as displayed (crop box, /Rotate applied) in points.
        virtual bool PageSize(uint32_t page, double& widthPt, double& heightPt) const = 0;
        // Renders the page to exactly widthPx x heightPx, opaque on white. Blocks: call on a worker
        // (MTA) thread, never the UI thread.
        virtual bool Render(uint32_t page, uint32_t widthPx, uint32_t heightPx, Bitmap& out, std::wstring& error) const = 0;
    };

    struct RenderHooks
    {
        // Opens a PDF for rendering; null with `error` set on failure. Blocks; worker thread only.
        std::unique_ptr<IRenderedPdf> (*open)(const std::wstring& path, std::wstring& error) = nullptr;
    };
    RenderHooks& Renderer();

    std::unique_ptr<IRenderedPdf> OpenForRendering(const std::wstring& path, std::wstring& error);
    bool RenderingAvailable();

    // Pixel size for a page of w x h points at `dpi`, scaled down so neither side exceeds `maxSide`.
    void PixelSize(double widthPt, double heightPt, double dpi, uint32_t maxSide, uint32_t& widthPx, uint32_t& heightPx);
}
