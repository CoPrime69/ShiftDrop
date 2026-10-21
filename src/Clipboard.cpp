#include "Clipboard.h"
#include "FileActions.h"

#include <shlobj.h>
#include <wil/resource.h>

#include <cstring>

namespace sd::clipboard
{
    namespace
    {
        // A moveable block holding `bytes`, as SetClipboardData wants; null when out of memory.
        wil::unique_hglobal Block(const void* bytes, size_t size)
        {
            wil::unique_hglobal block(GlobalAlloc(GMEM_MOVEABLE, size));
            if (!block)
                return block;
            void* p = GlobalLock(block.get());
            if (!p)
                return nullptr;
            memcpy(p, bytes, size);
            GlobalUnlock(block.get());
            return block;
        }

        // Another app may have the clipboard open for a moment (a clipboard manager reading what was
        // just copied): try for about a fifth of a second.
        bool Open(HWND owner)
        {
            for (int attempt = 0; attempt < 10; ++attempt)
            {
                if (OpenClipboard(owner))
                    return true;
                Sleep(20);
            }
            return false;
        }

        // On success the clipboard owns the block, so release ours only then.
        bool Put(UINT format, wil::unique_hglobal& block)
        {
            if (!block || !SetClipboardData(format, block.get()))
                return false;
            block.release();
            return true;
        }
    }

    bool SetText(HWND owner, const std::wstring& text, std::wstring* error)
    {
        auto block = Block(text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        bool ok = false;
        if (block && Open(owner))
        {
            ok = EmptyClipboard() && Put(CF_UNICODETEXT, block);
            CloseClipboard();
            if (!ok && error)
                *error = L"could not write to the clipboard";
        }
        else if (error)
            *error = block ? L"the clipboard is busy" : L"could not write to the clipboard";
        return ok;
    }

    bool SetFiles(HWND owner, const std::vector<std::wstring>& files, DWORD effect, std::span<const uint8_t> png, std::span<const uint8_t> dib)
    {
        wil::unique_hglobal drop = MakeDropFiles(files);
        auto effectBlock = Block(&effect, sizeof(effect));
        if (!drop || !effectBlock || !Open(owner))
            return false;
        auto close = wil::scope_exit([] { CloseClipboard(); });
        EmptyClipboard();
        if (!Put(CF_HDROP, drop))
            return false;
        static const UINT kPreferredEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        static const UINT kPng = RegisterClipboardFormatW(L"PNG");
        Put(kPreferredEffect, effectBlock);
        if (!dib.empty())
            if (auto block = Block(dib.data(), dib.size()))
                Put(CF_DIB, block);
        if (!png.empty())
            if (auto block = Block(png.data(), png.size()))
                Put(kPng, block);
        return true;
    }
}
