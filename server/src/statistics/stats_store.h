#pragma once

// Local statistics database: schema, batch upsert, queries and cleanup. A
// direct port of the standalone tool's internal/store package.
//
// The schema deliberately holds only counts and time buckets: no text, no
// pinyin, no candidates, no application or window names. That property is the
// privacy contract and is asserted by tests.
//
// The store is used from two processes (Server writes, Settings reads and
// cleans). WAL mode plus a busy timeout serialise the cross-process access;
// in-process calls are serialised by a mutex. The path is supplied by the
// caller, so tests point it at a temporary directory and Server never reads an
// environment variable here.

#include "statistics/stats_types.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace MsimeStats
{
// MetaSchemaVersion is the stats_meta key the schema version is written under
// on open; kStoreSchemaVersion is the value. They must stay distinct: a future
// migration reads the key back and compares it against the current version.
inline constexpr const char *kStoreMetaSchemaVersion = "schema_version";
// v3 增加 stats_honor：荣誉室的解锁事实。它和每日计数分开存放，保留策略删掉旧计数后
// 已解锁的成就仍然保留，不会因为数据过期重新锁上。
inline constexpr const char *kStoreSchemaVersion = "3";

// First-word hit-rate columns of stats_daily (introduced by schema v2).
// first_word_counts commits made from the candidate list; first_word_hits
// counts those that selected the first candidate of the first page. Only
// counts, never text: the privacy contract is unchanged.
inline constexpr const char *kStoreColumnFirstWordHits = "first_word_hits";
inline constexpr const char *kStoreColumnFirstWordTotal = "first_word_total";

// MetaFirstDay holds the smallest day_key currently stored; it doubles as the
// empty-state predicate (the key is deleted when no data remains) and is
// recomputed after every write and cleanup.
inline constexpr const char *kStoreMetaFirstDay = "first_day";

// MetaLastRetentionDay holds the local day key the automatic retention cleanup
// last ran for. It is what makes the cleanup once-per-day and idempotent: the
// Server checks it after every successful write and skips the work when the
// marker already names today.
inline constexpr const char *kStoreMetaLastRetentionDay = "last_retention_day";

// Retention is a cleanup policy chosen in the settings view. Months and years
// are approximated by days (the UI explains the approximation). The default
// value is Forever: keeping everything never deletes anything behind the user's
// back.
enum class Retention
{
    Days30 = 30,
    Days90 = 90,
    Days180 = 180,
    Days365 = 365,
    Forever = 0,
};

// ParseRetention validates a value coming from the settings page.
bool ParseRetention(const std::string &value, Retention &out);

// RetentionCutoff returns the oldest day key a cleanup keeps: the retention
// window includes today, so "30 days" keeps today and the 29 days before it.
// Returns false for Forever (nothing may be deleted).
bool RetentionCutoff(Retention retention, int today_day_key, int &cutoff_day_key);

class Store
{
  public:
    // Open creates or opens the database at path and applies the schema.
    // Returns nullptr on failure with `error` describing the reason.
    static std::unique_ptr<Store> Open(const std::filesystem::path &path, std::string &error);

    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    const std::filesystem::path &Path() const
    {
        return path_;
    }

    // Upsert applies one batch in a single transaction and refreshes
    // first_day. An empty batch is a no-op.
    bool Upsert(const Batch &batch, std::string &error);

    // UpsertFirstWordSelection credits one Chinese candidate commit on
    // day_key without touching the character counters: +1 to first_word_total
    // and, when is_first, +1 to first_word_hits. The incremental two-column
    // update matches the design for the first-word hit rate; total == 0 days
    // carry no ratio (see DailyRow::FirstWordRatio).
    bool UpsertFirstWordSelection(int day_key, bool is_first, std::string &error);

    // DailyRows returns every recorded day, ascending.
    bool DailyRows(std::vector<DailyRow> &rows, std::string &error) const;

    // HourlyRows returns the hour buckets of one day, ascending by hour.
    bool HourlyRows(int day_key, std::vector<HourlyRow> &rows, std::string &error) const;

    // HourlyRowsAll returns every hour bucket, ascending by day then hour. The
    // honour room needs the 0-5 o'clock band across the whole history; the
    // statistics page only ever asks for today.
    bool HourlyRowsAll(std::vector<HourlyRow> &rows, std::string &error) const;

    // HonorUnlocks reads the unlock facts, ascending by day.
    bool HonorUnlocks(std::vector<HonorUnlock> &unlocks, std::string &error) const;

    // AddHonorUnlocks inserts unlock facts with INSERT OR IGNORE: an achievement
    // unlocked twice keeps its first day, which is what the UI shows. Callers
    // pass only the ids they believe are new; duplicates are harmless.
    bool AddHonorUnlocks(const std::vector<HonorUnlock> &unlocks, std::string &error);

    // Meta reads a stats_meta value; `present` reports whether the key exists.
    bool Meta(const std::string &key, std::string &value, bool &present, std::string &error) const;

    // SetMeta writes a stats_meta value.
    bool SetMeta(const std::string &key, const std::string &value, std::string &error);

    // ClearThrough deletes every day strictly before keep_from together with
    // its hour buckets, and reports the number of removed days. Day keys are
    // YYYYMMDD and therefore positive, so a keep_from of 0 removes nothing
    // (use ClearAll to wipe the database). Cleanup is irreversible by design.
    bool ClearThrough(int keep_from_day_key, int64_t &removed_days, std::string &error);

    // ClearAll deletes every recorded day and hour bucket and resets
    // first_day and last_retention_day, so the overview returns to the empty
    // state. It backs the explicit "clear all statistics" action; the
    // confirmation is the caller's responsibility.
    bool ClearAll(int64_t &removed_days, std::string &error);

    // FirstDay returns the smallest recorded day key; `present` is false for an
    // empty database.
    bool FirstDay(int &day_key, bool &present, std::string &error) const;

  private:
    Store(sqlite3 *db, std::filesystem::path path);

    bool Initialize(std::string &error);
    // Lifts a v1 database (or one stamped with a superseded version) to
    // kStoreSchemaVersion. Fresh databases already build with the v2 shape, so
    // this becomes a no-op there. Returns false on failure; statistics are
    // non-critical so callers only log.
    bool ApplySchemaMigration(std::string &error);
    bool TableHasColumn(const char *table, const char *column) const;
    bool Exec(const char *sql, std::string &error) const;
    bool BeginTransaction(std::string &error);
    void Rollback();
    bool Commit(std::string &error);
    bool RefreshFirstDay(std::string &error);
    bool WriteMeta(const std::string &key, const std::string &value, std::string &error);
    // Callers hold mutex_ and a transaction; ClearAll uses it to reset the
    // retention marker in the same atomic wipe.
    bool DeleteMetaRow(const std::string &key, std::string &error);

    sqlite3 *db_ = nullptr;
    std::filesystem::path path_;
    mutable std::mutex mutex_;
};

// RetentionOutcome reports what one RunDailyRetention call did.
struct RetentionOutcome
{
    // True when the marker already named today, so nothing was touched.
    bool already_ran_today = false;
    // Days removed by this run (0 for Forever or an already-run day).
    int64_t removed_days = 0;
};

// RunDailyRetention enforces the retention policy at most once per local day.
//
// It reads stats_meta.last_retention_day and does nothing when that marker
// already names today. Otherwise it applies the policy: Forever only records
// the day, every other value deletes everything older than the retention
// window (the window includes today). The marker is written only when the
// deletion succeeded, so a failure leaves it stale and the next call retries.
// A marker that is not a positive decimal day key is treated as "never ran",
// which repairs a hand-edited database instead of stalling the policy.
//
// The function is intentionally free-standing and takes the store by
// reference: the Server calls it after a successful write, and tests drive it
// directly with a fixed day key instead of waiting for midnight.
bool RunDailyRetention(Store &store, Retention retention, int today_day_key, RetentionOutcome &outcome,
                       std::string &error);
} // namespace MsimeStats
