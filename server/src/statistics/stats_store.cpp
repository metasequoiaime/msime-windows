#include "statistics/stats_store.h"

#include "engine/core/data_path.h"

#include <sqlite3.h>

#include <cstdlib>

namespace MsimeStats
{
namespace
{
const char *kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS stats_daily(
  day_key INTEGER PRIMARY KEY,
  cjk INTEGER NOT NULL DEFAULT 0,
  latin INTEGER NOT NULL DEFAULT 0,
  digit INTEGER NOT NULL DEFAULT 0,
  punct INTEGER NOT NULL DEFAULT 0,
  other INTEGER NOT NULL DEFAULT 0,
  active_ms INTEGER NOT NULL DEFAULT 0,
  first_word_hits INTEGER NOT NULL DEFAULT 0,
  first_word_total INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS stats_hourly(
  day_key INTEGER NOT NULL,
  hour INTEGER NOT NULL,
  chars INTEGER NOT NULL DEFAULT 0,
  active_ms INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(day_key, hour));
CREATE TABLE IF NOT EXISTS stats_meta(
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS stats_honor(
  id TEXT PRIMARY KEY,
  unlocked_day_key INTEGER NOT NULL);
)SQL";

const char *kUpsertDailySql =
    "INSERT INTO stats_daily(day_key, cjk, latin, digit, punct, other, active_ms) "
    "VALUES(?, ?, ?, ?, ?, ?, ?) "
    "ON CONFLICT(day_key) DO UPDATE SET "
    "cjk = cjk + excluded.cjk, latin = latin + excluded.latin, digit = digit + excluded.digit, "
    "punct = punct + excluded.punct, other = other + excluded.other, "
    "active_ms = active_ms + excluded.active_ms";

// Incremental first-word counters only: the character counters must not be
// touched (they are driven by the stats pipe, this one by the selection path).
// Inserting zeros seeds a fresh row; the conflict branch adds the deltas.
const char *kUpsertFirstWordSql =
    "INSERT INTO stats_daily(day_key, cjk, latin, digit, punct, other, active_ms, first_word_hits, first_word_total) "
    "VALUES(?, 0, 0, 0, 0, 0, 0, ?, ?) "
    "ON CONFLICT(day_key) DO UPDATE SET "
    "first_word_hits = first_word_hits + excluded.first_word_hits, "
    "first_word_total = first_word_total + excluded.first_word_total";

const char *kUpsertHourlySql = "INSERT INTO stats_hourly(day_key, hour, chars, active_ms) "
                               "VALUES(?, ?, ?, ?) "
                               "ON CONFLICT(day_key, hour) DO UPDATE SET "
                               "chars = chars + excluded.chars, active_ms = active_ms + excluded.active_ms";

std::string SqliteMessage(sqlite3 *db)
{
    return db != nullptr ? std::string(sqlite3_errmsg(db)) : std::string("sqlite handle is null");
}

std::string Describe(const char *what, sqlite3 *db)
{
    return std::string("stats store: ") + what + ": " + SqliteMessage(db);
}
} // namespace

bool ParseRetention(const std::string &value, Retention &out)
{
    if (value == "30d")
    {
        out = Retention::Days30;
    }
    else if (value == "90d")
    {
        out = Retention::Days90;
    }
    else if (value == "180d")
    {
        out = Retention::Days180;
    }
    else if (value == "365d")
    {
        out = Retention::Days365;
    }
    else if (value == "forever")
    {
        out = Retention::Forever;
    }
    else
    {
        return false;
    }
    return true;
}

bool RetentionCutoff(Retention retention, int today_day_key, int &cutoff_day_key)
{
    const int days = static_cast<int>(retention);
    if (days <= 0)
    {
        return false;
    }
    cutoff_day_key = AddDays(today_day_key, -(days - 1));
    return true;
}

Store::Store(sqlite3 *db, std::filesystem::path path) : db_(db), path_(std::move(path))
{
}

Store::~Store()
{
    if (db_ != nullptr)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

std::unique_ptr<Store> Store::Open(const std::filesystem::path &path, std::string &error)
{
    // A UTF-8 path keeps SQLite on its own UTF-16 conversion; path::string()
    // would go through the ANSI code page and corrupt non-ASCII profile paths.
    const std::string utf8_path = metasequoia::path_to_utf8(path);
    sqlite3 *raw = nullptr;
    if (sqlite3_open_v2(utf8_path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
    {
        error = raw != nullptr ? SqliteMessage(raw) : std::string("stats store: unable to open database");
        if (raw != nullptr)
        {
            sqlite3_close(raw);
        }
        return nullptr;
    }

    std::unique_ptr<Store> store(new Store(raw, path));
    if (!store->Initialize(error))
    {
        return nullptr;
    }
    return store;
}

bool Store::Initialize(std::string &error)
{
    const char *pragmas[] = {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL", "PRAGMA busy_timeout=5000"};
    for (const char *pragma : pragmas)
    {
        if (!Exec(pragma, error))
        {
            return false;
        }
    }
    if (!Exec(kSchema, error))
    {
        return false;
    }
    // Schema v2 added the first-word hit-rate columns. kSchema lifts fresh
    // databases straight to v2; a v1 database (the shipped schema up to now,
    // with or without the stored version key, which v1 always wrote) needs its
    // stats_daily extended in place and the version stamp advanced.
    if (!ApplySchemaMigration(error))
    {
        return false;
    }
    return SetMeta(kStoreMetaSchemaVersion, kStoreSchemaVersion, error);
}

bool Store::ApplySchemaMigration(std::string &error)
{
    // The table shape is the source of truth, not the stored version: a stamp
    // written after a failed or half-applied migration must not hide missing
    // columns forever. Checking two columns is cheap enough for every open.
    const char *const columns[] = {kStoreColumnFirstWordHits, kStoreColumnFirstWordTotal};
    const auto has_all_columns = [this, &columns] {
        for (const char *column : columns)
        {
            if (!TableHasColumn("stats_daily", column))
            {
                return false;
            }
        }
        return true;
    };
    if (has_all_columns())
    {
        return true;
    }

    // ALTER TABLE ADD COLUMN has no IF NOT EXISTS form, and Settings and Server
    // can open a v1 database at the same time. Re-check under the write lock
    // and add only the missing columns in one transaction, so a crash or a
    // lost race leaves either the old shape or the new one, never a half.
    if (!BeginTransaction(error))
    {
        return false;
    }
    for (const char *column : columns)
    {
        if (TableHasColumn("stats_daily", column))
        {
            continue;
        }
        const std::string sql =
            std::string("ALTER TABLE stats_daily ADD COLUMN ") + column + " INTEGER NOT NULL DEFAULT 0";
        if (!Exec(sql.c_str(), error))
        {
            Rollback();
            return false;
        }
    }
    return Commit(error);
}

bool Store::Exec(const char *sql, std::string &error) const
{
    char *message = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &message) != SQLITE_OK)
    {
        error = message != nullptr ? std::string(message) : SqliteMessage(db_);
        sqlite3_free(message);
        return false;
    }
    return true;
}

bool Store::BeginTransaction(std::string &error)
{
    // BEGIN IMMEDIATE instead of a deferred transaction: the database is shared
    // between two processes, and a deferred transaction that only takes the
    // write lock at its first INSERT can fail with SQLITE_BUSY without the busy
    // handler having been able to wait.
    return Exec("BEGIN IMMEDIATE", error);
}

void Store::Rollback()
{
    std::string ignored;
    Exec("ROLLBACK", ignored);
}

bool Store::Commit(std::string &error)
{
    if (!Exec("COMMIT", error))
    {
        Rollback();
        return false;
    }
    return true;
}

bool Store::RefreshFirstDay(std::string &error)
{
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT MIN(day_key) FROM stats_daily", -1, &statement, nullptr) != SQLITE_OK)
    {
        error = Describe("read first_day", db_);
        return false;
    }

    int64_t minimum = 0;
    bool present = false;
    const int step = sqlite3_step(statement);
    if (step == SQLITE_ROW)
    {
        if (sqlite3_column_type(statement, 0) != SQLITE_NULL)
        {
            minimum = sqlite3_column_int64(statement, 0);
            present = true;
        }
    }
    else if (step != SQLITE_DONE)
    {
        error = Describe("read first_day", db_);
        sqlite3_finalize(statement);
        return false;
    }
    sqlite3_finalize(statement);

    if (!present)
    {
        return Exec("DELETE FROM stats_meta WHERE key = 'first_day'", error);
    }
    return WriteMeta(kStoreMetaFirstDay, std::to_string(minimum), error);
}

bool Store::WriteMeta(const std::string &key, const std::string &value, std::string &error)
{
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO stats_meta(key, value) VALUES(?, ?) "
                           "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                           -1, &statement, nullptr) != SQLITE_OK)
    {
        error = Describe("write meta", db_);
        return false;
    }
    sqlite3_bind_text(statement, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(statement) == SQLITE_DONE;
    if (!ok)
    {
        error = Describe("write meta", db_);
    }
    sqlite3_finalize(statement);
    return ok;
}

bool Store::DeleteMetaRow(const std::string &key, std::string &error)
{
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM stats_meta WHERE key = ?", -1, &statement, nullptr) != SQLITE_OK)
    {
        error = Describe("delete meta", db_);
        return false;
    }
    sqlite3_bind_text(statement, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(statement) == SQLITE_DONE;
    if (!ok)
    {
        error = Describe("delete meta", db_);
    }
    sqlite3_finalize(statement);
    return ok;
}

bool Store::Upsert(const Batch &batch, std::string &error)
{
    if (batch.Empty())
    {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!BeginTransaction(error))
    {
        return false;
    }

    bool ok = true;
    if (!batch.days.empty())
    {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(db_, kUpsertDailySql, -1, &statement, nullptr) != SQLITE_OK)
        {
            error = Describe("prepare daily upsert", db_);
            ok = false;
        }
        else
        {
            for (const DayDelta &day : batch.days)
            {
                sqlite3_bind_int64(statement, 1, day.day_key);
                sqlite3_bind_int64(statement, 2, day.cjk);
                sqlite3_bind_int64(statement, 3, day.latin);
                sqlite3_bind_int64(statement, 4, day.digit);
                sqlite3_bind_int64(statement, 5, day.punct);
                sqlite3_bind_int64(statement, 6, day.other);
                sqlite3_bind_int64(statement, 7, day.active_ms);
                if (sqlite3_step(statement) != SQLITE_DONE)
                {
                    error = Describe("write daily", db_);
                    ok = false;
                    break;
                }
                sqlite3_reset(statement);
            }
            sqlite3_finalize(statement);
        }
    }
    if (ok && !batch.hours.empty())
    {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(db_, kUpsertHourlySql, -1, &statement, nullptr) != SQLITE_OK)
        {
            error = Describe("prepare hourly upsert", db_);
            ok = false;
        }
        else
        {
            for (const HourDelta &hour : batch.hours)
            {
                sqlite3_bind_int64(statement, 1, hour.day_key);
                sqlite3_bind_int64(statement, 2, hour.hour);
                sqlite3_bind_int64(statement, 3, hour.chars);
                sqlite3_bind_int64(statement, 4, hour.active_ms);
                if (sqlite3_step(statement) != SQLITE_DONE)
                {
                    error = Describe("write hourly", db_);
                    ok = false;
                    break;
                }
                sqlite3_reset(statement);
            }
            sqlite3_finalize(statement);
        }
    }
    if (ok)
    {
        ok = RefreshFirstDay(error);
    }
    if (!ok)
    {
        Rollback();
        return false;
    }
    return Commit(error);
}

bool Store::TableHasColumn(const char *table, const char *column) const
{
    sqlite3_stmt *statement = nullptr;
    std::string sql = std::string("SELECT 1 FROM pragma_table_info('") + table + "') WHERE name = ?";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    {
        return false;
    }
    sqlite3_bind_text(statement, 1, column, -1, SQLITE_TRANSIENT);
    const int step = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return step == SQLITE_ROW;
}

bool Store::UpsertFirstWordSelection(int day_key, bool is_first, std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!BeginTransaction(error))
    {
        return false;
    }
    sqlite3_stmt *statement = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kUpsertFirstWordSql, -1, &statement, nullptr) == SQLITE_OK;
    if (!ok)
    {
        error = Describe("prepare first-word upsert", db_);
    }
    else
    {
        sqlite3_bind_int64(statement, 1, day_key);
        sqlite3_bind_int64(statement, 2, is_first ? 1 : 0);
        sqlite3_bind_int64(statement, 3, 1);
        ok = sqlite3_step(statement) == SQLITE_DONE;
        if (!ok)
        {
            error = Describe("write first-word", db_);
        }
        sqlite3_finalize(statement);
    }
    if (ok)
    {
        // A brand-new day must refresh first_day in the same transaction so the
        // overview sees the day at all.
        ok = RefreshFirstDay(error);
    }
    if (!ok)
    {
        Rollback();
        return false;
    }
    return Commit(error);
}

bool Store::DailyRows(std::vector<DailyRow> &rows, std::string &error) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    rows.clear();
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT day_key, cjk, latin, digit, punct, other, active_ms, first_word_hits, "
                           "first_word_total FROM stats_daily "
                           "ORDER BY day_key",
                           -1, &statement, nullptr) != SQLITE_OK)
    {
        error = Describe("query daily", db_);
        return false;
    }

    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW)
    {
        DailyRow row;
        row.day_key = static_cast<int>(sqlite3_column_int64(statement, 0));
        row.cjk = sqlite3_column_int64(statement, 1);
        row.latin = sqlite3_column_int64(statement, 2);
        row.digit = sqlite3_column_int64(statement, 3);
        row.punct = sqlite3_column_int64(statement, 4);
        row.other = sqlite3_column_int64(statement, 5);
        row.active_ms = sqlite3_column_int64(statement, 6);
        row.first_word_hits = sqlite3_column_int64(statement, 7);
        row.first_word_total = sqlite3_column_int64(statement, 8);
        rows.push_back(row);
    }
    const bool ok = step == SQLITE_DONE;
    if (!ok)
    {
        error = Describe("query daily", db_);
        rows.clear();
    }
    sqlite3_finalize(statement);
    return ok;
}

bool Store::HourlyRows(int day_key, std::vector<HourlyRow> &rows, std::string &error) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    rows.clear();
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT day_key, hour, chars, active_ms FROM stats_hourly WHERE day_key = ? "
                           "ORDER BY hour",
                           -1, &statement, nullptr) != SQLITE_OK)
    {
        error = Describe("query hourly", db_);
        return false;
    }
    sqlite3_bind_int64(statement, 1, day_key);

    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW)
    {
        HourlyRow row;
        row.day_key = static_cast<int>(sqlite3_column_int64(statement, 0));
        row.hour = static_cast<int>(sqlite3_column_int64(statement, 1));
        row.chars = sqlite3_column_int64(statement, 2);
        row.active_ms = sqlite3_column_int64(statement, 3);
        rows.push_back(row);
    }
    const bool ok = step == SQLITE_DONE;
    if (!ok)
    {
        error = Describe("query hourly", db_);
        rows.clear();
    }
    sqlite3_finalize(statement);
    return ok;
}

bool Store::HourlyRowsAll(std::vector<HourlyRow> &rows, std::string &error) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    rows.clear();
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT day_key, hour, chars, active_ms FROM stats_hourly ORDER BY day_key, hour", -1,
                           &statement, nullptr) != SQLITE_OK)
    {
        error = Describe("query all hourly", db_);
        return false;
    }

    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW)
    {
        HourlyRow row;
        row.day_key = static_cast<int>(sqlite3_column_int64(statement, 0));
        row.hour = static_cast<int>(sqlite3_column_int64(statement, 1));
        row.chars = sqlite3_column_int64(statement, 2);
        row.active_ms = sqlite3_column_int64(statement, 3);
        rows.push_back(row);
    }
    const bool ok = step == SQLITE_DONE;
    if (!ok)
    {
        error = Describe("query all hourly", db_);
        rows.clear();
    }
    sqlite3_finalize(statement);
    return ok;
}

bool Store::HonorUnlocks(std::vector<HonorUnlock> &unlocks, std::string &error) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    unlocks.clear();
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id, unlocked_day_key FROM stats_honor ORDER BY unlocked_day_key, id", -1,
                           &statement, nullptr) != SQLITE_OK)
    {
        error = Describe("query honor", db_);
        return false;
    }

    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW)
    {
        HonorUnlock unlock;
        const unsigned char *id = sqlite3_column_text(statement, 0);
        unlock.id = id != nullptr ? reinterpret_cast<const char *>(id) : "";
        unlock.day_key = static_cast<int>(sqlite3_column_int64(statement, 1));
        unlocks.push_back(std::move(unlock));
    }
    const bool ok = step == SQLITE_DONE;
    if (!ok)
    {
        error = Describe("query honor", db_);
        unlocks.clear();
    }
    sqlite3_finalize(statement);
    return ok;
}

bool Store::AddHonorUnlocks(const std::vector<HonorUnlock> &unlocks, std::string &error)
{
    if (unlocks.empty())
    {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!BeginTransaction(error))
    {
        return false;
    }
    sqlite3_stmt *statement = nullptr;
    bool ok = sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO stats_honor(id, unlocked_day_key) VALUES(?, ?)", -1,
                                 &statement, nullptr) == SQLITE_OK;
    if (!ok)
    {
        error = Describe("prepare honor upsert", db_);
    }
    else
    {
        for (const HonorUnlock &unlock : unlocks)
        {
            sqlite3_bind_text(statement, 1, unlock.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(statement, 2, unlock.day_key);
            if (sqlite3_step(statement) != SQLITE_DONE)
            {
                error = Describe("write honor", db_);
                ok = false;
                break;
            }
            sqlite3_reset(statement);
        }
        sqlite3_finalize(statement);
    }
    if (!ok)
    {
        Rollback();
        return false;
    }
    return Commit(error);
}

bool Store::Meta(const std::string &key, std::string &value, bool &present, std::string &error) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    value.clear();
    present = false;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM stats_meta WHERE key = ?", -1, &statement, nullptr) != SQLITE_OK)
    {
        error = Describe("read meta", db_);
        return false;
    }
    sqlite3_bind_text(statement, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    const int step = sqlite3_step(statement);
    if (step == SQLITE_ROW)
    {
        const unsigned char *text = sqlite3_column_text(statement, 0);
        value = text != nullptr ? reinterpret_cast<const char *>(text) : std::string();
        present = true;
    }
    else if (step != SQLITE_DONE)
    {
        error = Describe("read meta", db_);
        sqlite3_finalize(statement);
        return false;
    }
    sqlite3_finalize(statement);
    return true;
}

bool Store::SetMeta(const std::string &key, const std::string &value, std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!BeginTransaction(error))
    {
        return false;
    }
    if (!WriteMeta(key, value, error))
    {
        Rollback();
        return false;
    }
    return Commit(error);
}

bool Store::ClearThrough(int keep_from_day_key, int64_t &removed_days, std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    removed_days = 0;
    if (!BeginTransaction(error))
    {
        return false;
    }

    bool ok = true;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM stats_daily WHERE day_key < ?", -1, &statement, nullptr) != SQLITE_OK)
    {
        error = Describe("clear daily", db_);
        ok = false;
    }
    else
    {
        sqlite3_bind_int64(statement, 1, keep_from_day_key);
        ok = sqlite3_step(statement) == SQLITE_DONE;
        if (!ok)
        {
            error = Describe("clear daily", db_);
        }
        else
        {
            removed_days = sqlite3_changes(db_);
        }
        sqlite3_finalize(statement);
    }

    if (ok)
    {
        statement = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM stats_hourly WHERE day_key < ?", -1, &statement, nullptr) != SQLITE_OK)
        {
            error = Describe("clear hourly", db_);
            ok = false;
        }
        else
        {
            sqlite3_bind_int64(statement, 1, keep_from_day_key);
            ok = sqlite3_step(statement) == SQLITE_DONE;
            if (!ok)
            {
                error = Describe("clear hourly", db_);
            }
            sqlite3_finalize(statement);
        }
    }
    if (ok)
    {
        ok = RefreshFirstDay(error);
    }
    if (!ok)
    {
        Rollback();
        return false;
    }
    return Commit(error);
}

bool Store::ClearAll(int64_t &removed_days, std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    removed_days = 0;
    if (!BeginTransaction(error))
    {
        return false;
    }

    // Unconditional deletes, not ClearThrough(0): day keys are positive, so a
    // "day_key < 0" predicate would remove nothing.
    bool ok = true;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM stats_daily", -1, &statement, nullptr) != SQLITE_OK)
    {
        error = Describe("clear all daily", db_);
        ok = false;
    }
    else
    {
        ok = sqlite3_step(statement) == SQLITE_DONE;
        if (!ok)
        {
            error = Describe("clear all daily", db_);
        }
        else
        {
            removed_days = sqlite3_changes(db_);
        }
        sqlite3_finalize(statement);
    }

    if (ok)
    {
        statement = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM stats_hourly", -1, &statement, nullptr) != SQLITE_OK)
        {
            error = Describe("clear all hourly", db_);
            ok = false;
        }
        else
        {
            ok = sqlite3_step(statement) == SQLITE_DONE;
            if (!ok)
            {
                error = Describe("clear all hourly", db_);
            }
            sqlite3_finalize(statement);
        }
    }
    if (ok)
    {
        // RefreshFirstDay deletes the empty first_day key, so the overview
        // falls back to the empty state.
        ok = RefreshFirstDay(error);
    }
    if (ok)
    {
        // The retention marker goes too: after a wipe there is nothing left to
        // keep, and the next write may start a fresh once-per-day cycle.
        ok = DeleteMetaRow(kStoreMetaLastRetentionDay, error);
    }
    // stats_honor is intentionally not touched here. Clearing counts is not a
    // request to forget achievements; retention likewise leaves it alone.
    if (!ok)
    {
        Rollback();
        return false;
    }
    return Commit(error);
}

bool RunDailyRetention(Store &store, Retention retention, int today_day_key, RetentionOutcome &outcome,
                       std::string &error)
{
    outcome = RetentionOutcome{};

    std::string marker;
    bool present = false;
    if (!store.Meta(kStoreMetaLastRetentionDay, marker, present, error))
    {
        return false;
    }
    int last_day_key = 0;
    if (present)
    {
        char *end = nullptr;
        const long long parsed = std::strtoll(marker.c_str(), &end, 10);
        // A malformed marker falls back to 0: the cleanup then runs once and
        // rewrites the key, which repairs a hand-edited database instead of
        // stalling the policy forever.
        if (!marker.empty() && end != marker.c_str() && *end == '\0' && parsed > 0)
        {
            last_day_key = static_cast<int>(parsed);
        }
    }
    if (last_day_key == today_day_key)
    {
        outcome.already_ran_today = true;
        return true;
    }

    int cutoff_day_key = 0;
    if (RetentionCutoff(retention, today_day_key, cutoff_day_key))
    {
        if (!store.ClearThrough(cutoff_day_key, outcome.removed_days, error))
        {
            return false;
        }
    }
    // Forever still writes the marker: the once-per-day check is about the
    // day, not about the policy, so a later switch to a finite window still
    // cleans up on the next day's first write.
    return store.SetMeta(kStoreMetaLastRetentionDay, std::to_string(today_day_key), error);
}

bool Store::FirstDay(int &day_key, bool &present, std::string &error) const
{
    std::string value;
    if (!Meta(kStoreMetaFirstDay, value, present, error))
    {
        return false;
    }
    day_key = 0;
    if (!present)
    {
        return true;
    }
    char *end = nullptr;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (value.empty() || end == value.c_str() || *end != '\0')
    {
        error = "stats store: invalid first_day value \"" + value + "\"";
        present = false;
        return false;
    }
    day_key = static_cast<int>(parsed);
    return true;
}
} // namespace MsimeStats
