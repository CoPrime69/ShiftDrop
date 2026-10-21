#include "VirtualDrop.h"
#include "ErrorText.h"
#include "Log.h"
#include "OutputPath.h"
#include "PathUtil.h"
#include "Strings.h"
#include "TempPath.h"
#include "Utf.h"

#include <shlobj.h>
#include <wil/resource.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <format>
#include <mutex>
#include <random>

namespace sd
{
    namespace
    {
        // A drop's items sit in a folder of this name inside it, so an output named after their
        // folder (a ZIP of all of them) is "Dropped files (zipped).zip".
        constexpr wchar_t kItemsFolder[] = L"Dropped files";
        constexpr size_t kMaxItems = 1000;
        constexpr size_t kMaxNameChars = 200;   // leaves room for " (converted 12)" and the folder

        std::atomic<int> g_choice{ static_cast<int>(VirtualOutput::Desktop) };

        std::mutex g_dropsMutex;
        std::vector<std::weak_ptr<VirtualDrop>> g_drops;

        bool IsDropFolderName(const std::wstring& name)
        {
            return name.size() == 8 && std::all_of(name.begin(), name.end(), [](wchar_t c) { return std::iswxdigit(c) != 0; });
        }

        std::wstring FromAnsi(const char* text, size_t max)
        {
            const size_t len = strnlen(text, max);
            if (len == 0)
                return {};
            const int n = MultiByteToWideChar(CP_ACP, 0, text, static_cast<int>(len), nullptr, 0);
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_ACP, 0, text, static_cast<int>(len), out.data(), n);
            return out;
        }

        template <class Descriptor>
        void AddItem(const Descriptor& d, LONG index, std::wstring name, std::vector<VirtualItem>& items)
        {
            // A folder of the source's (an FTP client, an archive viewer) is not saved itself; its
            // files are items of their own, saved side by side.
            if ((d.dwFlags & FD_ATTRIBUTES) && (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                return;
            VirtualItem item;
            item.name = std::move(name);
            item.index = index;
            if (d.dwFlags & FD_FILESIZE)
            {
                item.hasSize = true;
                item.size = FileSizeOf(d);
            }
            items.push_back(std::move(item));
        }

        // Writes everything the stream has left, from its start when it can seek, to a new file.
        HRESULT WriteStream(IStream* stream, HANDLE file, uint64_t& done, const std::function<void()>& report)
        {
            LARGE_INTEGER zero{};
            stream->Seek(zero, STREAM_SEEK_SET, nullptr);   // some streams can't seek; they start at the start
            std::vector<BYTE> buffer(256 * 1024);
            for (;;)
            {
                ULONG read = 0;
                const HRESULT hr = stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &read);
                if (FAILED(hr))
                    return hr;
                if (read == 0)
                    return S_OK;
                DWORD written = 0;
                if (!WriteFile(file, buffer.data(), read, &written, nullptr) || written != read)
                    return HRESULT_FROM_WIN32(GetLastError());
                done += read;
                report();
                if (hr == S_FALSE)   // the end came within this read
                    return S_OK;
            }
        }

        HRESULT WriteMemory(HGLOBAL memory, const VirtualItem& item, HANDLE file, uint64_t& done, const std::function<void()>& report)
        {
            // GlobalSize may round up; the descriptor's size, when given, is the exact one.
            SIZE_T size = GlobalSize(memory);
            if (item.hasSize && item.size <= size)
                size = static_cast<SIZE_T>(item.size);
            const auto* bytes = static_cast<const BYTE*>(GlobalLock(memory));
            if (!bytes && size)
                return HRESULT_FROM_WIN32(GetLastError());
            auto unlock = wil::scope_exit([&] { GlobalUnlock(memory); });
            for (SIZE_T at = 0; at < size;)
            {
                const DWORD chunk = static_cast<DWORD>(std::min<SIZE_T>(size - at, 1 << 20));
                DWORD written = 0;
                if (!WriteFile(file, bytes + at, chunk, &written, nullptr) || written != chunk)
                    return HRESULT_FROM_WIN32(GetLastError());
                at += chunk;
                done += chunk;
                report();
            }
            return S_OK;
        }

        // An Outlook message (.msg) comes as a structured storage: copied whole into a new compound file.
        HRESULT WriteStorage(IStorage* source, const std::wstring& path)
        {
            Microsoft::WRL::ComPtr<IStorage> target;
            // No STGM_CREATE: fails if the file exists.
            HRESULT hr = StgCreateDocfile(path.c_str(), STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &target);
            if (SUCCEEDED(hr))
                hr = source->CopyTo(0, nullptr, nullptr, target.Get());
            if (SUCCEEDED(hr))
                hr = target->Commit(STGC_DEFAULT);
            return hr;
        }
    }

    std::wstring SafeFileName(std::wstring_view name)
    {
        // Only the last part: "folder\photo.png" and "../../x" must not leave the drop folder.
        if (const auto slash = name.find_last_of(L"\\/"); slash != std::wstring_view::npos)
            name.remove_prefix(slash + 1);
        std::wstring safe;
        for (const wchar_t c : name)
        {
            if (c < 32 || c == 127)
                continue;
            safe += std::wstring_view(L"<>:\"|?*").find(c) != std::wstring_view::npos ? L'_' : c;
        }
        // Windows drops trailing dots and spaces, which would make "a." and "a" the same file.
        while (!safe.empty() && (safe.back() == L'.' || safe.back() == L' '))
            safe.pop_back();
        while (!safe.empty() && safe.front() == L' ')
            safe.erase(safe.begin());
        if (safe.empty())
            safe = L"Dropped file";

        // Device names stay devices with any extension ("nul.txt"): prefix them.
        const auto base = safe.substr(0, safe.find(L'.'));
        static constexpr const wchar_t* kDevices[] = { L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$" };
        bool device = std::any_of(std::begin(kDevices), std::end(kDevices), [&](const wchar_t* d) { return EqualsNoCase(base, d); });
        const auto digit = [](wchar_t c) { return (c >= L'0' && c <= L'9') || c == L'¹' || c == L'²' || c == L'³'; };
        if (base.size() == 4 && digit(base[3]))
        {
            const auto prefix = base.substr(0, 3);
            device |= EqualsNoCase(prefix, L"COM") || EqualsNoCase(prefix, L"LPT");
        }
        if (device)
            safe = L"_" + safe;

        // Long names are cut before the extension, which says what the file is.
        if (safe.size() > kMaxNameChars)
        {
            auto ext = std::filesystem::path(safe).extension().wstring();
            if (ext.size() > 16)
                ext.clear();
            safe = safe.substr(0, kMaxNameChars - ext.size());
            while (!safe.empty() && (safe.back() == L'.' || safe.back() == L' '))
                safe.pop_back();
            safe += ext;
        }
        return safe;
    }

    std::vector<VirtualItem> ParseFileGroupDescriptor(const void* block, size_t bytes, bool wide)
    {
        std::vector<VirtualItem> items;
        if (!block || bytes < sizeof(UINT))
            return items;
        const auto* base = static_cast<const BYTE*>(block);
        const UINT count = *reinterpret_cast<const UINT*>(base);
        // The array starts where FILEGROUPDESCRIPTOR puts fgd[0] (after the count, with its alignment).
        const size_t offset = wide ? offsetof(FILEGROUPDESCRIPTORW, fgd) : offsetof(FILEGROUPDESCRIPTORA, fgd);
        const size_t each = wide ? sizeof(FILEDESCRIPTORW) : sizeof(FILEDESCRIPTORA);
        const size_t fits = bytes > offset ? (bytes - offset) / each : 0;
        const size_t n = std::min<size_t>({ count, fits, kMaxItems });
        for (size_t i = 0; i < n; ++i)
        {
            const BYTE* at = base + offset + i * each;
            if (wide)
            {
                FILEDESCRIPTORW d;
                memcpy(&d, at, sizeof(d));   // the block need not be aligned
                AddItem(d, static_cast<LONG>(i), std::wstring(d.cFileName, wcsnlen(d.cFileName, MAX_PATH)), items);
            }
            else
            {
                FILEDESCRIPTORA d;
                memcpy(&d, at, sizeof(d));
                AddItem(d, static_cast<LONG>(i), FromAnsi(d.cFileName, MAX_PATH), items);
            }
        }
        return items;
    }

    std::vector<VirtualItem> ReadVirtualItems(IDataObject* data)
    {
        if (!data)
            return {};
        static const CLIPFORMAT kWide = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
        static const CLIPFORMAT kAnsi = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORA));
        for (const bool wide : { true, false })
        {
            FORMATETC fmt{ wide ? kWide : kAnsi, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            STGMEDIUM medium{};
            if (FAILED(data->GetData(&fmt, &medium)))
                continue;
            auto release = wil::scope_exit([&] { ReleaseStgMedium(&medium); });
            if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal)
                continue;
            const void* block = GlobalLock(medium.hGlobal);
            if (!block)
                continue;
            auto items = ParseFileGroupDescriptor(block, GlobalSize(medium.hGlobal), wide);
            GlobalUnlock(medium.hGlobal);
            if (!items.empty())
                return items;
        }
        return {};
    }

    bool PreferVirtualFiles(const std::vector<std::wstring>& paths)
    {
        std::wstring folder = TempDirectory();
        if (folder.empty())
            return paths.empty();
        while (!folder.empty() && folder.back() == L'\\')
            folder.pop_back();
        // TEMP often holds the short 8.3 form of the profile folder (RUNNER~1); a drag may give
        // either form, so both count.
        std::wstring longFolder = folder;
        wchar_t full[MAX_PATH + 1]{};
        if (const DWORD m = GetLongPathNameW(folder.c_str(), full, ARRAYSIZE(full)); m > 0 && m <= MAX_PATH)
            longFolder.assign(full, m);
        return std::all_of(paths.begin(), paths.end(), [&](const std::wstring& p) { return IsWithin(p, folder) || IsWithin(p, longFolder); });
    }

    void SetVirtualOutputChoice(VirtualOutput choice) { g_choice = static_cast<int>(choice); }
    VirtualOutput VirtualOutputChoice() { return static_cast<VirtualOutput>(g_choice.load()); }

    std::wstring VirtualOutputFolder(VirtualOutput choice)
    {
        const auto known = [](REFKNOWNFOLDERID id) {
            wil::unique_cotaskmem_string path;
            // KF_FLAG_CREATE: a Downloads folder someone deleted comes back rather than failing.
            if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path)) || !path)
                return std::wstring();
            return std::wstring(path.get());
        };
        auto folder = known(choice == VirtualOutput::Downloads ? FOLDERID_Downloads : FOLDERID_Desktop);
        if (folder.empty() && choice == VirtualOutput::Downloads)
            folder = known(FOLDERID_Desktop);
        return folder;
    }

    std::wstring VirtualDropRoot()
    {
        return log::AppDataFolder() + L"\\dropped";
    }

    void RemoveStaleVirtualDrops(const std::wstring& root)
    {
        std::error_code ec;
        size_t removed = 0;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec))
        {
            // Only our own drop folders (eight hex digits), never anything else someone put there.
            if (!entry.is_directory(ec) || !IsDropFolderName(entry.path().filename().wstring()))
                continue;
            std::error_code removeError;
            if (std::filesystem::remove_all(entry.path(), removeError) > 0 && !removeError)
                ++removed;
        }
        if (removed)
            log::Info(L"dropped: removed {} drop folder(s) left from an earlier run", removed);
    }

    std::shared_ptr<VirtualDrop> VirtualDrop::Create(std::vector<VirtualItem> items, const std::wstring& root,
                                                     const std::wstring& outputFolder, bool askFolder)
    {
        if (items.empty())
            return nullptr;
        thread_local std::mt19937 rng{ std::random_device{}() };
        std::shared_ptr<VirtualDrop> drop(new VirtualDrop());
        drop->m_folder = (std::filesystem::path(root) / std::format(L"{:08x}", static_cast<unsigned>(rng()))).wstring();
        drop->m_output = outputFolder;
        drop->m_ask = askFolder || outputFolder.empty();
        const auto itemsFolder = std::filesystem::path(drop->m_folder) / kItemsFolder;
        for (const auto& item : items)
        {
            // Two items may have the same name (two "image.png" from one page): "image (2).png".
            const auto name = SafeFileName(item.name);
            auto unique = name;
            const std::filesystem::path p(name);
            for (int n = 2; std::any_of(drop->m_paths.begin(), drop->m_paths.end(),
                                        [&](const std::wstring& taken) { return EqualsNoCase(FileName(taken), unique); });
                 ++n)
                unique = std::format(L"{} ({}){}", p.stem().wstring(), n, p.extension().wstring());
            drop->m_paths.push_back((itemsFolder / unique).wstring());
        }
        drop->m_items = std::move(items);
        if (!drop->m_output.empty())
            RedirectOutputs(drop->m_folder, drop->m_output);
        std::scoped_lock lock(g_dropsMutex);
        std::erase_if(g_drops, [](const std::weak_ptr<VirtualDrop>& d) { return d.expired(); });
        g_drops.push_back(drop);
        return drop;
    }

    std::shared_ptr<VirtualDrop> VirtualDrop::ForDrag(std::vector<VirtualItem> items)
    {
        const auto choice = VirtualOutputChoice();
        return Create(std::move(items), VirtualDropRoot(), VirtualOutputFolder(choice), choice == VirtualOutput::Ask);
    }

    std::shared_ptr<VirtualDrop> VirtualDrop::Find(const std::wstring& path)
    {
        std::scoped_lock lock(g_dropsMutex);
        for (const auto& weak : g_drops)
            if (auto drop = weak.lock(); drop && IsWithin(path, drop->m_folder))
                return drop;
        return nullptr;
    }

    VirtualDrop::~VirtualDrop()
    {
        RedirectOutputs(m_folder, L"");
        std::error_code ec;
        if (std::filesystem::exists(m_folder, ec))
        {
            std::filesystem::remove_all(m_folder, ec);
            if (ec)
                log::Warn(L"dropped: could not delete {} ({}); it goes at the next start", m_folder, ec.value());
        }
    }

    void VirtualDrop::SetOutputFolder(const std::wstring& folder)
    {
        m_output = folder;
        m_ask = folder.empty();
        RedirectOutputs(m_folder, folder);
    }

    std::vector<std::wstring> VirtualDrop::Save(const SaveProgress& progress, std::wstring& error)
    {
        if (m_saved)
            return m_savedPaths;
        m_saved = true;
        if (!m_data)
        {
            error = L"the drag is over before its files could be saved";
            return {};
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(m_paths.front()).parent_path(), ec);
        if (ec)
        {
            error = std::format(L"cannot create {}: {}", m_folder, Widen(ec.message()));
            return {};
        }

        static const CLIPFORMAT kContents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
        uint64_t total = 0, done = 0;
        for (const auto& item : m_items)
            total += item.hasSize ? item.size : 0;
        const std::function<void()> report = [&] {
            if (progress)
                progress(done, total);
        };
        for (size_t i = 0; i < m_items.size(); ++i)
        {
            const auto& item = m_items[i];
            const auto& path = m_paths[i];
            // Ask for every medium we can write; some sources only answer when asked for exactly theirs.
            STGMEDIUM medium{};
            HRESULT hr = E_FAIL;
            for (const DWORD tymed : { DWORD(TYMED_ISTREAM | TYMED_HGLOBAL | TYMED_ISTORAGE), DWORD(TYMED_ISTREAM), DWORD(TYMED_HGLOBAL), DWORD(TYMED_ISTORAGE) })
            {
                FORMATETC fmt{ kContents, nullptr, DVASPECT_CONTENT, item.index, tymed };
                hr = m_data->GetData(&fmt, &medium);
                if (SUCCEEDED(hr))
                    break;
            }
            if (SUCCEEDED(hr))
            {
                auto release = wil::scope_exit([&] { ReleaseStgMedium(&medium); });
                if (medium.tymed == TYMED_ISTORAGE && medium.pstg)
                    hr = WriteStorage(medium.pstg, path);
                else if ((medium.tymed == TYMED_ISTREAM && medium.pstm) || (medium.tymed == TYMED_HGLOBAL && medium.hGlobal))
                {
                    wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
                    if (!file)
                        hr = HRESULT_FROM_WIN32(GetLastError());
                    else if (medium.tymed == TYMED_ISTREAM)
                        hr = WriteStream(medium.pstm, file.get(), done, report);
                    else
                        hr = WriteMemory(medium.hGlobal, item, file.get(), done, report);
                }
                else
                    hr = DV_E_TYMED;
            }
            if (FAILED(hr) && i < m_fallback.size() && m_items.size() == m_fallback.size())
            {
                // The contents were not handed over, but the drag also gave a path to a copy of them.
                DeleteFileW(path.c_str());
                if (CopyFileW(m_fallback[i].c_str(), path.c_str(), TRUE))
                    hr = S_OK;
            }
            if (SUCCEEDED(hr))
            {
                m_savedPaths.push_back(path);
                log::Info(L"dropped: saved {}", path);
                continue;
            }
            DeleteFileW(path.c_str());   // whatever part was written
            log::Warn(L"dropped: could not save '{}' ({})", item.name, HexCode(hr));
            if (error.empty())
                error = std::format(L"{}: the app it came from did not hand it over ({})", FileName(path), HexCode(hr));
        }
        return m_savedPaths;
    }

    HRESULT KeepVirtualFile(const std::wstring& file, const std::wstring& folder, std::wstring& kept)
    {
        const std::filesystem::path src(file);
        const std::filesystem::path dir(folder);
        // First under a new "~name.xxxxxxxx.sdpart" there (a copy when it is on another drive), then
        // renamed to the first free name, so a half-copied file never has a final name.
        const auto staging = (dir / StagingName(src.stem().wstring())).wstring();
        if (!MoveFileExW(file.c_str(), staging.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
            return HRESULT_FROM_WIN32(GetLastError());
        // Always "name (2).ext", whatever the output naming style: this is the user's own file.
        const HRESULT hr = RenameToFreeName(staging, OutputNaming::Parentheses, src.stem().wstring(), L"", src.extension().wstring(), kept);
        if (FAILED(hr))
            DeleteFileW(staging.c_str());
        return hr;
    }
}
