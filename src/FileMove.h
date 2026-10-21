#pragma once
#include "HistoryStore.h"
#include "Recipe.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sd
{
    // "Move to…" on the File-actions wheel: pick a folder, then move the dropped files and folders there.

    // Shows the folder picker. Runs on the UI thread (the dialog is modal and keeps pumping messages, so
    // the mouse hook stays responsive). Returns false when the user cancels.
    // `verb` names what happens there ("Move 2 items to", "Move here"; "Save" for dropped files).
    bool PickDestinationFolder(HWND owner, size_t itemCount, std::wstring& folder, std::wstring_view verb = L"Move");

    struct MoveOutcome
    {
        std::wstring source;
        std::wstring destination;   // final path; the shell renames to "name (2)" if the name is taken
        std::wstring error;
        bool ok = false;
        bool skipped = false;        // already in that folder
    };

    // Moves with IFileOperation, so it works across drives and for folders. Never overwrites: a name
    // collision gets a new name. Records a history run (runId, 0 without history) so the move can be
    // undone from History. Call on an STA thread: IFileOperation is not supported in the MTA.
    std::vector<MoveOutcome> MoveToFolder(const Recipe& recipe, const std::vector<std::wstring>& sources,
                                          const std::wstring& folder, HistoryStore* history, int64_t& runId);

    // True when a history run was a move (its undo moves items back instead of recycling them).
    bool IsMoveRun(const RunRecord& run);

    // "Rename…" on the File-actions wheel (rename/RenameWindow.h asks for the names, from
    // rename/RenamePattern.h). Each item gets its new name in its own folder, through IFileOperation
    // like Move, with FOF_RENAMEONCOLLISION: the names were checked first, and a name taken in the
    // meantime gets "(2)" rather than replacing anything. Recorded as one history run, which Undo
    // puts back like a move. STA thread only.
    struct Rename
    {
        std::wstring from;   // full path
        std::wstring to;     // full path, same folder
    };
    std::vector<MoveOutcome> RenameItems(const Recipe& recipe, const std::vector<Rename>& renames, HistoryStore* history, int64_t& runId);
    bool IsRenameRun(const RunRecord& run);
}
