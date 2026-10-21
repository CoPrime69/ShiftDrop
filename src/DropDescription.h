#pragma once
#include <windows.h>
#include <objidl.h>
#include <shlobj.h>   // DROPIMAGETYPE, IDropTargetHelper
#include <wrl/client.h>

// The drag picture: the item's icon or thumbnail and its name that the drag source (Explorer, a
// browser, a mail app) shows under the cursor, and the badge under it.
namespace sd
{
    // The badge under Explorer's drag picture ("+ Copy", "→ Move to Documents"). A drop target sets it
    // by storing CFSTR_DROPDESCRIPTION in the drag's data object: DROPIMAGE_NOIMAGE with no text shows
    // no badge, DROPIMAGE_INVALID hands it back to the default.
    void SetDropDescription(IDataObject* data, DROPIMAGETYPE type, const wchar_t* message = L"");

    // The shell's drag-image helper (CLSID_DragDropHelper), or null if it can't be created. A drop
    // target that hands it DragEnter, DragOver, DragLeave and Drop keeps the source's drag picture
    // showing over its window; over a window that doesn't, the shell fades the picture away.
    Microsoft::WRL::ComPtr<IDropTargetHelper> MakeDragImageHelper();

    // The window the drag picture is drawn in: the shell's drag-source helper puts its handle in the
    // data object ("DragWindow", 32 bits). Null when there is none (the source draws no picture).
    HWND DragImageWindow(IDataObject* data);

    // The picture is a topmost window of the drag source's. Our topmost windows that appear after the
    // drag started (the wheel, the top panel, the predicted-output card) would go above it and hide
    // it, so each visible one of this thread's that is above it moves just below it. Returns how many
    // moved. Only our own windows are touched.
    int KeepDragImageOnTop(HWND dragWindow);
}
