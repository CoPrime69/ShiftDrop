#include "shelf/Carry.h"
#include "FileActions.h"
#include "ImagePipeline.h"
#include "Log.h"
#include "OutputPath.h"
#include "Recipe.h"

#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wil/resource.h>

#include <algorithm>
#include <filesystem>
#include <map>

using Microsoft::WRL::ComPtr;

namespace sd::shelf
{
    namespace
    {
        // ILFree as the deleter: PIDLs are __unaligned pointers on x64, which a plain void* deleter would not accept.
        using Pidl = wil::unique_any<PIDLIST_ABSOLUTE, decltype(&::ILFree), ::ILFree>;

        constexpr unsigned long long kMaxClipboardPixels = 64ull * 1000 * 1000;

        Pidl ParsePath(const std::wstring& path)
        {
            PIDLIST_ABSOLUTE raw = nullptr;
            if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &raw, 0, nullptr)))
                return Pidl();
            return Pidl(raw);
        }

        // Copies `bytes` into a movable HGLOBAL, as SetClipboardData wants it.
        wil::unique_hglobal ToGlobal(const std::vector<uint8_t>& bytes)
        {
            wil::unique_hglobal mem(GlobalAlloc(GMEM_MOVEABLE, bytes.size()));
            if (!mem)
                return mem;
            memcpy(GlobalLock(mem.get()), bytes.data(), bytes.size());
            GlobalUnlock(mem.get());
            return mem;
        }

        HRESULT StreamBytes(IStream* stream, std::vector<uint8_t>& bytes)
        {
            STATSTG stat{};
            HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME);
            if (FAILED(hr))
                return hr;
            if (stat.cbSize.QuadPart > 0x7FFFFFFF)
                return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            bytes.resize(static_cast<size_t>(stat.cbSize.QuadPart));
            const LARGE_INTEGER zero{};
            if (FAILED(hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr)))
                return hr;
            ULONG read = 0;
            hr = stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
            return SUCCEEDED(hr) && read != bytes.size() ? E_FAIL : hr;
        }
    }

    ComPtr<IDataObject> MakeFilesDataObject(const std::vector<std::wstring>& files)
    {
        std::vector<Pidl> pidls;
        std::vector<std::wstring> parsed;
        for (const auto& f : files)
            if (auto pidl = ParsePath(f))
            {
                pidls.push_back(std::move(pidl));
                parsed.push_back(f);
            }
        if (pidls.empty())
            return nullptr;
        // The shell's object renders CF_HDROP, file contents and the IDList array from a folder and
        // its children, like a drag from Explorer. Files from one folder (the usual case) use that
        // folder; files from several use the desktop as the parent with absolute IDs as "children".
        const auto folderOf = [](const std::wstring& p) { return std::filesystem::path(p).parent_path().wstring(); };
        const bool oneFolder = std::all_of(parsed.begin(), parsed.end(), [&](const std::wstring& p) {
            return CompareStringOrdinal(folderOf(p).c_str(), -1, folderOf(parsed.front()).c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        Pidl parent;
        if (oneFolder)
            parent = ParsePath(folderOf(parsed.front()));
        std::vector<PCUITEMID_CHILD> children;
        for (const auto& p : pidls)
            children.push_back(parent ? reinterpret_cast<PCUITEMID_CHILD>(ILFindLastID(p.get())) : reinterpret_cast<PCUITEMID_CHILD>(p.get()));
        ComPtr<IDataObject> data;
        if (FAILED(SHCreateDataObject(parent.get(), static_cast<UINT>(children.size()), children.data(), nullptr, IID_PPV_ARGS(&data))))
        {
            // The folder's own data object, from IShellFolder (what Wine, which lacks
            // SHCreateDataObject, offers).
            ComPtr<IShellFolder> folder;
            const HRESULT hr = parent ? SHBindToObject(nullptr, parent.get(), nullptr, IID_PPV_ARGS(&folder)) : SHGetDesktopFolder(&folder);
            if (FAILED(hr) ||
                FAILED(folder->GetUIObjectOf(nullptr, static_cast<UINT>(children.size()), children.data(), IID_IDataObject, nullptr,
                                             reinterpret_cast<void**>(data.ReleaseAndGetAddressOf()))))
                return nullptr;
        }
        // CF_HDROP is what browsers and chat apps read. The shell renders it from the IDs too, but we
        // set it ourselves from the paths we were given, so it is exactly those paths whichever the
        // object hands out.
        FORMATETC hdrop{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        if (auto block = MakeDropFiles(parsed))
        {
            STGMEDIUM medium{ TYMED_HGLOBAL };
            medium.hGlobal = block.get();
            if (SUCCEEDED(data->SetData(&hdrop, &medium, TRUE)))
                block.release();   // the data object owns it now
        }
        return data;
    }

    DWORD DragOut(HWND source, const std::vector<std::wstring>& files)
    {
        auto data = MakeFilesDataObject(files);
        if (!data)
        {
            log::Warn(L"carry: none of the {} file(s) could be dragged", files.size());
            return DROPEFFECT_NONE;
        }
        // SHDoDragDrop supplies the drop source and the shell's drag picture (thumbnails and a count).
        DWORD effect = DROPEFFECT_NONE;
        const HRESULT hr = SHDoDragDrop(source, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
        log::Info(L"carry: dragged {} file(s) out: {}", files.size(),
                  hr == DRAGDROP_S_DROP ? (effect & DROPEFFECT_LINK ? L"linked" : L"copied") : L"cancelled");
        return hr == DRAGDROP_S_DROP ? effect : DROPEFFECT_NONE;
    }

    void ShowInFolder(const std::vector<std::wstring>& files)
    {
        std::map<std::wstring, std::vector<std::wstring>> byFolder;
        for (const auto& f : files)
            byFolder[std::filesystem::path(f).parent_path().wstring()].push_back(f);
        size_t opened = 0;
        for (const auto& [folder, items] : byFolder)
        {
            if (opened++ == 4)
                break;
            const auto folderPidl = ParsePath(folder);
            if (!folderPidl)
                continue;
            std::vector<Pidl> pidls;
            std::vector<PCUITEMID_CHILD> children;
            for (const auto& f : items)
                if (auto p = ParsePath(f))
                {
                    // The array wants IDs relative to the folder: the last ID of each absolute one.
                    children.push_back(reinterpret_cast<PCUITEMID_CHILD>(ILFindLastID(p.get())));
                    pidls.push_back(std::move(p));
                }
            SHOpenFolderAndSelectItems(folderPidl.get(), static_cast<UINT>(children.size()), children.data(), 0);
        }
    }

    bool IsClipboardImage(const std::wstring& path)
    {
        const auto ext = LowerExtension(path);
        return FormatOfExtension(ext) != nullptr || ext == L".gif";
    }

    HRESULT MakeClipboardImage(const std::wstring& path, ClipboardImage& out, std::wstring& error)
    {
        // 1. PNG through the image pipeline: it decodes WebP with libwebp and applies EXIF orientation.
        Recipe png;
        png.id = "clipboard-png";
        png.steps.push_back(RecipeStep{ "image.encode", nlohmann::json{ { "format", "png" }, { "quality", 90 } } });
        ComPtr<IStream> stream;
        HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        if (FAILED(hr))
            return hr;
        if (FAILED(hr = RunImageRecipe(png, path, stream.Get(), std::stop_token{}, error)))
            return hr;
        if (FAILED(hr = StreamBytes(stream.Get(), out.png)))
            return hr;

        // 2. The same pixels as a DIB for apps that only read CF_DIB.
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> wicStream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(hr = factory->CreateStream(&wicStream)) ||
            FAILED(hr = wicStream->InitializeFromMemory(out.png.data(), static_cast<DWORD>(out.png.size()))) ||
            FAILED(hr = factory->CreateDecoderFromStream(wicStream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(hr = decoder->GetFrame(0, &frame)) ||
            FAILED(hr = factory->CreateFormatConverter(&converter)) ||
            FAILED(hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                              WICBitmapPaletteTypeCustom)))
        {
            error = L"decode: " + HResultText(hr);
            return hr;
        }
        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        if (w == 0 || h == 0 || static_cast<unsigned long long>(w) * h > kMaxClipboardPixels)
        {
            error = L"the picture is too large for the clipboard";
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        }
        const size_t stride = static_cast<size_t>(w) * 4;
        std::vector<uint8_t> pixels(stride * h);
        if (FAILED(hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(pixels.size()), pixels.data())))
        {
            error = L"decode: " + HResultText(hr);
            return hr;
        }

        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = static_cast<LONG>(w);
        header.biHeight = static_cast<LONG>(h);   // positive = bottom-up rows, the classic DIB layout
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;
        header.biSizeImage = static_cast<DWORD>(pixels.size());
        out.dib.resize(sizeof(header) + pixels.size());
        memcpy(out.dib.data(), &header, sizeof(header));
        uint8_t* dst = out.dib.data() + sizeof(header);
        for (UINT y = 0; y < h; ++y)
        {
            const uint8_t* src = pixels.data() + static_cast<size_t>(h - 1 - y) * stride;
            uint8_t* row = dst + static_cast<size_t>(y) * stride;
            for (UINT x = 0; x < w; ++x)
            {
                // Straight alpha over white; CF_DIB readers mostly ignore the alpha byte.
                const unsigned a = src[x * 4 + 3];
                for (int c = 0; c < 3; ++c)
                    row[x * 4 + c] = static_cast<uint8_t>((src[x * 4 + c] * a + 255u * (255u - a) + 127u) / 255u);
                row[x * 4 + 3] = 255;
            }
        }
        out.width = w;
        out.height = h;
        return S_OK;
    }

    bool PutOnClipboard(HWND owner, const std::vector<std::wstring>& files, const ClipboardImage* image)
    {
        wil::unique_hglobal drop = MakeDropFiles(files);
        wil::unique_hglobal effect(GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD)));
        if (!drop || !effect)
            return false;
        *static_cast<DWORD*>(GlobalLock(effect.get())) = DROPEFFECT_COPY;
        GlobalUnlock(effect.get());
        wil::unique_hglobal dib, png;
        if (image && !image->dib.empty())
        {
            dib = ToGlobal(image->dib);
            png = ToGlobal(image->png);
        }

        // Another app may have the clipboard open for a moment; try a few times.
        bool open = false;
        for (int attempt = 0; attempt < 4 && !(open = OpenClipboard(owner) != FALSE); ++attempt)
            Sleep(15);
        if (!open)
            return false;
        auto close = wil::scope_exit([] { CloseClipboard(); });
        EmptyClipboard();
        static const UINT kPreferredEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        static const UINT kPng = RegisterClipboardFormatW(L"PNG");
        // On success the clipboard owns each block, so release ours only then.
        if (!SetClipboardData(CF_HDROP, drop.get()))
            return false;
        drop.release();
        if (SetClipboardData(kPreferredEffect, effect.get()))
            effect.release();
        if (dib && SetClipboardData(CF_DIB, dib.get()))
            dib.release();
        if (png && SetClipboardData(kPng, png.get()))
            png.release();
        return true;
    }
}
