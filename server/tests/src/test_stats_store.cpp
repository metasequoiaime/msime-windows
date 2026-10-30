#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "statistics/stats_candidate_capture.h"
#include "statistics/stats_overview.h"
#include "statistics/stats_store.h"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using MsimeStats::Batch;
using MsimeStats::DailyRow;
using MsimeStats::DayDelta;
using MsimeStats::HourDelta;
using MsimeStats::HourlyRow;
using MsimeStats::Retention;
using MsimeStats::Store;

// The directory name is deliberately non-ASCII: every store path crosses the
// wide/narrow boundary into SQLite, and the profile directory can contain
// Chinese on a real machine.
class ScopedTempDir
{
  public:
    ScopedTempDir()
    {
        root_ = test::TempRoot() / L"msime-统计测试-存储";
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
        REQUIRE(!ec);
    }

    ~ScopedTempDir()
    {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    ScopedTempDir(const ScopedTempDir &) = delete;
    ScopedTempDir &operator=(const ScopedTempDir &) = delete;

    const fs::path &Root() const
    {
        return root_;
    }

  private:
    fs::path root_;
};

std::unique_ptr<Store> OpenStore(const ScopedTempDir &temp, fs::path &db_path)
{
    db_path = temp.Root() / L"stats.db";
    std::string error;
    std::unique_ptr<Store> store = Store::Open(db_path, error);
    REQUIRE(store != nullptr);
    return store;
}

int DayKey(int year, int month, int day)
{
    return year * 10000 + month * 100 + day;
}

bool HasDay(const std::vector<DailyRow> &rows, int day_key)
{
    return std::any_of(rows.begin(), rows.end(), [day_key](const DailyRow &row) { return row.day_key == day_key; });
}

bool HasHour(const std::vector<HourlyRow> &rows, int day_key, int hour)
{
    return std::any_of(rows.begin(), rows.end(),
                       [day_key, hour](const HourlyRow &row) { return row.day_key == day_key && row.hour == hour; });
}

bool NearlyEqual(double left, double right)
{
    return std::abs(left - right) <= 0.001;
}

class RawDb
{
  public:
    explicit RawDb(const fs::path &path)
    {
        // CREATE 让测试能从零构造一个库（迁移用例在打开前并不存在文件）；
        // 对已有的库无副作用，先 OpenStore 再检查的用例行为不变。
        if (sqlite3_open_v2(test::Utf8(path).c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
            SQLITE_OK)
        {
            if (db_ != nullptr)
            {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            throw std::runtime_error("Requirement failed: raw sqlite connection");
        }
    }

    ~RawDb()
    {
        if (db_ != nullptr)
        {
            sqlite3_close(db_);
        }
    }

    RawDb(const RawDb &) = delete;
    RawDb &operator=(const RawDb &) = delete;

    sqlite3 *get() const
    {
        return db_;
    }

  private:
    sqlite3 *db_ = nullptr;
};
} // namespace

TEST_CASE(stats_store_upsert_accumulates_and_maintains_first_day)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    std::string error;
    int first = 0;
    bool present = false;
    REQUIRE(store->FirstDay(first, present, error));
    REQUIRE(!present);

    Batch first_batch;
    first_batch.days.push_back(DayDelta{20260920, 10, 2, 0, 0, 0, 5000});
    first_batch.days.push_back(DayDelta{20260918, 1, 0, 1, 0, 0, 1000});
    first_batch.hours.push_back(HourDelta{20260920, 9, 12, 5000});
    REQUIRE(store->Upsert(first_batch, error));

    Batch second_batch;
    second_batch.days.push_back(DayDelta{20260920, 5, 0, 0, 1, 0, 2000});
    second_batch.days.push_back(DayDelta{20260901, 0, 0, 0, 0, 3, 300});
    second_batch.hours.push_back(HourDelta{20260920, 9, 6, 2000});
    second_batch.hours.push_back(HourDelta{20260920, 10, 6, 2000});
    REQUIRE(store->Upsert(second_batch, error));

    std::vector<DailyRow> daily;
    REQUIRE(store->DailyRows(daily, error));
    REQUIRE_EQ(daily.size(), 3u);
    // Rows come back ascending.
    REQUIRE_EQ(daily[0].day_key, 20260901);
    REQUIRE_EQ(daily[2].day_key, 20260920);
    REQUIRE_EQ(daily[2].cjk, 15);
    REQUIRE_EQ(daily[2].latin, 2);
    REQUIRE_EQ(daily[2].punct, 1);
    REQUIRE_EQ(daily[2].active_ms, 7000);

    REQUIRE(store->FirstDay(first, present, error));
    REQUIRE(present);
    REQUIRE_EQ(first, 20260901);

    std::vector<HourlyRow> hourly;
    REQUIRE(store->HourlyRows(20260920, hourly, error));
    REQUIRE_EQ(hourly.size(), 2u);
    REQUIRE_EQ(hourly[0].hour, 9);
    REQUIRE_EQ(hourly[0].chars, 18);
    REQUIRE_EQ(hourly[1].hour, 10);
    REQUIRE_EQ(hourly[1].chars, 6);
}

// SQLite must not invent the parent directory: a broken DataDir has to surface as
// a failed open with a message, so the caller can tell "no statistics yet" (absent
// file) apart from "the data directory is unusable" (absent parent).
TEST_CASE(stats_store_open_without_parent_directory_fails)
{
    ScopedTempDir temp;
    const fs::path missing = temp.Root() / L"absent" / L"stats.db";

    std::string error;
    std::unique_ptr<Store> store = Store::Open(missing, error);
    REQUIRE(store == nullptr);
    REQUIRE(!error.empty());
    REQUIRE(!fs::exists(missing));
}

TEST_CASE(stats_store_upsert_empty_batch_is_noop)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    std::string error;
    REQUIRE(store->Upsert(Batch{}, error));
    int first = 0;
    bool present = false;
    REQUIRE(store->FirstDay(first, present, error));
    REQUIRE(!present);
}

TEST_CASE(stats_store_persistence_across_reopen)
{
    ScopedTempDir temp;
    const fs::path db_path = temp.Root() / L"stats.db";

    {
        std::string error;
        std::unique_ptr<Store> store = Store::Open(db_path, error);
        REQUIRE(store != nullptr);
        Batch batch;
        batch.days.push_back(DayDelta{20260920, 42, 0, 0, 0, 0, 1234});
        REQUIRE(store->Upsert(batch, error));
    }

    std::string error;
    std::unique_ptr<Store> reopened = Store::Open(db_path, error);
    REQUIRE(reopened != nullptr);
    std::vector<DailyRow> daily;
    REQUIRE(reopened->DailyRows(daily, error));
    REQUIRE_EQ(daily.size(), 1u);
    REQUIRE_EQ(daily[0].cjk, 42);
    REQUIRE_EQ(daily[0].active_ms, 1234);

    std::string value;
    bool present = false;
    REQUIRE(reopened->Meta(MsimeStats::kStoreMetaSchemaVersion, value, present, error));
    REQUIRE(present);
    REQUIRE_EQ(value, std::string(MsimeStats::kStoreSchemaVersion));
}

TEST_CASE(stats_store_clear_through_boundaries)
{
    const int today = DayKey(2026, 9, 20);
    struct Case
    {
        Retention retention;
        int keep_days;
    };
    const std::vector<Case> cases = {
        {Retention::Days30, 30},
        {Retention::Days90, 90},
        {Retention::Days180, 180},
        {Retention::Days365, 365},
    };

    for (const Case &test_case : cases)
    {
        ScopedTempDir temp;
        fs::path db_path;
        std::unique_ptr<Store> store = OpenStore(temp, db_path);

        // One row per day across a window wider than any retention, seeded in a
        // single batch to keep the test quick.
        Batch seed;
        seed.days.reserve(400);
        seed.hours.reserve(400);
        for (int index = 0; index < 400; ++index)
        {
            const int key = MsimeStats::AddDays(today, -index);
            seed.days.push_back(DayDelta{key, 1, 0, 0, 0, 0, 1000});
            seed.hours.push_back(HourDelta{key, 12, 1, 1000});
        }
        std::string error;
        REQUIRE(store->Upsert(seed, error));

        int cutoff = 0;
        REQUIRE(MsimeStats::RetentionCutoff(test_case.retention, today, cutoff));
        int64_t removed = 0;
        REQUIRE(store->ClearThrough(cutoff, removed, error));
        REQUIRE_EQ(removed, static_cast<int64_t>(400 - test_case.keep_days));

        const int kept = MsimeStats::AddDays(today, -(test_case.keep_days - 1));
        int first = 0;
        bool present = false;
        REQUIRE(store->FirstDay(first, present, error));
        REQUIRE(present);
        REQUIRE_EQ(first, kept);

        // The boundary day survives, the day before it does not.
        std::vector<DailyRow> daily;
        REQUIRE(store->DailyRows(daily, error));
        REQUIRE(HasDay(daily, kept));
        REQUIRE(!HasDay(daily, MsimeStats::AddDays(kept, -1)));

        // Hour buckets follow the same boundary.
        std::vector<HourlyRow> hourly;
        REQUIRE(store->HourlyRows(kept, hourly, error));
        REQUIRE(HasHour(hourly, kept, 12));
        hourly.clear();
        REQUIRE(store->HourlyRows(MsimeStats::AddDays(kept, -1), hourly, error));
        REQUIRE(hourly.empty());
    }
}

TEST_CASE(stats_store_clear_forever_deletes_nothing)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch seed;
    seed.days.reserve(500);
    for (int index = 0; index < 500; ++index)
    {
        seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -index), 1, 0, 0, 0, 0, 0});
    }
    std::string error;
    REQUIRE(store->Upsert(seed, error));

    int cutoff = 0;
    REQUIRE(!MsimeStats::RetentionCutoff(Retention::Forever, today, cutoff));
    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE_EQ(rows.size(), 500u);
}

TEST_CASE(stats_store_clear_to_empty_resets_first_day)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch seed;
    seed.days.push_back(DayDelta{today, 5, 0, 0, 0, 0, 0});
    seed.hours.push_back(HourDelta{today, 8, 5, 0});
    std::string error;
    REQUIRE(store->Upsert(seed, error));

    int cutoff = 0;
    REQUIRE(MsimeStats::RetentionCutoff(Retention::Days30, today, cutoff));
    int64_t removed = 0;
    REQUIRE(store->ClearThrough(cutoff, removed, error));
    REQUIRE_EQ(removed, 0);

    REQUIRE(store->ClearThrough(MsimeStats::AddDays(today, 1), removed, error));
    REQUIRE_EQ(removed, 1);

    int first = 0;
    bool present = false;
    REQUIRE(store->FirstDay(first, present, error));
    REQUIRE(!present);

    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE(rows.empty());
    std::vector<HourlyRow> hourly;
    REQUIRE(store->HourlyRows(today, hourly, error));
    REQUIRE(hourly.empty());
}

// The privacy contract: the on-disk schema must not be able to store typed
// text. Daily and hourly rows are numbers only; stats_honor holds a catalog id
// and a day key, and the id comes from the fixed achievement table.
TEST_CASE(stats_store_schema_has_no_text_columns)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);
    RawDb raw(db_path);

    sqlite3_stmt *statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw.get(),
                               "SELECT name FROM pragma_table_info('stats_daily') "
                               "UNION ALL SELECT name FROM pragma_table_info('stats_hourly')",
                               -1, &statement, nullptr) == SQLITE_OK);
    const std::vector<std::string> allowed = {"day_key",         "hour",  "cjk",   "latin",     "digit",
                                              "punct",           "other", "chars", "active_ms", "first_word_hits",
                                              "first_word_total"};
    std::vector<std::string> columns;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW)
    {
        const char *name = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
        REQUIRE(name != nullptr);
        columns.push_back(name);
    }
    REQUIRE_EQ(step, SQLITE_DONE);
    sqlite3_finalize(statement);
    // 先收完再断言：断言失败时不能留着未 finalize 的语句，否则连接关不掉，
    // 临时目录删不干净，后面的用例会连锁失败。
    for (const std::string &name : columns)
        REQUIRE(std::find(allowed.begin(), allowed.end(), name) != allowed.end());
    // 表不存在或列被删掉时循环会真空通过；daily 9 列加 hourly 4 列正好 13。
    REQUIRE_EQ(columns.size(), size_t{13});

    // 荣誉室只有目录 id 和解锁日期，没有可写自由文本的列。
    statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw.get(), "SELECT name FROM pragma_table_info('stats_honor')", -1, &statement,
                               nullptr) == SQLITE_OK);
    std::vector<std::string> honor_columns;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW)
    {
        const char *name = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
        REQUIRE(name != nullptr);
        honor_columns.push_back(name);
    }
    REQUIRE_EQ(step, SQLITE_DONE);
    sqlite3_finalize(statement);
    const std::vector<std::string> honor_allowed = {"id", "unlocked_day_key"};
    for (const std::string &name : honor_columns)
        REQUIRE(std::find(honor_allowed.begin(), honor_allowed.end(), name) != honor_allowed.end());
    // 表不存在时 pragma 返回零行，上面的循环会真空通过；这里把列数钉死。
    REQUIRE_EQ(honor_columns.size(), size_t{2});

    // WAL is a database property, so a second connection can assert it too.
    statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw.get(), "PRAGMA journal_mode", -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE_EQ(sqlite3_step(statement), SQLITE_ROW);
    const char *mode = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
    REQUIRE_EQ(std::string(mode != nullptr ? mode : ""), std::string("wal"));
    sqlite3_finalize(statement);
}

// busy_timeout must make a write wait for a foreign writer instead of failing
// immediately; with WAL both processes can otherwise work concurrently.
TEST_CASE(stats_store_waits_for_a_cross_process_writer)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    sqlite3 *blocker = nullptr;
    REQUIRE(sqlite3_open_v2(test::Utf8(db_path).c_str(), &blocker, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(blocker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);

    std::atomic<bool> started{false};
    bool upsert_ok = false;
    std::string upsert_error;
    int64_t waited_ms = 0;
    std::thread writer([&]() {
        started.store(true);
        const auto begin = std::chrono::steady_clock::now();
        Batch batch;
        batch.days.push_back(DayDelta{DayKey(2026, 9, 20), 1, 0, 0, 0, 0, 0});
        upsert_ok = store->Upsert(batch, upsert_error);
        waited_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    });
    while (!started.load())
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(sqlite3_exec(blocker, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK);
    writer.join();

    REQUIRE(upsert_ok);
    REQUIRE(waited_ms >= 200);
    sqlite3_close(blocker);

    std::vector<DailyRow> rows;
    std::string error;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE_EQ(rows.size(), 1u);
}

TEST_CASE(stats_store_overview_reads_through_queries)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch batch;
    batch.days.push_back(DayDelta{20260919, 10, 0, 0, 0, 0, 60'000});
    batch.days.push_back(DayDelta{today, 20, 5, 0, 0, 0, 120'000});
    batch.hours.push_back(HourDelta{today, 14, 25, 120'000});
    std::string error;
    REQUIRE(store->Upsert(batch, error));

    std::vector<DailyRow> daily;
    std::vector<HourlyRow> hourly;
    REQUIRE(store->DailyRows(daily, error));
    REQUIRE(store->HourlyRows(today, hourly, error));
    const MsimeStats::Overview overview = MsimeStats::ComputeOverview(daily, hourly, today);
    REQUIRE(overview.has_data);
    REQUIRE_EQ(overview.days, 2);
    REQUIRE_EQ(overview.total_chars, 35);
    REQUIRE_EQ(overview.today_chars, 25);
    REQUIRE_EQ(overview.current_streak, 2);
    REQUIRE_EQ(overview.today_hourly[14], 25);
}

TEST_CASE(stats_store_parse_retention)
{
    Retention retention = Retention::Forever;
    REQUIRE(MsimeStats::ParseRetention("30d", retention));
    REQUIRE(retention == Retention::Days30);
    REQUIRE(MsimeStats::ParseRetention("90d", retention));
    REQUIRE(retention == Retention::Days90);
    REQUIRE(MsimeStats::ParseRetention("180d", retention));
    REQUIRE(retention == Retention::Days180);
    REQUIRE(MsimeStats::ParseRetention("365d", retention));
    REQUIRE(retention == Retention::Days365);
    REQUIRE(MsimeStats::ParseRetention("forever", retention));
    REQUIRE(retention == Retention::Forever);
    REQUIRE(!MsimeStats::ParseRetention("2d", retention));
    REQUIRE(!MsimeStats::ParseRetention("", retention));
}

// The retention marker is the whole idempotence mechanism: the first call of a
// day runs the policy, every later call that day only reads the marker, and a
// new day runs it again. No timer is involved.
TEST_CASE(stats_store_retention_runs_once_per_local_day)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch seed;
    seed.days.reserve(400);
    for (int index = 0; index < 400; ++index)
    {
        seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -index), 1, 0, 0, 0, 0, 1000});
    }
    std::string error;
    REQUIRE(store->Upsert(seed, error));
    // Yesterday's marker: today has not run yet.
    REQUIRE(
        store->SetMeta(MsimeStats::kStoreMetaLastRetentionDay, std::to_string(MsimeStats::AddDays(today, -1)), error));

    MsimeStats::RetentionOutcome outcome;
    REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Days30, today, outcome, error));
    REQUIRE(!outcome.already_ran_today);
    REQUIRE_EQ(outcome.removed_days, static_cast<int64_t>(400 - 30));

    std::string marker;
    bool present = false;
    REQUIRE(store->Meta(MsimeStats::kStoreMetaLastRetentionDay, marker, present, error));
    REQUIRE(present);
    REQUIRE_EQ(marker, std::to_string(today));

    // A second call on the same day must not delete anything - not even a row
    // that just arrived with an expired day key.
    Batch late;
    late.days.push_back(DayDelta{MsimeStats::AddDays(today, -200), 1, 0, 0, 0, 0, 1000});
    REQUIRE(store->Upsert(late, error));
    REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Days30, today, outcome, error));
    REQUIRE(outcome.already_ran_today);
    REQUIRE_EQ(outcome.removed_days, 0);
    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE(HasDay(rows, MsimeStats::AddDays(today, -200)));

    // The next local day runs the policy again and clears the late row.
    const int tomorrow = MsimeStats::AddDays(today, 1);
    REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Days30, tomorrow, outcome, error));
    REQUIRE(!outcome.already_ran_today);
    rows.clear();
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE(!HasDay(rows, MsimeStats::AddDays(today, -200)));
}

// Forever is not "skip the day": it still records the marker, so a later
// switch to a finite window starts cleaning from the next write.
TEST_CASE(stats_store_retention_forever_records_day_without_deleting)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch seed;
    seed.days.reserve(500);
    for (int index = 0; index < 500; ++index)
    {
        seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -index), 1, 0, 0, 0, 0, 0});
    }
    std::string error;
    REQUIRE(store->Upsert(seed, error));

    MsimeStats::RetentionOutcome outcome;
    REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Forever, today, outcome, error));
    REQUIRE(!outcome.already_ran_today);
    REQUIRE_EQ(outcome.removed_days, 0);

    std::string marker;
    bool present = false;
    REQUIRE(store->Meta(MsimeStats::kStoreMetaLastRetentionDay, marker, present, error));
    REQUIRE(present);
    REQUIRE_EQ(marker, std::to_string(today));

    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE_EQ(rows.size(), 500u);

    REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Forever, today, outcome, error));
    REQUIRE(outcome.already_ran_today);
    REQUIRE_EQ(outcome.removed_days, 0);
}

// The window includes today and the days back to today-(days-1); the first day
// outside it is removed, together with its hour buckets and the first_day key.
TEST_CASE(stats_store_retention_boundary_keeps_today_and_window_start)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    const int window_start = MsimeStats::AddDays(today, -29);
    const int expired = MsimeStats::AddDays(today, -30);
    Batch seed;
    seed.days.push_back(DayDelta{expired, 1, 0, 0, 0, 0, 0});
    seed.days.push_back(DayDelta{window_start, 1, 0, 0, 0, 0, 0});
    seed.days.push_back(DayDelta{today, 1, 0, 0, 0, 0, 0});
    seed.hours.push_back(HourDelta{expired, 9, 1, 0});
    seed.hours.push_back(HourDelta{window_start, 9, 1, 0});
    std::string error;
    REQUIRE(store->Upsert(seed, error));

    MsimeStats::RetentionOutcome outcome;
    REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Days30, today, outcome, error));
    REQUIRE_EQ(outcome.removed_days, 1);

    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE(HasDay(rows, window_start));
    REQUIRE(HasDay(rows, today));
    REQUIRE(!HasDay(rows, expired));

    std::vector<HourlyRow> hourly;
    REQUIRE(store->HourlyRows(window_start, hourly, error));
    REQUIRE(HasHour(hourly, window_start, 9));
    hourly.clear();
    REQUIRE(store->HourlyRows(expired, hourly, error));
    REQUIRE(hourly.empty());

    int first = 0;
    bool present = false;
    REQUIRE(store->FirstDay(first, present, error));
    REQUIRE(present);
    REQUIRE_EQ(first, window_start);
}

// A failed cleanup must not advance the marker, otherwise the stale rows would
// survive until the next calendar day before the retry.
TEST_CASE(stats_store_retention_failure_keeps_marker_stale)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    const int yesterday = MsimeStats::AddDays(today, -1);
    Batch seed;
    seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -100), 1, 0, 0, 0, 0, 0});
    seed.days.push_back(DayDelta{today, 1, 0, 0, 0, 0, 0});
    std::string error;
    REQUIRE(store->Upsert(seed, error));
    REQUIRE(store->SetMeta(MsimeStats::kStoreMetaLastRetentionDay, std::to_string(yesterday), error));

    // Break the table the cleanup deletes from: ClearThrough fails and the
    // marker must keep yesterday's value, so the next write retries.
    {
        RawDb raw(db_path);
        REQUIRE(sqlite3_exec(raw.get(), "DROP TABLE stats_daily", nullptr, nullptr, nullptr) == SQLITE_OK);
    }

    MsimeStats::RetentionOutcome outcome;
    REQUIRE(!MsimeStats::RunDailyRetention(*store, Retention::Days30, today, outcome, error));
    REQUIRE(!error.empty());

    std::string marker;
    bool present = false;
    REQUIRE(store->Meta(MsimeStats::kStoreMetaLastRetentionDay, marker, present, error));
    REQUIRE(present);
    REQUIRE_EQ(marker, std::to_string(yesterday));
}

// The explicit "clear all statistics" action wipes both tables and both meta
// keys, so the overview falls back to the empty state.
TEST_CASE(stats_store_clear_all_resets_rows_and_meta)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch seed;
    seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -2), 1, 0, 0, 0, 0, 0});
    seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -1), 1, 0, 0, 0, 0, 0});
    seed.days.push_back(DayDelta{today, 1, 0, 0, 0, 0, 0});
    seed.hours.push_back(HourDelta{today, 8, 1, 0});
    std::string error;
    REQUIRE(store->Upsert(seed, error));
    REQUIRE(store->SetMeta(MsimeStats::kStoreMetaLastRetentionDay, std::to_string(today), error));

    int64_t removed = 0;
    REQUIRE(store->ClearAll(removed, error));
    REQUIRE_EQ(removed, 3);

    std::vector<DailyRow> daily;
    REQUIRE(store->DailyRows(daily, error));
    REQUIRE(daily.empty());
    std::vector<HourlyRow> hourly;
    REQUIRE(store->HourlyRows(today, hourly, error));
    REQUIRE(hourly.empty());

    int first = 0;
    bool present = false;
    REQUIRE(store->FirstDay(first, present, error));
    REQUIRE(!present);
    std::string marker;
    REQUIRE(store->Meta(MsimeStats::kStoreMetaLastRetentionDay, marker, present, error));
    REQUIRE(!present);
    // schema_version is written on open and is not user data; it must survive.
    REQUIRE(store->Meta(MsimeStats::kStoreMetaSchemaVersion, marker, present, error));
    REQUIRE(present);
    REQUIRE_EQ(marker, std::string(MsimeStats::kStoreSchemaVersion));

    // Clearing an already empty database is a success that removed nothing.
    REQUIRE(store->ClearAll(removed, error));
    REQUIRE_EQ(removed, 0);
}

// A marker corrupted by a hand edit must not stall the policy forever: a value
// that is not a positive decimal day key is treated as "never ran", so the
// cleanup runs once and rewrites a valid key.
TEST_CASE(stats_store_retention_malformed_marker_self_repairs)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch seed;
    seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -100), 1, 0, 0, 0, 0, 0});
    seed.days.push_back(DayDelta{today, 1, 0, 0, 0, 0, 0});
    std::string error;
    REQUIRE(store->Upsert(seed, error));

    MsimeStats::RetentionOutcome outcome;
    std::string marker;
    bool present = false;
    for (const char *bad : {"not-a-day", "-5", "20260920x", ""})
    {
        REQUIRE(store->SetMeta(MsimeStats::kStoreMetaLastRetentionDay, bad, error));
        REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Days30, today, outcome, error));
        REQUIRE(!outcome.already_ran_today);
        REQUIRE(store->Meta(MsimeStats::kStoreMetaLastRetentionDay, marker, present, error));
        REQUIRE(present);
        REQUIRE_EQ(marker, std::to_string(today));
    }

    // The first repair run applied the policy; the day outside the window is gone.
    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE(!HasDay(rows, MsimeStats::AddDays(today, -100)));
    REQUIRE(HasDay(rows, today));
}

// A v1 database (the shipped schema up to now) must open, keep its rows, gain
// the two first-word columns filled with 0 (the migration must not invent any
// measurement for days that had none) and be stamped with the new version.
TEST_CASE(stats_store_migrates_v1_database_with_version_key)
{
    ScopedTempDir temp;
    const fs::path db_path = temp.Root() / L"stats.db";
    {
        RawDb raw(db_path);
        REQUIRE(sqlite3_exec(raw.get(),
                             "CREATE TABLE stats_daily(day_key INTEGER PRIMARY KEY, cjk INTEGER NOT NULL DEFAULT 0, "
                             "latin INTEGER NOT NULL DEFAULT 0, digit INTEGER NOT NULL DEFAULT 0, "
                             "punct INTEGER NOT NULL DEFAULT 0, other INTEGER NOT NULL DEFAULT 0, "
                             "active_ms INTEGER NOT NULL DEFAULT 0);"
                             "CREATE TABLE stats_hourly(day_key INTEGER NOT NULL, hour INTEGER NOT NULL, "
                             "chars INTEGER NOT NULL DEFAULT 0, active_ms INTEGER NOT NULL DEFAULT 0, "
                             "PRIMARY KEY(day_key, hour));"
                             "CREATE TABLE stats_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                             "INSERT INTO stats_meta(key, value) VALUES('schema_version', '1');"
                             "INSERT INTO stats_daily(day_key, cjk, active_ms) VALUES(20260919, 42, 1000);",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
    }

    std::string error;
    std::unique_ptr<Store> store = Store::Open(db_path, error);
    REQUIRE(store != nullptr);

    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE_EQ(rows.size(), 1u);
    REQUIRE_EQ(rows[0].cjk, 42);
    REQUIRE_EQ(rows[0].active_ms, 1000);
    // 历史日线补 0：迁移不给没有测量的日子编造命中率。
    REQUIRE_EQ(rows[0].first_word_hits, 0);
    REQUIRE_EQ(rows[0].first_word_total, 0);
    // 迁移不给没有测量的日子编造比率：total == 0 时 FirstWordRatio 是 nullopt，不是 0%。
    REQUIRE(!rows[0].FirstWordRatio().has_value());

    std::string value;
    bool present = false;
    REQUIRE(store->Meta(MsimeStats::kStoreMetaSchemaVersion, value, present, error));
    REQUIRE(present);
    REQUIRE_EQ(value, std::string(MsimeStats::kStoreSchemaVersion));

    // v3 的 stats_honor 由 kSchema 的 CREATE TABLE IF NOT EXISTS 补上：老库第一次打开
    // 就能读写解锁记录，不需要单独的 ALTER。
    REQUIRE(store->AddHonorUnlocks({{"first_commit", 20260919}}, error));
    std::vector<MsimeStats::HonorUnlock> unlocks;
    REQUIRE(store->HonorUnlocks(unlocks, error));
    REQUIRE_EQ(unlocks.size(), size_t{1});
    REQUIRE_EQ(unlocks[0].id, std::string("first_commit"));
}

// A database whose tables exist but were never stamped (a v1 file from before
// the meta key had that meaning, or a partially created one) migrates too:
// deciding from the metadata alone would strand it with a missing key.
TEST_CASE(stats_store_migrates_v1_database_without_version_key)
{
    ScopedTempDir temp;
    const fs::path db_path = temp.Root() / L"stats.db";
    {
        RawDb raw(db_path);
        REQUIRE(sqlite3_exec(raw.get(),
                             "CREATE TABLE stats_daily(day_key INTEGER PRIMARY KEY, cjk INTEGER NOT NULL DEFAULT 0, "
                             "latin INTEGER NOT NULL DEFAULT 0, digit INTEGER NOT NULL DEFAULT 0, "
                             "punct INTEGER NOT NULL DEFAULT 0, other INTEGER NOT NULL DEFAULT 0, "
                             "active_ms INTEGER NOT NULL DEFAULT 0);"
                             "INSERT INTO stats_daily(day_key, cjk) VALUES(20260920, 7);",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
    }

    std::string error;
    std::unique_ptr<Store> store = Store::Open(db_path, error);
    REQUIRE(store != nullptr);

    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE_EQ(rows.size(), 1u);
    REQUIRE_EQ(rows[0].cjk, 7);
    std::string value;
    bool present = false;
    REQUIRE(store->Meta(MsimeStats::kStoreMetaSchemaVersion, value, present, error));
    REQUIRE_EQ(value, std::string(MsimeStats::kStoreSchemaVersion));
}

// A v2 database has the first-word columns but no stats_honor. Opening it must
// create the honour table without touching the existing rows; the table is the
// only change v3 makes.
TEST_CASE(stats_store_migrates_v2_database_gains_honor_table)
{
    ScopedTempDir temp;
    const fs::path db_path = temp.Root() / L"stats.db";
    {
        RawDb raw(db_path);
        REQUIRE(sqlite3_exec(raw.get(),
                             "CREATE TABLE stats_daily(day_key INTEGER PRIMARY KEY, cjk INTEGER NOT NULL DEFAULT 0, "
                             "latin INTEGER NOT NULL DEFAULT 0, digit INTEGER NOT NULL DEFAULT 0, "
                             "punct INTEGER NOT NULL DEFAULT 0, other INTEGER NOT NULL DEFAULT 0, "
                             "active_ms INTEGER NOT NULL DEFAULT 0, first_word_hits INTEGER NOT NULL DEFAULT 0, "
                             "first_word_total INTEGER NOT NULL DEFAULT 0);"
                             "CREATE TABLE stats_hourly(day_key INTEGER NOT NULL, hour INTEGER NOT NULL, "
                             "chars INTEGER NOT NULL DEFAULT 0, active_ms INTEGER NOT NULL DEFAULT 0, "
                             "PRIMARY KEY(day_key, hour));"
                             "CREATE TABLE stats_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                             "INSERT INTO stats_meta(key, value) VALUES('schema_version', '2');"
                             "INSERT INTO stats_daily(day_key, cjk) VALUES(20261001, 9);"
                             "INSERT INTO stats_hourly(day_key, hour, chars) VALUES(20261001, 3, 4);",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
    }

    std::string error;
    std::unique_ptr<Store> store = Store::Open(db_path, error);
    REQUIRE(store != nullptr);

    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE_EQ(rows.size(), size_t{1});
    REQUIRE_EQ(rows[0].cjk, 9);
    REQUIRE(store->AddHonorUnlocks({{"thousand_chars", 20261001}}, error));
    std::vector<MsimeStats::HonorUnlock> unlocks;
    REQUIRE(store->HonorUnlocks(unlocks, error));
    REQUIRE_EQ(unlocks.size(), size_t{1});
    REQUIRE_EQ(unlocks[0].day_key, 20261001);

    // 原样保留的小时桶与版本戳也要核对：v3 只加表，不改已有数据。
    std::vector<HourlyRow> hourly;
    REQUIRE(store->HourlyRowsAll(hourly, error));
    REQUIRE_EQ(hourly.size(), size_t{1});
    REQUIRE_EQ(hourly[0].chars, int64_t{4});
    std::string version;
    bool present = false;
    REQUIRE(store->Meta(MsimeStats::kStoreMetaSchemaVersion, version, present, error));
    REQUIRE(present);
    REQUIRE_EQ(version, std::string(MsimeStats::kStoreSchemaVersion));
}

// The table shape decides, not the stamp: a database already stamped with the
// current version but still missing the columns (an earlier open whose
// migration step failed) must still be repaired.
TEST_CASE(stats_store_migrates_database_stamped_current_but_missing_columns)
{
    ScopedTempDir temp;
    const fs::path db_path = temp.Root() / L"stats.db";
    {
        RawDb raw(db_path);
        const std::string sql =
            std::string("CREATE TABLE stats_daily(day_key INTEGER PRIMARY KEY, cjk INTEGER NOT NULL DEFAULT 0, "
                        "latin INTEGER NOT NULL DEFAULT 0, digit INTEGER NOT NULL DEFAULT 0, "
                        "punct INTEGER NOT NULL DEFAULT 0, other INTEGER NOT NULL DEFAULT 0, "
                        "active_ms INTEGER NOT NULL DEFAULT 0);"
                        "CREATE TABLE stats_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                        "INSERT INTO stats_meta(key, value) VALUES('schema_version', '") +
            MsimeStats::kStoreSchemaVersion + "');";
        REQUIRE(sqlite3_exec(raw.get(), sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    }

    std::string error;
    std::unique_ptr<Store> store = Store::Open(db_path, error);
    REQUIRE(store != nullptr);
    REQUIRE(store->UpsertFirstWordSelection(DayKey(2026, 9, 20), true, error));
    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE_EQ(rows.size(), 1u);
    REQUIRE_EQ(rows[0].first_word_total, 1);
}

// A migration interrupted after the first ALTER leaves one of the two columns;
// the next open must add only the missing one instead of failing on the
// duplicate.
TEST_CASE(stats_store_migrates_half_applied_database)
{
    ScopedTempDir temp;
    const fs::path db_path = temp.Root() / L"stats.db";
    {
        RawDb raw(db_path);
        REQUIRE(sqlite3_exec(raw.get(),
                             "CREATE TABLE stats_daily(day_key INTEGER PRIMARY KEY, cjk INTEGER NOT NULL DEFAULT 0, "
                             "latin INTEGER NOT NULL DEFAULT 0, digit INTEGER NOT NULL DEFAULT 0, "
                             "punct INTEGER NOT NULL DEFAULT 0, other INTEGER NOT NULL DEFAULT 0, "
                             "active_ms INTEGER NOT NULL DEFAULT 0, first_word_hits INTEGER NOT NULL DEFAULT 0);"
                             "INSERT INTO stats_daily(day_key, cjk, first_word_hits) VALUES(20260920, 7, 3);",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
    }

    std::string error;
    std::unique_ptr<Store> store = Store::Open(db_path, error);
    REQUIRE(store != nullptr);
    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE_EQ(rows.size(), 1u);
    REQUIRE_EQ(rows[0].cjk, 7);
    REQUIRE_EQ(rows[0].first_word_hits, 3);
    REQUIRE_EQ(rows[0].first_word_total, 0);
}

// A brand-new database must be created at the current version directly.
TEST_CASE(stats_store_fresh_database_is_created_at_current_version)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    std::string value;
    bool present = false;
    std::string error;
    REQUIRE(store->Meta(MsimeStats::kStoreMetaSchemaVersion, value, present, error));
    REQUIRE(present);
    REQUIRE_EQ(value, std::string(MsimeStats::kStoreSchemaVersion));

    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE(rows.empty());
}

// The capture path writes one commit at a time; the counters must accumulate
// across calls, feed the user-character counters nothing, and drive
// FirstWordRatio (nullopt without selections, a real ratio with them).
TEST_CASE(stats_store_upsert_first_word_selection_increments_independently)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    std::string error;
    REQUIRE(store->UpsertFirstWordSelection(today, true, error));  // 首选
    REQUIRE(store->UpsertFirstWordSelection(today, false, error)); // 次选
    REQUIRE(store->UpsertFirstWordSelection(today, true, error));
    REQUIRE(store->UpsertFirstWordSelection(DayKey(2026, 9, 19), false, error));

    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE_EQ(rows.size(), 2u);
    REQUIRE_EQ(rows[1].day_key, today);
    REQUIRE_EQ(rows[1].first_word_hits, 2);
    REQUIRE_EQ(rows[1].first_word_total, 3);
    // 字符计数完全不被这条路径触碰：分母不是输入字数。
    REQUIRE_EQ(rows[1].cjk, 0);
    REQUIRE_EQ(rows[1].active_ms, 0);

    const MsimeStats::Overview overview = MsimeStats::ComputeOverview(rows, {}, today);
    REQUIRE(overview.today_first_word_ratio.has_value());
    REQUIRE(NearlyEqual(*overview.today_first_word_ratio, 2.0 / 3.0));
    REQUIRE_EQ(overview.today_first_word_total, 3);
    // 09-19 只有一次次选、零首选：这是一次真实测量（0%），不是 nullopt——
    // nullopt 只留给完全没有候选上屏的日子（first_word_total == 0，见 stats_types.h）。
    REQUIRE(rows[0].FirstWordRatio().has_value());
    REQUIRE(NearlyEqual(*rows[0].FirstWordRatio(), 0.0));
    REQUIRE(rows[1].FirstWordRatio().has_value());
    REQUIRE(NearlyEqual(*rows[1].FirstWordRatio(), 2.0 / 3.0));

    // 管道侧的批量 upsert 与本计数互不覆盖：再写一批字符计数后两列仍在。
    Batch batch;
    batch.days.push_back(DayDelta{today, 5, 0, 0, 0, 0, 0});
    REQUIRE(store->Upsert(batch, error));
    rows.clear();
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE_EQ(rows[1].cjk, 5);
    REQUIRE_EQ(rows[1].first_word_hits, 2);
    REQUIRE_EQ(rows[1].first_word_total, 3);
}

// The Chinese-candidate gate (the policy half of the capture): engine-ranked
// Chinese output counts; mixed-in English, emoji-style synthetic lists, date
// formats, translation glosses and the raw-fallback placeholder do not.
// Japanese candidates are excluded by scheme at the call site.
TEST_CASE(stats_capture_policy_counts_toward_first_word_selection)
{
    using MsimeStats::CountsTowardFirstWordSelection;
    WordItem item;
    item.source = CandidateSource::Database;
    item.sentence_association = false;
    REQUIRE(CountsTowardFirstWordSelection(item));
    item.source = CandidateSource::UserDatabase;
    REQUIRE(CountsTowardFirstWordSelection(item));
    item.source = CandidateSource::CloudSuggestion;
    REQUIRE(CountsTowardFirstWordSelection(item));
    item.source = CandidateSource::AiSuggestion;
    REQUIRE(CountsTowardFirstWordSelection(item));
    item.source = CandidateSource::QuickPhrase;
    REQUIRE(CountsTowardFirstWordSelection(item));
    item.source = CandidateSource::Generated;
    REQUIRE(CountsTowardFirstWordSelection(item));
    item.source = CandidateSource::NeuralDesktop;
    REQUIRE(CountsTowardFirstWordSelection(item));
    item.source = CandidateSource::NeuralKeyboard;
    REQUIRE(CountsTowardFirstWordSelection(item));
    item.source = CandidateSource::Collocation;
    REQUIRE(CountsTowardFirstWordSelection(item));

    // 整句联想是引擎输出，计入；同名 Fallback 的合成项（.raw 占位、译文页）不计。
    item.source = CandidateSource::Fallback;
    item.sentence_association = true;
    REQUIRE(CountsTowardFirstWordSelection(item));
    item.sentence_association = false;
    REQUIRE(!CountsTowardFirstWordSelection(item));

    item.sentence_association = false;
    item.source = CandidateSource::EnglishDictionary;
    REQUIRE(!CountsTowardFirstWordSelection(item));
    item.source = CandidateSource::Emoji;
    REQUIRE(!CountsTowardFirstWordSelection(item));
    item.source = CandidateSource::Kaomoji;
    REQUIRE(!CountsTowardFirstWordSelection(item));
    item.source = CandidateSource::DateTime;
    REQUIRE(!CountsTowardFirstWordSelection(item));
}

// 跨线程一致性由 Store 自身的 mutex 保证（管道线程与选词路径共用句柄）；
// 这里冒烟验证共享句柄下的连续 upsert 与首日/版本元数据的读取走的是同一份 schema。
TEST_CASE(stats_store_shared_store_reference_is_usable_from_capture_path)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    std::string error;
    const int today = DayKey(2026, 9, 20);
    REQUIRE(store->UpsertFirstWordSelection(today, true, error));
    REQUIRE(store->UpsertFirstWordSelection(today, true, error));

    int first = 0;
    bool present = false;
    REQUIRE(store->FirstDay(first, present, error));
    REQUIRE(present);
    REQUIRE_EQ(first, today);
    std::string value;
    REQUIRE(store->Meta(MsimeStats::kStoreMetaSchemaVersion, value, present, error));
    REQUIRE(present);
    REQUIRE_EQ(value, std::string(MsimeStats::kStoreSchemaVersion));
}
