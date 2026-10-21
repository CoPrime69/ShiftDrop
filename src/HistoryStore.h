#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace sd
{
    struct Recipe;

    // Status strings stored in the database. Kept as text so the file is readable with any SQLite tool.
    namespace status
    {
        inline constexpr const char* Running = "running";
        inline constexpr const char* Done = "done";
        inline constexpr const char* Skipped = "skipped";
        inline constexpr const char* Failed = "failed";
        inline constexpr const char* Interrupted = "interrupted";   // app died mid-job; staging file removed at next start
        inline constexpr const char* Undone = "undone";
        inline constexpr const char* Partial = "partial";           // run: some items done, some failed/skipped
    }

    struct ItemRecord
    {
        int64_t id = 0;
        int64_t runId = 0;
        std::wstring source;
        std::wstring staging;
        std::wstring output;
        uint64_t outputSize = 0;
        std::string outputSha256;
        std::string status;
        std::wstring error;
        int64_t durationMs = 0;
    };

    struct RunRecord
    {
        int64_t id = 0;
        std::wstring started;        // local time, "2026-10-20 14:31:07"
        std::string recipeId;
        std::wstring recipeLabel;
        std::string recipeJson;      // snapshot of the recipe as it ran
        std::string status;
        int items = 0;
        int done = 0;
        std::vector<std::wstring> files;   // the sources' names (no folder), for History's search
    };

    // Run history in SQLite (%LOCALAPPDATA%\ShiftDrop\history.db). Thread-safe: workers record item
    // outcomes while the UI thread reads. Every call is short; nothing holds the lock across I/O
    // other than SQLite's own.
    class HistoryStore
    {
    public:
        static std::unique_ptr<HistoryStore> Open(const std::wstring& path, std::wstring& error);
        ~HistoryStore();
        HistoryStore(const HistoryStore&) = delete;
        HistoryStore& operator=(const HistoryStore&) = delete;

        int64_t BeginRun(const Recipe& recipe, int itemCount);
        // Recorded *before* the staging file is written, so a crash can be cleaned up at next start.
        int64_t BeginItem(int64_t runId, const std::wstring& source, const std::wstring& staging);
        void FinishItem(int64_t itemId, const char* itemStatus, const std::wstring& output, uint64_t size,
                        const std::string& sha256, const std::wstring& error, int64_t durationMs);
        void FinishRun(int64_t runId);   // derives done / partial / failed from its items

        std::vector<RunRecord> RecentRuns(int limit);
        std::vector<ItemRecord> Items(int64_t runId);
        void SetItemStatus(int64_t itemId, const char* itemStatus);
        void SetRunStatus(int64_t runId, const char* runStatus);

        // History's Clear history: every finished run and its items leave the list (and the file).
        // Only the records go: no file is touched, and outputs that exist stay. A cleared run can no
        // longer be undone from History. Runs still going keep their rows, as a crash mid-run needs
        // them to remove its staging files at the next start. Returns how many runs went.
        int Clear();

        // Items still "running" from a previous session: marks them (and their runs) interrupted and
        // returns them so the caller can delete leftover staging files.
        std::vector<ItemRecord> RecoverInterrupted();

        struct Key { explicit Key() = default; };   // passkey: only Open() can construct, yet make_unique works
        HistoryStore(Key, sqlite3* db) : m_db(db) {}

    private:
        bool Exec(const char* sql);
        std::mutex m_mutex;
        sqlite3* m_db;
    };
}
