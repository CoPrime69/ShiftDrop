#pragma once
#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

// Carrying files on to another app: dragging them out (from the Float shelf or from a finished
// job's result in the top panel), putting them on the clipboard, and showing them in Explorer.
namespace sd::shelf
{
    // A shell data object for the files, the same kind Explorer puts in a drag: Shell IDList Array
    // (Explorer, mail), CF_HDROP (browsers, chat apps, almost everything else) and file contents on
    // demand. The files may be in different folders. Null if none of them could be parsed.
    Microsoft::WRL::ComPtr<IDataObject> MakeFilesDataObject(const std::vector<std::wstring>& files);

    // Drags the files out (modal: returns once they are dropped or the drag is cancelled) with the
    // shell's drag picture. Copy or link only, so no target can move the originals away. Returns
    // the effect the target chose (DROPEFFECT_NONE when cancelled).
    DWORD DragOut(HWND source, const std::vector<std::wstring>& files);

    // Explorer with the files selected, one window per folder (at most four folders).
    void ShowInFolder(const std::vector<std::wstring>& files);

    // Open (the top panel's result, a shelf item): the file itself in its default app. Of a few
    // outputs (up to kOpenFirstAtMost) the first; of more (a folder of frames, a split PDF) the
    // folder they are in, so Open never starts a dozen apps. Empty for no files.
    inline constexpr size_t kOpenFirstAtMost = 3;
    struct OpenTarget
    {
        std::wstring path;
        bool folder = false;   // the folder the outputs are in, not one of them
        bool operator==(const OpenTarget&) const = default;
    };
    OpenTarget ChooseOpen(const std::vector<std::wstring>& files);
    // Opens ChooseOpen's choice with the shell's "open" verb. False when there was nothing to open or
    // the shell refused (no app for the type: Windows asks which one).
    bool OpenFiles(const std::vector<std::wstring>& files);

    // A picture that goes on the clipboard next to a single image file, so pasting into a chat
    // app or a document gets the image itself.
    struct ClipboardImage
    {
        std::vector<uint8_t> png;   // the "PNG" clipboard format (keeps transparency)
        std::vector<uint8_t> dib;   // CF_DIB: BITMAPINFOHEADER + bottom-up 32 bpp rows, flattened on white
        unsigned width = 0, height = 0;
    };
    // Is this a file MakeClipboardImage can turn into a picture (PNG, JPEG, WebP, TIFF, BMP, GIF)?
    bool IsClipboardImage(const std::wstring& path);
    // Decodes the image (EXIF orientation applied) into both formats. Slow for big photos: call it on
    // a worker thread with COM initialised. Pictures over 64 megapixels are refused.
    HRESULT MakeClipboardImage(const std::wstring& path, ClipboardImage& out, std::wstring& error);

    // Replaces the clipboard with the files (CF_HDROP and "Preferred DropEffect" = copy, as
    // Explorer's Copy does) and, with `image`, the picture too. UI thread (the owner window's).
    bool PutOnClipboard(HWND owner, const std::vector<std::wstring>& files, const ClipboardImage* image);
}
