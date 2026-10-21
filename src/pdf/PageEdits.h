#pragma once
#include "canvas/CanvasTool.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

// Editing single pages in the page view (Merge PDFs > More..., Edit PDF): a page is rendered to a
// picture, opened in one of the existing picture editors (Redact, Annotate, Crop, Edit), and the result
// becomes a one-page PDF that the page plan points at (PageRef::edited) in place of the original.
// The picture holds only pixels, so a redacted area is gone from the output, not covered over, and
// the original page's text, links and form fields are not in it. Nothing touches the source files;
// the pictures and pages live in a folder under %TEMP% that goes once the export has read it, or
// when the page view is cancelled.
namespace sd::pdf
{
    enum class PageTool
    {
        Redact,     // Redact Photos: covers, with Find text, Find faces and Find personal info
        Annotate,   // Annotate Photos: arrows, text, shapes, pen and highlighter
        Crop,       // Crop Images: trim, turn or straighten a scanned page
        Adjust,     // Edit Photos: light and colour, for scans and photographed pages
    };

    struct PageToolInfo
    {
        PageTool tool;
        const wchar_t* label;   // for the Edit menu
    };
    const std::vector<PageToolInfo>& PageTools();

    // Pages are edited as pictures at this resolution (lower for very large pages).
    inline constexpr double kEditDpi = 200;
    inline constexpr uint32_t kEditMaxSide = 4800;

    // A new, empty folder for one page view's edited pages, or empty when %TEMP% can't be written.
    std::wstring NewEditFolder();
    void RemoveEditFolder(const std::wstring& folder);
    // Removes folders more than a day old (left behind when the app stopped with a page view open).
    void RemoveOldEditFolders();

    // The editor for `pictures` (UI thread; the window that shows it lives in the app).
    std::unique_ptr<canvas::ICanvasTool> MakePageTool(PageTool tool, const std::vector<std::wstring>& pictures);

    // The tool's result for one of the pictures it edited, written to `out` as PNG. `changed` is false
    // (and nothing is written) when the tool left this picture as it was. Any thread with COM.
    bool RenderPageEdit(PageTool tool, const nlohmann::json& params, const std::wstring& picture, const std::wstring& out, bool& changed,
                        std::wstring& error);

    // `picture` as a one-page PDF, each pixel `pointsPerPixel` points (the page's own scale, so an
    // uncropped page keeps its size): what the plan's edited page points at.
    bool WritePicturePage(const std::wstring& picture, double pointsPerPixel, const std::wstring& out, std::wstring& error);

    // The picture kept beside an edited page, to edit it again: "x.pdf" -> "x.png".
    std::wstring PictureOf(const std::wstring& editedPage);
}
