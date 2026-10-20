#include "FileMove.h"
#include "Log.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <wil/resource.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <format>

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace sd
{
    namespace
    {
        // The dialog remembers its last folder per client GUID, so "Move to…" reopens where you moved last.
        // {6B1F8E2A-3C47-4D0B-9E55-2A7C1D9F4B63}
        constexpr GUID kMoveDialogGuid = { 0x6b1f8e2a, 0x3c47, 0x4d0b, { 0x9e, 0x55, 0x2a, 0x7c, 0x1d, 0x9f, 0x4b, 0x63 } };

        bool SamePath(const std::wstring& a, const std::wstring& b)
        {
            // NTFS names are case-insensitive; ordinal comparison avoids locale surprises.
            return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
        }

        bool IsInside(const std::wstring& child, const std::wstring& parent)
        {
            auto prefix = parent;
            if (!prefix.ends_with(L'\\'))
                prefix += L'\\';
            return child.size() >= prefix.size() && SamePath(child.substr(0, prefix.size()), prefix);
        }

        // The shell reports long paths ("C:\Users\runneradmin\..."), while a path we were handed can use
        // 8.3 short names ("C:\Users\RUNNER~1\..."); compare in the long form. Only works while the item exists.
        std::wstring LongPath(const std::wstring& path)
        {
            const DWORD n = GetLongPathNameW(path.c_str(), nullptr, 0);
            if (n == 0)
                return path;
            std::wstring out(n, L'\0');
            const DWORD written = GetLongPathNameW(path.c_str(), out.data(), n);
            if (written == 0 || written >= n)
                return path;
            out.resize(written);
            return out;
        }

        std::wstring PathOf(IShellItem* item)
        {
            wil::unique_cotaskmem_string path;
            if (item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                return path.get();
            return {};
        }

        // Collects where each item ended up. With FOF_RENAMEONCOLLISION the shell may pick "name (2)",
        // and PostMoveItem is the only place that reports the final name. For folders moved across drives
        // it is also called for every file inside; the caller keeps only the top-level items it asked for.
        class MoveSink final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IFileOperationProgressSink>
        {
        public:
            struct Entry
            {
                std::wstring source;
                std::wstring destination;
                HRESULT hr = S_OK;
            };
            std::vector<Entry> entries;

            IFACEMETHODIMP PostMoveItem(DWORD, IShellItem* item, IShellItem*, LPCWSTR, HRESULT hrMove, IShellItem* created) override
            {
                entries.push_back(Entry{ PathOf(item), PathOf(created), hrMove });
                return S_OK;
            }

            IFACEMETHODIMP StartOperations() override { return S_OK; }
            IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
            IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
            IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
            IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
            IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
            IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
            IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
            IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }
            IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
            IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
            IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
            IFACEMETHODIMP ResetTimer() override { return S_OK; }
            IFACEMETHODIMP PauseTimer() override { return S_OK; }
            IFACEMETHODIMP ResumeTimer() override { return S_OK; }
        };

        // A process that did not receive the last input event cannot take the foreground, and the drop
        // came through Explorer's drag loop, not our input queue. Attaching to the foreground thread's
        // input for a moment lets the folder picker open in front instead of flashing in the taskbar.
        void BringToFront(HWND hwnd)
        {
            const HWND fg = GetForegroundWindow();
            const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
            const DWORD self = GetCurrentThreadId();
            const bool attach = fgThread && fgThread != self && AttachThreadInput(self, fgThread, TRUE);
            SetForegroundWindow(hwnd);
            if (attach)
                AttachThreadInput(self, fgThread, FALSE);
        }

        std::wstring HrText(HRESULT hr)
        {
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == COPYENGINE_E_USER_CANCELLED)
                return L"cancelled";
            wil::unique_hlocal_string text;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                           static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
            std::wstring s = text ? text.get() : L"";
            while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.'))
                s.pop_back();
            return s.empty() ? std::format(L"error 0x{:08X}", static_cast<unsigned>(hr)) : s;
        }
    }

    bool PickDestinationFolder(HWND owner, size_t itemCount, std::wstring& folder)
    {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return false;
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        dialog->SetClientGuid(kMoveDialogGuid);
        const auto title = itemCount == 1 ? std::wstring(L"Move 1 item to") : std::format(L"Move {} items to", itemCount);
        dialog->SetTitle(title.c_str());
        dialog->SetOkButtonLabel(L"Move here");

        BringToFront(owner);
        if (FAILED(dialog->Show(owner)))   // HRESULT_FROM_WIN32(ERROR_CANCELLED) when the user closes it
            return false;
        ComPtr<IShellItem> result;
        if (FAILED(dialog->GetResult(&result)))
            return false;
        folder = PathOf(result.Get());
        return !folder.empty();
    }

    std::vector<MoveOutcome> MoveToFolder(const Recipe& recipe, const std::vector<std::wstring>& sources,
                                          const std::wstring& folderIn, HistoryStore* history, int64_t& runId)
    {
        const auto started = std::chrono::steady_clock::now();
        std::vector<MoveOutcome> outcomes(sources.size());
        std::vector<int64_t> itemIds(sources.size(), 0);
        std::vector<std::wstring> longSources(sources.size());
        const std::wstring longFolder = LongPath(folderIn);
        const std::wstring& folder = longFolder;
        runId = history ? history->BeginRun(recipe, static_cast<int>(sources.size())) : 0;

        ComPtr<IFileOperation> op;
        ComPtr<IShellItem> destination;
        HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
        if (SUCCEEDED(hr))
        {
            // No FOF_NOCONFIRMATION on purpose: should the shell ever want to ask (it shouldn't, with
            // RENAMEONCOLLISION), asking beats answering "yes" to a question we did not foresee.
            // FOF_ALLOWUNDO also puts the move on Explorer's own Ctrl+Z stack.
            hr = op->SetOperationFlags(FOF_ALLOWUNDO | FOF_RENAMEONCOLLISION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_SILENT);
        }
        if (SUCCEEDED(hr))
            hr = SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&destination));

        size_t queued = 0;
        for (size_t i = 0; i < sources.size(); ++i)
        {
            auto& out = outcomes[i];
            out.source = sources[i];
            if (history)
                itemIds[i] = history->BeginItem(runId, out.source, L"");
            longSources[i] = LongPath(out.source);
            const auto parent = std::filesystem::path(longSources[i]).parent_path().wstring();
            if (SamePath(parent, folder))
            {
                out.skipped = true;
                out.error = L"already in that folder";
                continue;
            }
            if (SamePath(longSources[i], folder) || IsInside(folder, longSources[i]))
            {
                out.error = L"a folder can't be moved into itself";
                continue;
            }
            if (FAILED(hr))
            {
                out.error = HrText(hr);
                continue;
            }
            ComPtr<IShellItem> item;
            const HRESULT itemHr = SHCreateItemFromParsingName(out.source.c_str(), nullptr, IID_PPV_ARGS(&item));
            if (SUCCEEDED(itemHr) && SUCCEEDED(op->MoveItem(item.Get(), destination.Get(), nullptr, nullptr)))
                ++queued;
            else
                out.error = HrText(FAILED(itemHr) ? itemHr : E_FAIL);
        }

        if (queued > 0)
        {
            auto sink = Microsoft::WRL::Make<MoveSink>();
            DWORD cookie = 0;
            const bool advised = sink && SUCCEEDED(op->Advise(sink.Get(), &cookie));
            const HRESULT performed = op->PerformOperations();
            if (advised)
                op->Unadvise(cookie);

            for (size_t i = 0; i < outcomes.size(); ++i)
            {
                auto& out = outcomes[i];
                if (out.skipped || !out.error.empty())
                    continue;
                bool reported = false;
                for (const auto& e : sink ? sink->entries : std::vector<MoveSink::Entry>{})
                {
                    if (!SamePath(e.source, longSources[i]))
                        continue;   // a file inside a moved folder
                    reported = true;
                    if (SUCCEEDED(e.hr) && !e.destination.empty())
                    {
                        out.ok = true;
                        out.destination = e.destination;
                    }
                    else
                    {
                        out.error = HrText(FAILED(e.hr) ? e.hr : E_FAIL);
                    }
                }
                if (!reported)
                    out.error = HrText(FAILED(performed) ? performed : HRESULT_FROM_WIN32(ERROR_CANCELLED));
            }
        }

        const auto ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
        for (size_t i = 0; i < outcomes.size(); ++i)
        {
            const auto& out = outcomes[i];
            if (out.ok)
                log::Info(L"move: {} -> {}", out.source, out.destination);
            else
                log::Warn(L"move: {} {}: {}", out.source, out.skipped ? L"skipped" : L"failed", out.error);
            if (!history)
                continue;
            uint64_t size = 0;
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (out.ok && GetFileAttributesExW(out.destination.c_str(), GetFileExInfoStandard, &data) &&
                !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            // No hash: undoing a move never deletes anything, so an edited file can safely go back.
            history->FinishItem(itemIds[i], out.ok ? status::Done : out.skipped ? status::Skipped : status::Failed,
                                out.destination, size, "", out.error, ms);
        }
        if (history)
            history->FinishRun(runId);
        return outcomes;
    }

    bool IsMoveRun(const RunRecord& run)
    {
        const auto json = nlohmann::json::parse(run.recipeJson, nullptr, false);
        if (json.is_discarded() || !json.contains("steps") || !json["steps"].is_array() || json["steps"].size() != 1)
            return false;
        const auto& step = json["steps"][0];
        return step.is_object() && step.value("action", std::string{}) == "file.move";
    }
}
