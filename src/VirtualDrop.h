#pragma once
#include "Settings.h"

#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sd
{
    // "Virtual files": an image dragged out of a web page, an attachment dragged out of a mail app.
    // The drag carries names (CFSTR_FILEDESCRIPTORW) and contents (CFSTR_FILECONTENTS) but no paths
    // (no CF_HDROP). During the drag only the names are read, so the wheel is filtered by them.
    // On the drop, the contents are saved into a private drop folder under
    // %LOCALAPPDATA%\ShiftDrop\dropped, the slice runs on those copies like on any file, its outputs
    // go to the folder the user chose (the Desktop by default, OutputPath.h's RedirectOutputs), and
    // the drop folder is deleted when the job is over.

    struct VirtualItem
    {
        std::wstring name;    // as the source gave it; may hold a relative path
        LONG index = 0;       // its lindex for CFSTR_FILECONTENTS
        uint64_t size = 0;
        bool hasSize = false;
    };

    // A name that is safe as one file name in one folder: no folder parts, none of \ / : * ? " < > |
    // or control characters, no trailing dots or spaces, not a device name (CON, NUL, COM1...), and
    // not empty.
    std::wstring SafeFileName(std::wstring_view name);

    // The files of a FILEGROUPDESCRIPTORW (`wide`) or FILEGROUPDESCRIPTORA block; folders are left
    // out (their files come as items of their own). Reads no further than `bytes`.
    std::vector<VirtualItem> ParseFileGroupDescriptor(const void* block, size_t bytes, bool wide);
    // The virtual files a drag offers (names only), or none.
    std::vector<VirtualItem> ReadVirtualItems(IDataObject* data);
    // Whether a drag's virtual files win over the paths (CF_HDROP) it also gives: when there are no
    // paths, or every one is in the temp folder. Firefox hands over a temporary copy of a picture
    // that way (and Explorer a file inside a ZIP), and outputs written next to it would be lost.
    bool PreferVirtualFiles(const std::vector<std::wstring>& paths);

    // The output folder setting for drags (from settings.ini at startup, then the tray menu).
    void SetVirtualOutputChoice(VirtualOutput choice);
    VirtualOutput VirtualOutputChoice();
    // The folder of a choice, from the known-folder APIs (FOLDERID_Desktop, FOLDERID_Downloads).
    // Ask gives the Desktop: predicted names are worked out there until a folder is picked.
    std::wstring VirtualOutputFolder(VirtualOutput choice);

    // %LOCALAPPDATA%\ShiftDrop\dropped, where drop folders live.
    std::wstring VirtualDropRoot();
    // Deletes drop folders a crash left behind (at startup, before any drag).
    void RemoveStaleVirtualDrops(const std::wstring& root);

    // Bytes saved so far and in all (0 when the source does not say).
    using SaveProgress = std::function<void(uint64_t done, uint64_t total)>;

    // One drag's virtual files, from DragEnter until the job that uses them is over. Shared: the drop
    // target holds it during the drag, the job it starts holds it after, and the last one out deletes
    // the drop folder.
    class VirtualDrop
    {
    public:
        // A drop folder under `root` (nothing is written until Save) whose outputs go to
        // `outputFolder`. `askFolder`: the user picks the output folder after the drop.
        static std::shared_ptr<VirtualDrop> Create(std::vector<VirtualItem> items, const std::wstring& root,
                                                   const std::wstring& outputFolder, bool askFolder);
        // The same, for a drag: under VirtualDropRoot(), into the folder of VirtualOutputChoice().
        static std::shared_ptr<VirtualDrop> ForDrag(std::vector<VirtualItem> items);
        // The live drop that `path` is a file of, or null.
        static std::shared_ptr<VirtualDrop> Find(const std::wstring& path);

        ~VirtualDrop();   // deletes the drop folder with the saved copies, and the redirect
        VirtualDrop(const VirtualDrop&) = delete;
        VirtualDrop& operator=(const VirtualDrop&) = delete;

        // Where each item is saved: what the wheel sees as the dragged files.
        const std::vector<std::wstring>& Paths() const { return m_paths; }
        const std::wstring& Folder() const { return m_folder; }
        const std::wstring& OutputFolder() const { return m_output; }
        void SetOutputFolder(const std::wstring& folder);
        bool AsksFolder() const { return m_ask; }
        bool Saved() const { return m_saved; }

        // The drag's data object, while the drag is over ShiftDrop (DropTarget). Detach on the thread
        // that attached it: the object is a proxy belonging to that apartment.
        void Attach(IDataObject* data) { m_data = data; }
        void Detach() { m_data.Reset(); }
        // The paths the drag also gave (PreferVirtualFiles), one per item: copied from when the source
        // does not hand an item's contents over.
        void SetFallback(std::vector<std::wstring> paths) { m_fallback = std::move(paths); }

        // Reads every item from the attached data object into Paths(), once (later calls return the
        // same files). On the UI thread, inside Drop or during the drag while the source still holds
        // the data. Returns the files saved; `error` says why the first one that failed did.
        std::vector<std::wstring> Save(const SaveProgress& progress, std::wstring& error);

    private:
        VirtualDrop() = default;

        std::vector<VirtualItem> m_items;
        std::vector<std::wstring> m_paths;
        std::vector<std::wstring> m_savedPaths;
        std::vector<std::wstring> m_fallback;
        std::wstring m_folder;   // the drop folder, root\xxxxxxxx; the items are in a folder inside it
        std::wstring m_output;
        bool m_ask = false;
        bool m_saved = false;
        Microsoft::WRL::ComPtr<IDataObject> m_data;
    };

    // Moves a saved copy out of its drop folder into `folder`, under its own name or "name (2).ext",
    // never replacing anything. For what hands the file itself on (Copy, Float, Share, Move to…): it
    // has to outlive the drop folder.
    HRESULT KeepVirtualFile(const std::wstring& file, const std::wstring& folder, std::wstring& kept);
}
