#pragma once
#include <windows.h>
#include <oleidl.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <memory>
#include <string>
#include <vector>

namespace sd
{
    // What the overlay implements to receive drag events. Everything is called on the UI thread:
    // OLE marshals the cross-process IDropTarget calls from the drag source to the thread that
    // called RegisterDragDrop.
    struct IDropSink
    {
        // files is empty when the drag carries no files (text, a URL...). Files dragged out of a
        // browser or a mail app ("virtual files", VirtualDrop.h) come as the paths they will be
        // saved at on the drop: not on disk yet, but named, so the wheel can be filtered by them.
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
    class VirtualDrop;

    class DropTarget final
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropTarget>
    {
    public:
        // `helper` is the shell's drag-image helper (MakeDragImageHelper; null: no drag picture over
        // `hwnd`). Every drag event is handed to it with `hwnd`, so the source's picture of the item
        // stays visible over the window and follows the cursor.
        DropTarget(HWND hwnd, IDropSink* sink, Microsoft::WRL::ComPtr<IDropTargetHelper> helper);

        STDMETHOD(DragEnter)(IDataObject* data, DWORD keyState, POINTL pt, DWORD* effect) override;
        STDMETHOD(DragOver)(DWORD keyState, POINTL pt, DWORD* effect) override;
        STDMETHOD(DragLeave)() override;
        STDMETHOD(Drop)(IDataObject* data, DWORD keyState, POINTL pt, DWORD* effect) override;

    private:
        void EndVirtual();

        HWND m_hwnd;
        IDropSink* m_sink;   // not owned; the Overlay outlives the registration (RevokeDragDrop first)
        bool m_accepting = false;
        Microsoft::WRL::ComPtr<IDataObject> m_data;   // while accepting, to give the badge back on leave
        std::shared_ptr<VirtualDrop> m_virtual;       // the drag's virtual files, from DragEnter to its end
        Microsoft::WRL::ComPtr<IDropTargetHelper> m_helper;
        HWND m_dragWindow = nullptr;   // the source's drag picture, kept above our windows during the drag
    };

    // Reads CF_HDROP from a data object. Returns an empty vector if the format is not present.
    std::vector<std::wstring> ReadDroppedFiles(IDataObject* data);

    // Explorer treats Shift as "move", Ctrl as "copy" and Alt as "link" during a drag. Whatever
    // modifier the user holds, we answer COPY: with DROPEFFECT_MOVE the *source* would delete the
    // original after the drop. LINK is the fallback for sources that do not offer COPY.
    DWORD SafeEffect(DWORD allowed);
}
