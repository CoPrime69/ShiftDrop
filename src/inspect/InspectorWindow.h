#pragma once

// The metadata inspector's window: one per Inspect drop, a tab per file, a search box and a grouped
// two-column list (Field, Value). Read-only: values can be copied (Ctrl+C, double-click, right-click
// menu), never changed. Reading happens on a worker thread; each tab fills in as its file is read.
//
// A plain top-level window laid out in pixels from its DPI, like the history window; WM_DPICHANGED
// rebuilds the font and the layout.
namespace sd::inspect
{
    // Hands the Inspect engine (InspectModule.h) the function that opens the window. UI thread, once.
    void InstallWindow();
}
