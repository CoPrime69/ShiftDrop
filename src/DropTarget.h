#pragma once
#include <windows.h>
#include <oleidl.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <string>
#include <vector>

namespace sd
{
    // What the overlay implements to receive drag events. Everything is called on the UI thread:
    // OLE marshals the cross-process IDropTarget calls from the drag source to the thread that
    // called RegisterDragDrop.
    struct IDropSink
    {
        // files is empty when the data has no CF_HDROP (text, a URL, a browser image...).
        virtual bool OnDragEnter(const std::vector<std::wstring>& files, POINT pt) = 0;
        // Returns the effect to report for this point; `allowed` is what the source permits.
        virtual DWORD OnDragOver(POINT pt, DWORD allowed) = 0;
        virtual void OnDragLeave() = 0;
        virtual void OnDrop(const std::vector<std::wstring>& files, POINT pt) = 0;

    protected:
        ~IDropSink() = default;
    };

    // Our IDropTarget. RuntimeClass<ClassicCom> supplies IUnknown (QueryInterface/AddRef/Release),
    // and Microsoft::WRL::Make creates it with a ref count of 1, so there is no bare new/delete.
    class DropTarget final
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropTarget>
    {
    public:
        explicit DropTarget(IDropSink* sink);

        STDMETHOD(DragEnter)(IDataObject* data, DWORD keyState, POINTL pt, DWORD* effect) override;
        STDMETHOD(DragOver)(DWORD keyState, POINTL pt, DWORD* effect) override;
        STDMETHOD(DragLeave)() override;
        STDMETHOD(Drop)(IDataObject* data, DWORD keyState, POINTL pt, DWORD* effect) override;

    private:
        IDropSink* m_sink;   // not owned; the Overlay outlives the registration (RevokeDragDrop first)
        bool m_accepting = false;
        // Deliberately no IDropTargetHelper. Explorer's drag thumbnail and its "+ Copy" label are only
        // drawn over windows that report the drag to that helper, so leaving it out hides both while
        // the cursor is over the wheel (they sat on the centre and covered the top slices). They come
        // back as soon as the cursor is over any other window.
    };

    // Reads CF_HDROP from a data object. Returns an empty vector if the format is not present.
    std::vector<std::wstring> ReadDroppedFiles(IDataObject* data);

    // Explorer treats Shift as "move", Ctrl as "copy" and Alt as "link" during a drag. Whatever
    // modifier the user holds, we answer COPY: with DROPEFFECT_MOVE the *source* would delete the
    // original after the drop. LINK is the fallback for sources that do not offer COPY.
    DWORD SafeEffect(DWORD allowed);
}
