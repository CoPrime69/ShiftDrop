#pragma once
#include "pdf/PagePlan.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

// The PDF editor windows (MergeWindow.cpp, OrganizeWindow.cpp) are UI code built into the app
// only; they install themselves here at startup. The engine opens them through these hooks and
// falls back to "no changes" where they are absent (unit tests).
namespace sd::pdf
{
    struct EditorHooks
    {
        // Merge PDFs and Edit PDF: the dropped files as a list (drag to reorder) and every page of them
        // in a page view (reorder, turn, remove and edit pages, with a large preview). Merge starts
        // with the list, whose More... opens the page view; Edit (`pageView`) starts with the pages.
        // `done` gets the files in list order and, once the page view was used, the page plan over
        // them (file indices in that order); an empty plan means every page as it is. It is not
        // called when the window is cancelled. `names`: what the window calls each file (Edit on a
        // document opens its laid-out pages under the document's name); missing ones go by the file.
        void (*merge)(HWND owner, const std::vector<std::wstring>& files, const std::vector<std::wstring>& names, bool pageView,
                      std::function<void(std::vector<std::wstring>, PagePlan)> done) = nullptr;
        // Organize PDF Pages: a grid of page thumbnails. `done` gets the page plan to save.
        void (*organize)(HWND owner, const std::vector<std::wstring>& files, std::function<void(PagePlan)> done) = nullptr;
    };
    EditorHooks& Editors();
}
