#pragma once
#include "HistoryStore.h"

#include <windows.h>

#include <string>
#include <vector>

namespace sd
{
    struct UndoReport
    {
        bool ok = false;
        int recycled = 0;     // outputs sent to the Recycle Bin
        int movedBack = 0;    // items a Move run put back where they were
        int alreadyGone = 0;
        std::vector<std::wstring> modified;   // outputs changed since we wrote them: undo refused
        std::vector<std::wstring> occupied;   // Move undo: something new sits at an original location: refused
        std::wstring message;                 // one line for the user
    };

    // Undoes one history run (ADR 0005). All-or-nothing, and outputs already deleted by the user are skipped.
    //  - Conversions and Duplicate: outputs go to the Recycle Bin, refused if any output was modified
    //    since ShiftDrop wrote it (SHA-256 differs).
    //  - Move: each item moves back to where it was, refused if that place is taken again. Nothing is
    //    deleted, so an item edited after the move is fine.
    // Call on an STA thread (the UI thread): IFileOperation may show shell UI.
    UndoReport UndoRun(HistoryStore& history, const RunRecord& run, HWND owner);
}
