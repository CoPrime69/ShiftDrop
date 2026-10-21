# ADR 0005: History in SQLite; undo recycles, and refuses if an output changed

Status: accepted (2026-10-20)

## Context
Every run should be undoable, but undo must never destroy work: if the user edited an output after
ShiftDrop wrote it, deleting that file would lose their edit.

## Decision
- `%LOCALAPPDATA%\ShiftDrop\history.db` (SQLite, WAL) stores runs and items: source, staging,
  output, size, SHA-256 of the output, status, error, duration, and the recipe JSON.
- Undo of a conversion or Duplicate re-hashes every output first. If any differs, nothing is undone.
  Otherwise all outputs go to the Recycle Bin through `IFileOperation` with `FOFX_RECYCLEONDELETE`,
  never a permanent delete.
- Undo of Move to… moves each item back. It is refused if something new now sits at an original
  location. Nothing is deleted, so no hash check is needed.
- Outputs the user already deleted are skipped and reported.

## Consequences
- Undo is all-or-nothing per run and can always be undone again from the Recycle Bin.
- Undo re-hashes outputs on the UI thread: quick for pictures, a moment for a large video.
- If the database cannot be opened, conversions still work, without history or undo.
