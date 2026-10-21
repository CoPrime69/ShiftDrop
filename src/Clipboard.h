#pragma once
#include <windows.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Writing to the clipboard, for every place that copies: Copy and Copy path, Copy text (OCR), the
// inspector's values and the shelf's files. One way of doing it: the clipboard's contents are
// replaced, another app holding it open for a moment is waited for briefly, and memory the clipboard
// did not take is freed. `owner` may be null (a worker thread).
namespace sd::clipboard
{
    // Plain text (CF_UNICODETEXT). `error` (optional) says why it failed.
    bool SetText(HWND owner, const std::wstring& text, std::wstring* error = nullptr);
    // The clipboard's plain text; false when it holds none.
    bool GetText(HWND owner, std::wstring& text);

    // Files the way Explorer's Copy puts them: CF_HDROP plus "Preferred DropEffect", which tells
    // Explorer's Paste to copy or move (`effect`). With a picture of a single image file, pasting into
    // a chat app or a document gets the image itself: `png` (the "PNG" format, keeps transparency)
    // and `dib` (CF_DIB: a BITMAPINFOHEADER and its rows).
    bool SetFiles(HWND owner, const std::vector<std::wstring>& files, DWORD effect, std::span<const uint8_t> png = {},
                  std::span<const uint8_t> dib = {});
}
