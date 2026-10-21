#pragma once

// The metadata inspector's window: one per Inspect drop, a tab per file, a search box and a grouped
// two-column list (Field, Value). Read-only: values can be copied (Ctrl+C, double-click, right-click
// menu), never changed. Reading happens on a worker thread; each tab fills in as its file is read.
//
// Standard controls in a DialogWindow (DialogWindow.h), like the history window.
namespace sd::inspect
{
    // Hands the Inspect engine (InspectModule.h) the function that opens the window. UI thread, once.
    void InstallWindow();
}
