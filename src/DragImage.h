#pragma once
#include "Theme.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <objidl.h>
#include <shlobj.h>   // DROPIMAGETYPE

#include <string>

namespace sd
{
    // Explorer's drag picture (the thumbnail that follows the pointer, with the file's name under it
    // and a "+ Copy" style badge) is shown by the shell's drag-image manager from data stored in the
    // drag's data object. A drop target may adjust both while the pointer is over it.

    // The badge: DROPIMAGE_NOIMAGE with no text shows nothing; DROPIMAGE_INVALID hands it back to the
    // default. Documented: drop targets set CFSTR_DROPDESCRIPTION on the data object.
    void SetDropDescription(IDataObject* data, DROPIMAGETYPE type, const wchar_t* message = L"");

    // Swaps the picture for `thumb` with `name` under it on one line ending in "…" (empty name: the
    // picture alone). IDragSourceHelper::InitializeFromBitmap stores the new picture in the data object
    // (documented); DDWM_UPDATEWINDOW then tells the shell's drag window to redraw it, a long-standing
    // but undocumented message. On any failure the original picture stays and this returns false.
    bool ReplaceDragImage(IDataObject* data, const theme::Pixels& thumb, const std::wstring& name, float scale,
                          ID2D1Factory* d2d, IDWriteFactory* dwrite, const std::wstring& fontFamily);
}
