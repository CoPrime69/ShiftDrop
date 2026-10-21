# ADR 0002: Detect "file drag + key" with a probe overlay

Status: accepted (2026-10-20)

## Context
On macOS an app can read the system drag pasteboard to learn that files are being dragged. Windows
has no such global API: only a drop target under the cursor receives the drag data (`IDataObject`).

## Decision
`WH_MOUSE_LL` notices "primary button held, moved past `SM_CXDRAG`, wheel key down". We then show an
almost invisible (about 2% alpha) but hit-testable layered window under the cursor that is itself an
OLE drop target. Only when OLE delivers `DragEnter` with `CF_HDROP` do we draw the wheel. The drop
effect is always `DROPEFFECT_COPY`, never `MOVE`.

## Consequences
- Text selections and window moves with Shift held never flash a menu.
- Virtual-file drags (browser images, Outlook attachments) carry no `CF_HDROP`. They were handled
  later by reading their file descriptors instead (`src/VirtualDrop.h`).
- Explorer treats Shift-drag as "move"; answering COPY means the source never deletes the original.
- An elevated source cannot drop on us (UIPI); documented, not worked around.
