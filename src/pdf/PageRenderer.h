#pragma once
#include "pdf/Bitmap.h"
#include "pdf/PagePlan.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

    // ---- The page view's pictures (OrganizeWindow.cpp) ------------------------------------------

    // Where a plan item's pixels come from, and the key the page view keeps its pictures under: the
    // edited page's one-page PDF (its page 0) when it has one, else its page of the source file.
    // Every edit writes a file of its own, so an edit, a second edit and a revert (back to the
    // source's key) each show their own render, never an earlier one.
    struct PageKey
    {
        std::wstring path;
        uint32_t page = 0;
        auto operator<=>(const PageKey&) const = default;
    };
    PageKey KeyOf(const PageRef& ref, const std::vector<std::wstring>& files);

    // Device pixels along the longest side of the large preview: the page (width / height `aspect`
    // as it is in its file, turned by `rotation`) fitted in `boxW` x `boxH` DIPs at `scale` device
    // pixels per DIP, so the render is drawn one pixel per screen pixel at every DPI.
    uint32_t PreviewSide(double aspect, int rotation, double boxW, double boxH, double scale);

    // A page to render, fitted in `maxSide` pixels.
    struct RenderJob
    {
        PageKey key;
        uint32_t maxSide = 0;
        bool preview = false;
    };

    struct RenderedPage
    {
        RenderJob job;
        Bitmap pixels;   // empty when it failed
        double widthPt = 0, heightPt = 0;
    };

    // One worker that renders what the page view asks for, the preview before any thumbnail, and
    // hands each page to the target. The window never waits for it; the worker sleeps while there
    // is nothing to do and wakes for every new request.
    class RenderQueue
    {
    public:
        // Gets each render on the worker; false when nobody can take it any more (the window is
        // gone), which ends the worker.
        using Target = std::function<bool(std::unique_ptr<RenderedPage>)>;
        using Opener = std::unique_ptr<IRenderedPdf> (*)(const std::wstring& path, std::wstring& error);

        // `open` opens each PDF once (OpenForRendering unless a test passes its own).
        explicit RenderQueue(Opener open = nullptr);
        ~RenderQueue();
        RenderQueue(const RenderQueue&) = delete;
        RenderQueue& operator=(const RenderQueue&) = delete;

        // Where the renders go; nothing is rendered before it is set.
        void SetTarget(Target target);
        // The thumbnails still needed, most wanted first; replaces the previous list.
        void Thumbnails(std::vector<RenderJob> wanted);
        // The preview to render next; replaces one not started yet.
        void Preview(RenderJob job);
        // Runs once the worker has let go of every file it opened (at once if it never started):
        // removing a folder of edited pages.
        void WhenDone(std::function<void()> fn);

    private:
        struct State;
        static void Work(const std::shared_ptr<State>& state);
        void Wake();

        std::shared_ptr<State> m_state;
    };
}
