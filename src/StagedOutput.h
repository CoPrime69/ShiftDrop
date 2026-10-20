#pragma once
#include "HistoryStore.h"

#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace sd
{
    struct ItemResult
    {
        bool ok = false;
        bool skipped = false;
        std::wstring output;
        std::wstring error;
        uint64_t size = 0;
        std::wstring note;   // optional line for the result panel, e.g. the loudness before and after
    };

    // One output of one source, written the safe way (OutputPath.h, ADR 0003) and recorded in
    // history: a staging file (or folder) next to the source, then a no-replace rename to
    // "name (suffix).ext", then its hash for Undo. Every engine writes through this, so a crash
    // never leaves a half-written file under a final name and every result can be undone.
    //
    //   StagedOutput out(history, runId, source);
    //   if (!out.Ready()) return out.Fail(out.Error());
    //   ... write out.Path() ...
    //   return out.Commit(L"converted", L".pdf");
    //
    // A folder output (frames of a video, pages of a PDF) passes `folder = true`: Path() is then a new
    // empty staging folder to fill. Folders are not hashed; Undo moves them to the Recycle Bin as is.
    // The destructor deletes the staging file or folder unless Commit succeeded.
    class StagedOutput
    {
    public:
        StagedOutput(HistoryStore* history, int64_t runId, const std::wstring& source, bool folder = false);
        ~StagedOutput();
        StagedOutput(const StagedOutput&) = delete;
        StagedOutput& operator=(const StagedOutput&) = delete;

        bool Ready() const { return !m_path.empty(); }
        const std::wstring& Error() const { return m_error; }
        const std::wstring& Path() const { return m_path; }   // staging path to write

        // Renames into place, hashes, records Done. On a rename error returns a failed result.
        ItemResult Commit(std::wstring_view suffix, std::wstring_view extension);
        // Records the item as failed (or skipped when `cancelled`) and removes the staging output.
        ItemResult Fail(std::wstring error, bool cancelled = false);

    private:
        void Discard();

        HistoryStore* m_history;
        std::wstring m_source;
        bool m_folder;
        std::wstring m_path;
        std::wstring m_error;
        int64_t m_item = 0;
        ULONGLONG m_started;
        bool m_done = false;
    };

    // Deletes a staging file or folder left by a crash (PipelineRunner::RecoverInterrupted).
    bool RemoveStaging(const std::wstring& path);
}
