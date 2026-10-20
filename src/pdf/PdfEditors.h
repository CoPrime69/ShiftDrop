#pragma once
#include "pdf/PagePlan.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

// The two PDF editor windows (MergeWindow.cpp, OrganizeWindow.cpp) are UI code built into the app
// only; they install themselves here at startup. The engine opens them through these hooks and
// falls back to "no changes" where they are absent (unit tests).
namespace sd::pdf
{
    struct EditorHooks
    {
        // Merge PDFs: the dropped files in Explorer's order, drag to reorder, OK. `done` gets the
        // files in the chosen order; it is not called when the window is cancelled.
        void (*merge)(HWND owner, const std::vector<std::wstring>& files, std::function<void(std::vector<std::wstring>)> done) = nullptr;
        // Organize PDF Pages: a grid of page thumbnails. `done` gets the page plan to save.
        void (*organize)(HWND owner, const std::vector<std::wstring>& files, std::function<void(PagePlan)> done) = nullptr;
    };
    EditorHooks& Editors();
}
