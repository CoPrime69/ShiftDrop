#include "DropDescription.h"

#include <shlguid.h>   // CLSID_DragDropHelper
#include <wil/resource.h>

#include <vector>

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

    Microsoft::WRL::ComPtr<IDropTargetHelper> MakeDragImageHelper()
    {
        Microsoft::WRL::ComPtr<IDropTargetHelper> helper;
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper));
        return helper;
    }

    HWND DragImageWindow(IDataObject* data)
    {
        if (!data)
            return nullptr;
        static const auto format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"DragWindow"));
        FORMATETC fmt{ format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medium{};
        if (FAILED(data->GetData(&fmt, &medium)))
            return nullptr;
        auto release = wil::scope_exit([&] { ReleaseStgMedium(&medium); });
        if (medium.tymed != TYMED_HGLOBAL || GlobalSize(medium.hGlobal) < sizeof(DWORD))
            return nullptr;
        const auto* value = static_cast<const DWORD*>(GlobalLock(medium.hGlobal));
        if (!value)
            return nullptr;
        // Window handles fit in 32 bits, also in a 64-bit process, so a 32-bit source can share one.
        const auto hwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(*value));
        GlobalUnlock(medium.hGlobal);
        return IsWindow(hwnd) ? hwnd : nullptr;
    }

    std::optional<RECT> DragImageRect(IDataObject* data, POINT cursor)
    {
        if (!data)
            return std::nullopt;
        constexpr LONG kMaxPx = 4096;
        const auto sane = [](const RECT& r) {
            return r.right > r.left && r.bottom > r.top && r.right - r.left <= kMaxPx && r.bottom - r.top <= kMaxPx &&
                   r.left <= 0 && r.top <= 0 && r.right >= 0 && r.bottom >= 0;   // the cursor is on the picture
        };
        static const auto format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"DragImageBits"));
        FORMATETC fmt{ format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medium{};
        if (SUCCEEDED(data->GetData(&fmt, &medium)))
        {
            auto release = wil::scope_exit([&] { ReleaseStgMedium(&medium); });
            // Only the first two fields are read (SIZE sizeDragImage, POINT ptOffset): they come before
            // the bitmap handle, so a 32-bit source lays them out the same way.
            if (medium.tymed == TYMED_HGLOBAL && GlobalSize(medium.hGlobal) >= 4 * sizeof(LONG))
                if (const auto* v = static_cast<const LONG*>(GlobalLock(medium.hGlobal)))
                {
                    const RECT r{ -v[2], -v[3], v[0] - v[2], v[1] - v[3] };
                    GlobalUnlock(medium.hGlobal);
                    if (sane(r))
                        return r;
                }
        }
        if (const HWND window = DragImageWindow(data); window && IsWindowVisible(window))
        {
            RECT r{};
            if (GetWindowRect(window, &r))
            {
                OffsetRect(&r, -cursor.x, -cursor.y);
                if (sane(r))
                    return r;
            }
        }
        return std::nullopt;
    }

    int KeepDragImageOnTop(HWND dragWindow)
    {
        if (!dragWindow || !IsWindow(dragWindow))
            return 0;
        // From the top of the z-order down to the picture (a few topmost windows at most).
        const DWORD thread = GetCurrentThreadId();
        std::vector<HWND> above;
        int steps = 0;
        HWND h = GetTopWindow(nullptr);
        for (; h && h != dragWindow && steps < 512; h = GetWindow(h, GW_HWNDNEXT), ++steps)
            if (GetWindowThreadProcessId(h, nullptr) == thread && IsWindowVisible(h))
                above.push_back(h);
        if (h != dragWindow)
            return 0;   // not found from the top: leave everything as it is
        // Each goes just below the previous one, so ours keep their order among themselves. Inserting
        // after a topmost window keeps them topmost.
        HWND after = dragWindow;
        for (HWND w : above)
        {
            SetWindowPos(w, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            after = w;
        }
        return static_cast<int>(above.size());
    }
}
