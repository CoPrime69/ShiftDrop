#include "DropTarget.h"
#include "Log.h"

#include <shellapi.h>
#include <shlguid.h>   // CLSID_DragDropHelper
#include <wil/resource.h>

namespace sd
{
    DWORD SafeEffect(DWORD allowed)
    {
        if (allowed & DROPEFFECT_COPY)
            return DROPEFFECT_COPY;
        if (allowed & DROPEFFECT_LINK)
            return DROPEFFECT_LINK;
        return DROPEFFECT_NONE;   // never MOVE
    }

    std::vector<std::wstring> ReadDroppedFiles(IDataObject* data)
    {
        std::vector<std::wstring> files;
        FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        if (!data || data->QueryGetData(&fmt) != S_OK)
            return files;

        STGMEDIUM medium{};
        if (FAILED(data->GetData(&fmt, &medium)))
            return files;
        auto releaseMedium = wil::scope_exit([&] { ReleaseStgMedium(&medium); });

        // The HGLOBAL holds a DROPFILES block; GlobalLock gives us the HDROP DragQueryFileW expects.
        auto* locked = GlobalLock(medium.hGlobal);
        if (!locked)
            return files;
        auto unlock = wil::scope_exit([&] { GlobalUnlock(medium.hGlobal); });

        const auto drop = static_cast<HDROP>(locked);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        files.reserve(count);
        for (UINT i = 0; i < count; ++i)
        {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);   // length without the terminator
            std::wstring path(len, L'\0');
            DragQueryFileW(drop, i, path.data(), len + 1);
            files.push_back(std::move(path));
        }
        return files;
    }

    DropTarget::DropTarget(HWND hwnd, IDropSink* sink) : m_hwnd(hwnd), m_sink(sink)
    {
        // Optional: without the helper the thumbnail simply stays where the source draws it.
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_helper));
    }

    STDMETHODIMP DropTarget::DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect)
    {
        // Reading CF_HDROP here (not only at Drop) lets the wheel show only the actions these files support.
        const auto files = ReadDroppedFiles(data);
        POINT p{ pt.x, pt.y };   // IDropTargetHelper takes a non-const POINT*
        m_accepting = m_sink->OnDragEnter(files, p);
        *effect = m_accepting ? m_sink->OnDragOver(p, *effect) : DROPEFFECT_NONE;
        if (m_helper)
        {
            m_helper->DragEnter(m_hwnd, data, &p, *effect);
            if (m_accepting)
                m_helper->Show(FALSE);
        }
        return S_OK;
    }

    STDMETHODIMP DropTarget::DragOver(DWORD, POINTL pt, DWORD* effect)
    {
        POINT p{ pt.x, pt.y };   // IDropTargetHelper takes a non-const POINT*
        *effect = m_accepting ? m_sink->OnDragOver(p, *effect) : DROPEFFECT_NONE;
        if (m_helper)
            m_helper->DragOver(&p, *effect);
        return S_OK;
    }

    STDMETHODIMP DropTarget::DragLeave()
    {
        if (m_helper)
        {
            m_helper->Show(TRUE);   // the thumbnail follows the cursor again outside the wheel
            m_helper->DragLeave();
        }
        m_accepting = false;
        m_sink->OnDragLeave();
        return S_OK;
    }

    STDMETHODIMP DropTarget::Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect)
    {
        POINT p{ pt.x, pt.y };   // IDropTargetHelper takes a non-const POINT*
        const bool accepting = m_accepting;
        m_accepting = false;
        *effect = accepting ? m_sink->OnDragOver(p, *effect) : DROPEFFECT_NONE;
        if (m_helper)
            m_helper->Drop(data, &p, *effect);

        // The drag source (e.g. Explorer) is blocked inside DoDragDrop until we return, so Drop only
        // reads the paths and hands them on; conversion runs on the job queue.
        if (accepting && *effect != DROPEFFECT_NONE)
            m_sink->OnDrop(ReadDroppedFiles(data), p);
        else
            m_sink->OnDragLeave();
        return S_OK;
    }
}
