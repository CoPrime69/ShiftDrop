#include "DropTarget.h"
#include "DropDescription.h"
#include "Log.h"
#include "VirtualDrop.h"

#include <shellapi.h>
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

    DropTarget::DropTarget(HWND hwnd, IDropSink* sink, Microsoft::WRL::ComPtr<IDropTargetHelper> helper)
        : m_hwnd(hwnd), m_sink(sink), m_helper(std::move(helper))
    {
    }

    STDMETHODIMP DropTarget::DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect)
    {
        // Reading CF_HDROP here (not only at Drop) lets the wheel show only the actions these files support.
        auto files = ReadDroppedFiles(data);
        EndVirtual();
        if (PreferVirtualFiles(files))
        {
            // No paths (or only to temporary copies): perhaps files dragged out of a browser or a mail
            // app. Only their names are read now; their contents are saved when dropped on a slice.
            if (auto items = ReadVirtualItems(data); !items.empty() && (m_virtual = VirtualDrop::ForDrag(std::move(items))))
            {
                m_virtual->Attach(data);
                m_virtual->SetFallback(std::move(files));
                files = m_virtual->Paths();
                log::Info(L"drag: {} virtual file(s), first '{}'", files.size(), files.front());
            }
        }
        POINT p{ pt.x, pt.y };   // IDropTargetHelper takes a non-const POINT*
        // Only a picture the helper keeps showing over the wheel is in the way of its centre.
        const auto picture = m_helper ? DragImageRect(data, p) : std::nullopt;
        m_sink->OnDragPicture(picture ? &*picture : nullptr);
        m_accepting = m_sink->OnDragEnter(files, p);
        if (!m_accepting)
            EndVirtual();
        *effect = m_accepting ? m_sink->OnDragOver(p, *effect) : DROPEFFECT_NONE;
        if (m_accepting)
        {
            m_data = data;
            // The picture stays, but no "+ Copy" badge under it: the wheel says what will happen.
            SetDropDescription(data, DROPIMAGE_NOIMAGE);
        }
        if (m_helper)
        {
            // The same for files with no path (a browser's image, a mail attachment): the helper
            // shows whatever picture the source made.
            m_helper->DragEnter(m_hwnd, data, &p, *effect);
            // The wheel and the top panel opened after the drag began, on top of the picture.
            m_dragWindow = DragImageWindow(data);
            KeepDragImageOnTop(m_dragWindow);
        }
        return S_OK;
    }

    STDMETHODIMP DropTarget::DragOver(DWORD, POINTL pt, DWORD* effect)
    {
        POINT p{ pt.x, pt.y };   // IDropTargetHelper takes a non-const POINT*
        *effect = m_accepting ? m_sink->OnDragOver(p, *effect) : DROPEFFECT_NONE;
        if (m_helper)
        {
            m_helper->DragOver(&p, *effect);
            KeepDragImageOnTop(m_dragWindow);   // e.g. the predicted-output card just appeared
        }
        return S_OK;
    }

    STDMETHODIMP DropTarget::DragLeave()
    {
        if (m_helper)
            m_helper->DragLeave();
        if (m_data)
            SetDropDescription(m_data.Get(), DROPIMAGE_INVALID);   // other windows get their usual badge
        m_data.Reset();
        m_dragWindow = nullptr;
        m_accepting = false;
        m_sink->OnDragLeave();
        EndVirtual();
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
        m_data.Reset();
        m_dragWindow = nullptr;

        // The drag source (e.g. Explorer) is blocked inside DoDragDrop until we return, so Drop only
        // reads the paths and hands them on; conversion runs on the job queue. Virtual files are
        // saved by whoever takes the drop (VirtualDrop::Save), still inside this call: the source
        // may let go of their contents once it returns.
        if (accepting && *effect != DROPEFFECT_NONE)
        {
            if (m_virtual)
                m_virtual->Attach(data);
            m_sink->OnDrop(m_virtual ? m_virtual->Paths() : ReadDroppedFiles(data), p);
        }
        else
            m_sink->OnDragLeave();
        EndVirtual();
        return S_OK;
    }

    void DropTarget::EndVirtual()
    {
        // The data object goes on this thread; a job that took the drop keeps the saved files.
        if (m_virtual)
            m_virtual->Detach();
        m_virtual.reset();
    }
}
