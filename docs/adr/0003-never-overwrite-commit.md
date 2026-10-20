# ADR 0003: Outputs are staged, then committed by a no-replace rename

Status: accepted (2026-10-20)

## Context
Outputs go next to the source. Two rules: never overwrite anything, and never leave a half-written
file under a real name if the app crashes, is killed, or the PC loses power.

## Decision
1. Write into a new staging file `~photo.1a2b3c4d.sdpart` in the same folder (created with
   `STGM_FAILIFTHERE`). The history database records the staging path before any byte is written.
2. Commit with `MoveFileExW(staging, "photo (converted).png", MOVEFILE_WRITE_THROUGH)` and no
   `MOVEFILE_REPLACE_EXISTING`. If the name exists, try `(converted 2)`, `(converted 3)`, and so on.
3. At startup, items still marked running are marked interrupted and their staging files deleted.

## Consequences
- A rename within one folder is atomic on NTFS, so a final name only ever holds a complete file.
- The no-replace rename fails instead of overwriting even when another program creates the name
  between our check and our rename.
- Staging files are visible for the length of a conversion. Hiding them was rejected: the hidden
  attribute survives the rename.
