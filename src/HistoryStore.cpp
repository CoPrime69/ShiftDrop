#include "HistoryStore.h"
#include "Log.h"
#include "Recipe.h"
#include "Utf.h"

#include <sqlite3.h>
#include <windows.h>

#include <format>

namespace sd
{
    namespace
    {
        // Minimal RAII statement wrapper: prepare, bind, step, finalize.
        class Statement
        {
        public:
            Statement(sqlite3* db, const char* sql)
            {
                if (sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr) != SQLITE_OK)
                    log::Error(L"history: prepare failed: {} ({})", Widen(sqlite3_errmsg(db)), Widen(sql));
            }
            ~Statement() { sqlite3_finalize(m_stmt); }
            Statement(const Statement&) = delete;
            Statement& operator=(const Statement&) = delete;

            Statement& Bind(int i, int64_t v) { sqlite3_bind_int64(m_stmt, i, v); return *this; }
            Statement& Bind(int i, const std::string& v) { sqlite3_bind_text(m_stmt, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); return *this; }
            Statement& Bind(int i, const std::wstring& v) { return Bind(i, Narrow(v)); }
            Statement& Bind(int i, const char* v) { return Bind(i, std::string(v)); }
            bool Step() { return m_stmt && sqlite3_step(m_stmt) == SQLITE_ROW; }
            bool Run() { return m_stmt && sqlite3_step(m_stmt) == SQLITE_DONE; }
            int64_t Int(int col) const { return sqlite3_column_int64(m_stmt, col); }
            std::string Text(int col) const
            {
                const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt, col));
                return p ? std::string(p) : std::string();
            }
            std::wstring WText(int col) const { return Widen(Text(col)); }

        private:
            sqlite3_stmt* m_stmt = nullptr;
        };

        std::string NowLocal()
        {
            SYSTEMTIME t{};
            GetLocalTime(&t);
            return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        }

        constexpr const char* kSchema = R"(
            PRAGMA journal_mode = WAL;
            PRAGMA foreign_keys = ON;
            CREATE TABLE IF NOT EXISTS runs (
                id           INTEGER PRIMARY KEY,
                started      TEXT NOT NULL,
                finished     TEXT,
                recipe_id    TEXT NOT NULL,
                recipe_label TEXT NOT NULL,
                recipe_json  TEXT NOT NULL,
                item_count   INTEGER NOT NULL,
                status       TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS items (
                id            INTEGER PRIMARY KEY,
                run_id        INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
                source        TEXT NOT NULL,
                staging       TEXT,
                output        TEXT,
                output_size   INTEGER,
                output_sha256 TEXT,
                status        TEXT NOT NULL,
                error         TEXT,
                duration_ms   INTEGER
            );
            CREATE INDEX IF NOT EXISTS items_run ON items(run_id);
            CREATE INDEX IF NOT EXISTS items_status ON items(status);
            PRAGMA user_version = 1;
        )";
    }

    std::unique_ptr<HistoryStore> HistoryStore::Open(const std::wstring& path, std::wstring& error)
    {
        sqlite3* db = nullptr;
        // FULLMUTEX: SQLite serialises use of the connection across threads; m_mutex additionally keeps
        // multi-statement operations (FinishRun) atomic with respect to each other.
        const int rc = sqlite3_open_v2(Narrow(path).c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK)
        {
            error = Widen(db ? sqlite3_errmsg(db) : "out of memory");
            sqlite3_close(db);
            return nullptr;
        }
        sqlite3_busy_timeout(db, 2000);
        auto store = std::make_unique<HistoryStore>(Key{}, db);
        if (!store->Exec(kSchema))
        {
            error = Widen(sqlite3_errmsg(db));
            return nullptr;
        }
        return store;
    }

    HistoryStore::~HistoryStore() { sqlite3_close(m_db); }

    bool HistoryStore::Exec(const char* sql)
    {
        char* message = nullptr;
        if (sqlite3_exec(m_db, sql, nullptr, nullptr, &message) != SQLITE_OK)
        {
            log::Error(L"history: {}", Widen(message ? message : "error"));
            sqlite3_free(message);
            return false;
        }
        return true;
    }

    int64_t HistoryStore::BeginRun(const Recipe& recipe, int itemCount)
    {
        std::scoped_lock lock(m_mutex);
        Statement s(m_db, "INSERT INTO runs(started, recipe_id, recipe_label, recipe_json, item_count, status) VALUES(?,?,?,?,?,?)");
        s.Bind(1, NowLocal()).Bind(2, recipe.id).Bind(3, recipe.label).Bind(4, recipe.ToJson()).Bind(5, int64_t{ itemCount }).Bind(6, status::Running).Run();
        return sqlite3_last_insert_rowid(m_db);
    }

    int64_t HistoryStore::BeginItem(int64_t runId, const std::wstring& source, const std::wstring& staging)
    {
        std::scoped_lock lock(m_mutex);
        Statement s(m_db, "INSERT INTO items(run_id, source, staging, status) VALUES(?,?,?,?)");
        s.Bind(1, runId).Bind(2, source).Bind(3, staging).Bind(4, status::Running).Run();
        return sqlite3_last_insert_rowid(m_db);
    }

    void HistoryStore::FinishItem(int64_t itemId, const char* itemStatus, const std::wstring& output, uint64_t size,
                                  const std::string& sha256, const std::wstring& error, int64_t durationMs)
    {
        std::scoped_lock lock(m_mutex);
        Statement s(m_db, "UPDATE items SET status=?, output=?, output_size=?, output_sha256=?, error=?, duration_ms=?, staging=NULL WHERE id=?");
        s.Bind(1, itemStatus).Bind(2, output).Bind(3, static_cast<int64_t>(size)).Bind(4, sha256).Bind(5, error).Bind(6, durationMs).Bind(7, itemId).Run();
    }

    void HistoryStore::FinishRun(int64_t runId)
    {
        std::scoped_lock lock(m_mutex);
        int64_t total = 0, done = 0;
        {
            Statement s(m_db, "SELECT COUNT(*), SUM(status='done') FROM items WHERE run_id=?");
            s.Bind(1, runId);
            if (s.Step())
            {
                total = s.Int(0);
                done = s.Int(1);
            }
        }
        const char* runStatus = done == 0 ? status::Failed : (done == total ? status::Done : status::Partial);
        Statement u(m_db, "UPDATE runs SET status=?, finished=? WHERE id=?");
        u.Bind(1, runStatus).Bind(2, NowLocal()).Bind(3, runId).Run();
    }

    std::vector<RunRecord> HistoryStore::RecentRuns(int limit)
    {
        std::scoped_lock lock(m_mutex);
        std::vector<RunRecord> runs;
        Statement s(m_db, R"(SELECT r.id, r.started, r.recipe_id, r.recipe_label, r.recipe_json, r.status,
                                    (SELECT COUNT(*) FROM items i WHERE i.run_id = r.id),
                                    (SELECT COUNT(*) FROM items i WHERE i.run_id = r.id AND i.status = 'done')
                             FROM runs r ORDER BY r.id DESC LIMIT ?)");
        s.Bind(1, int64_t{ limit });
        while (s.Step())
        {
            RunRecord r;
            r.id = s.Int(0);
            r.started = s.WText(1);
            r.recipeId = s.Text(2);
            r.recipeLabel = s.WText(3);
            r.recipeJson = s.Text(4);
            r.status = s.Text(5);
            r.items = static_cast<int>(s.Int(6));
            r.done = static_cast<int>(s.Int(7));
            runs.push_back(std::move(r));
        }
        return runs;
    }

    std::vector<ItemRecord> HistoryStore::Items(int64_t runId)
    {
        std::scoped_lock lock(m_mutex);
        std::vector<ItemRecord> items;
        Statement s(m_db, "SELECT id, run_id, source, staging, output, output_size, output_sha256, status, error, duration_ms FROM items WHERE run_id=? ORDER BY id");
        s.Bind(1, runId);
        while (s.Step())
        {
            ItemRecord i;
            i.id = s.Int(0);
            i.runId = s.Int(1);
            i.source = s.WText(2);
            i.staging = s.WText(3);
            i.output = s.WText(4);
            i.outputSize = static_cast<uint64_t>(s.Int(5));
            i.outputSha256 = s.Text(6);
            i.status = s.Text(7);
            i.error = s.WText(8);
            i.durationMs = s.Int(9);
            items.push_back(std::move(i));
        }
        return items;
    }

    void HistoryStore::SetItemStatus(int64_t itemId, const char* itemStatus)
    {
        std::scoped_lock lock(m_mutex);
        Statement s(m_db, "UPDATE items SET status=? WHERE id=?");
        s.Bind(1, itemStatus).Bind(2, itemId).Run();
    }

    void HistoryStore::SetRunStatus(int64_t runId, const char* runStatus)
    {
        std::scoped_lock lock(m_mutex);
        Statement s(m_db, "UPDATE runs SET status=? WHERE id=?");
        s.Bind(1, runStatus).Bind(2, runId).Run();
    }

    std::vector<ItemRecord> HistoryStore::RecoverInterrupted()
    {
        std::scoped_lock lock(m_mutex);
        std::vector<ItemRecord> items;
        {
            Statement s(m_db, "SELECT id, run_id, source, staging FROM items WHERE status='running'");
            while (s.Step())
            {
                ItemRecord i;
                i.id = s.Int(0);
                i.runId = s.Int(1);
                i.source = s.WText(2);
                i.staging = s.WText(3);
                items.push_back(std::move(i));
            }
        }
        Exec("UPDATE items SET status='interrupted', staging=NULL WHERE status='running';"
             "UPDATE runs SET status='interrupted', finished=COALESCE(finished, started) WHERE status='running';");
        return items;
    }
}
