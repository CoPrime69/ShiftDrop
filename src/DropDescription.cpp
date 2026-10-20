#include "DropDescription.h"

#include <wil/resource.h>

namespace sd
{
    void SetDropDescription(IDataObject* data, DROPIMAGETYPE type, const wchar_t* message)
    {
        if (!data)
            return;
        static const auto format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_DROPDESCRIPTION));
        wil::unique_hglobal memory(GlobalAlloc(GHND, sizeof(DROPDESCRIPTION)));
        if (!memory)
            return;
        auto* description = static_cast<DROPDESCRIPTION*>(GlobalLock(memory.get()));
        if (!description)
            return;
        description->type = type;
        wcsncpy_s(description->szMessage, message, _TRUNCATE);
        GlobalUnlock(memory.get());
        FORMATETC fmt{ format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medium{};
        medium.tymed = TYMED_HGLOBAL;
        medium.hGlobal = memory.get();
        if (SUCCEEDED(data->SetData(&fmt, &medium, TRUE)))   // fRelease: the data object owns it now
            memory.release();
    }
}
