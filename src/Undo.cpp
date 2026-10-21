#include "Undo.h"
#include "FileMove.h"
#include "Hash.h"
#include "Log.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <filesystem>
#include <format>

using Microsoft::WRL::ComPtr;

namespace sd
{
    namespace
    {
        ComPtr<IFileOperation> NewOperation(DWORD flags, HWND owner, HRESULT& hr)
        {
            ComPtr<IFileOperation> op;
            hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
            if (SUCCEEDED(hr))
                hr = op->SetOperationFlags(flags);
            if (SUCCEEDED(hr))
                op->SetOwnerWindow(owner);
            return op;
        }

        HRESULT Perform(IFileOperation* op)
        {
            HRESULT hr = op->PerformOperations();
            BOOL aborted = FALSE;
            if (SUCCEEDED(hr) && SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && aborted)
                hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            return hr;
        }

        bool SamePathNoCase(const std::wstring& a, const std::wstring& b)
        {
            return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
        }

        // Also undoes a rename: the item moves back into the folder it is already in, under its old name.
        UndoReport UndoMove(HistoryStore& history, const RunRecord& run, HWND owner, bool rename)
        {
            UndoReport report;
            const int64_t runId = run.id;
            std::vector<const ItemRecord*> toMove;
            const auto items = history.Items(runId);
            for (const auto& item : items)
            {
                if (item.status != status::Done || item.output.empty())
                    continue;
                if (GetFileAttributesW(item.output.c_str()) == INVALID_FILE_ATTRIBUTES)
                    ++report.alreadyGone;
                // A rename that only changed the case finds itself at the old name: not in the way.
                else if (!SamePathNoCase(item.source, item.output) && GetFileAttributesW(item.source.c_str()) != INVALID_FILE_ATTRIBUTES)
                    report.occupied.push_back(item.source);
                else
                    toMove.push_back(&item);
            }
            if (!report.occupied.empty())
            {
                report.message = std::format(L"Not undone: there is a new {} where the original was.",
                                             std::filesystem::path(report.occupied.front()).filename().wstring());
                log::Warn(L"undo: move run {} refused, {} original location(s) taken", runId, report.occupied.size());
                return report;
            }

            HRESULT hr = S_OK;
            if (!toMove.empty())
            {
                // No FOF_NOCONFIRMATION and no RENAMEONCOLLISION: if a name appears between our check and
                // the move, the shell asks instead of overwriting or putting the item back under a new name.
                auto op = NewOperation(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_SILENT, owner, hr);
                for (const auto* item : toMove)
                {
                    if (FAILED(hr))
                        break;
                    const std::filesystem::path original(item->source);
                    const auto parent = original.parent_path().wstring();
                    // The original folder may have been deleted since; recreate it (FOF_NOCONFIRMMKDIR).
                    SHCreateDirectoryExW(nullptr, parent.c_str(), nullptr);
                    ComPtr<IShellItem> shellItem, parentItem;
                    hr = SHCreateItemFromParsingName(item->output.c_str(), nullptr, IID_PPV_ARGS(&shellItem));
                    if (SUCCEEDED(hr))
                        hr = SHCreateItemFromParsingName(parent.c_str(), nullptr, IID_PPV_ARGS(&parentItem));
                    if (SUCCEEDED(hr))
                        hr = rename ? op->RenameItem(shellItem.Get(), original.filename().c_str(), nullptr)
                                    : op->MoveItem(shellItem.Get(), parentItem.Get(), original.filename().c_str(), nullptr);
                }
                if (SUCCEEDED(hr))
                    hr = Perform(op.Get());
            }
            if (FAILED(hr))
            {
                report.message = std::format(L"Undo failed (0x{:08X}).", static_cast<unsigned>(hr));
                log::Error(L"undo: move run {} failed 0x{:08X}", runId, static_cast<unsigned>(hr));
                return report;
            }
            for (const auto* item : toMove)
                history.SetItemStatus(item->id, status::Undone);
            history.SetRunStatus(runId, status::Undone);
            report.ok = true;
            report.movedBack = static_cast<int>(toMove.size());
            const auto done = rename ? std::format(L"Gave {} item(s) their old names back", report.movedBack)
                                     : std::format(L"Moved {} item(s) back", report.movedBack);
            report.message = done + (report.alreadyGone ? std::format(L" ({} were no longer there).", report.alreadyGone) : std::wstring(L"."));
            log::Info(L"undo: {} run {}: {}", rename ? L"rename" : L"move", runId, report.message);
            return report;
        }
    }

    UndoReport UndoRun(HistoryStore& history, const RunRecord& run, HWND owner)
    {
        if (IsMoveRun(run) || IsRenameRun(run))
            return UndoMove(history, run, owner, IsRenameRun(run));
        const int64_t runId = run.id;
        UndoReport report;
        const auto items = history.Items(runId);
        std::vector<const ItemRecord*> toRecycle;

        for (const auto& item : items)
        {
            if (item.status != status::Done || item.output.empty())
                continue;
            if (GetFileAttributesW(item.output.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                ++report.alreadyGone;
                continue;
            }
            // Hashing each output is the honest check (a timestamp can be preserved by an editor).
            // For M2's image outputs this is fast; large video outputs will move it off the UI thread.
            // Folder outputs (frames, pages) carry no hash; they are recycled as they are.
            if (!item.outputSha256.empty() && Sha256File(item.output) != item.outputSha256)
            {
                report.modified.push_back(item.output);
                continue;
            }
            toRecycle.push_back(&item);
        }

        if (!report.modified.empty())
        {
            report.message = std::format(L"Not undone: {} was changed after ShiftDrop created it.",
                                         std::filesystem::path(report.modified.front()).filename().wstring());
            log::Warn(L"undo: run {} refused, {} output(s) modified", runId, report.modified.size());
            return report;
        }

        if (!toRecycle.empty())
        {
            // FOFX_RECYCLEONDELETE (with FOF_ALLOWUNDO): delete means "move to Recycle Bin", never a hard delete.
            HRESULT hr = S_OK;
            auto op = NewOperation(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOFX_RECYCLEONDELETE, owner, hr);
            for (const auto* item : toRecycle)
            {
                if (FAILED(hr))
                    break;
                ComPtr<IShellItem> shellItem;
                hr = SHCreateItemFromParsingName(item->output.c_str(), nullptr, IID_PPV_ARGS(&shellItem));
                if (SUCCEEDED(hr))
                    hr = op->DeleteItem(shellItem.Get(), nullptr);
            }
            if (SUCCEEDED(hr))
                hr = Perform(op.Get());
            if (FAILED(hr))
            {
                report.message = std::format(L"Undo failed (0x{:08X}).", static_cast<unsigned>(hr));
                log::Error(L"undo: run {} failed 0x{:08X}", runId, static_cast<unsigned>(hr));
                return report;
            }
        }

        for (const auto* item : toRecycle)
            history.SetItemStatus(item->id, status::Undone);
        history.SetRunStatus(runId, status::Undone);
        report.ok = true;
        report.recycled = static_cast<int>(toRecycle.size());
        report.message = report.alreadyGone
                             ? std::format(L"Moved {} file(s) to the Recycle Bin ({} were already gone).", report.recycled, report.alreadyGone)
                             : std::format(L"Moved {} file(s) to the Recycle Bin.", report.recycled);
        log::Info(L"undo: run {}: {}", runId, report.message);
        return report;
    }
}
