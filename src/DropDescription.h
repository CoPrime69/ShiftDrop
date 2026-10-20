#pragma once
#include <windows.h>
#include <objidl.h>
#include <shlobj.h>   // DROPIMAGETYPE

namespace sd
{
    // The badge under Explorer's drag picture ("+ Copy", "→ Move to Documents"). A drop target sets it
    // by storing CFSTR_DROPDESCRIPTION in the drag's data object: DROPIMAGE_NOIMAGE with no text shows
    // no badge, DROPIMAGE_INVALID hands it back to the default.
    void SetDropDescription(IDataObject* data, DROPIMAGETYPE type, const wchar_t* message = L"");
}
