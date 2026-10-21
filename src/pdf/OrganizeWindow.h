#pragma once
#include "pdf/PagePlan.h"

#include <windows.h>
#include <d2d1.h>

#include <functional>
#include <string>
#include <vector>

// The page view (OrganizeWindow.cpp): every page of one or more PDFs as a thumbnail grid, rendered by
// Windows.Data.Pdf on a worker with the pages on screen first. Drag to reorder (across files too),
// rotate, duplicate, leave out, undo. Organize PDF Pages opens it for one PDF. Merge PDFs (More...)
// and Edit PDF open it with every file, a large preview of the selected page and per-page Edit
// (PageEdits.h). UI code, in the app only.
namespace sd::pdf
{
    enum class PageViewKind
    {
        Organize,   // one PDF; Save writes "name (organized).pdf"
        Merge,      // Merge PDFs > More...: "Merge and export"
        Edit,       // Edit PDF: "Save" for one PDF, "Merge and export" for several
    };

    struct PageViewRequest
    {
        PageViewKind kind = PageViewKind::Organize;
        std::vector<std::wstring> files;   // the plan's file indices point here
        std::vector<uint32_t> order;       // the files shown, in list order; empty: all of them
        PagePlan plan;                     // empty: every page of the files shown
        bool changed = false;              // the plan already holds the user's changes (Cancel asks)
        std::wstring editFolder;           // where edited pages are kept (PageEdits.h); made when needed
        std::wstring notice;               // a line shown over the pages until the next change
        // Save / Merge and export: the plan, with file indices into `files`.
        std::function<void(PagePlan plan, std::wstring editFolder)> save;
        // Back to Merge's file list, keeping the page edits; empty: no Back button.
        std::function<void(PagePlan plan, std::wstring editFolder)> back;
        // Closed without saving or going back (the edited pages are already removed).
        std::function<void()> cancelled;
    };

    // Opens the page view; false (and nothing called) when its window could not be made.
    bool OpenPageView(PageViewRequest request);

    // "Close without saving?" for a window whose page changes would be lost; true to close.
    bool ConfirmDiscard(HWND window, const std::wstring& title);

    // The colour and letter (A, B, ... AA) each file's pages carry in the page view and its row in
    // Merge's list, by the file's index in the drop.
    D2D1_COLOR_F FileTagColor(uint32_t file);
    std::wstring FileTagLetter(uint32_t file);
}
