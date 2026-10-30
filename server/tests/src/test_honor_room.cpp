#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "honor/honor_room.h"
#include "statistics/stats_store.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using MsimeHonor::Definition;
using MsimeHonor::Evaluate;
using MsimeHonor::Metric;
using MsimeHonor::Progress;
using MsimeHonor::Tier;
using MsimeStats::DailyRow;
using MsimeStats::HonorUnlock;
using MsimeStats::HourlyRow;
using MsimeStats::Store;

constexpr int kToday = 20261029;

DailyRow Row(int day_key, int64_t cjk, int64_t punct = 0, int64_t latin = 0, int64_t active_ms = 0,
             int64_t first_word_hits = 0, int64_t first_word_total = 0)
{
    DailyRow row;
    row.day_key = day_key;
    row.cjk = cjk;
    row.punct = punct;
    row.latin = latin;
    row.active_ms = active_ms;
    row.first_word_hits = first_word_hits;
    row.first_word_total = first_word_total;
    return row;
}

HourlyRow Hour(int day_key, int hour, int64_t chars)
{
    HourlyRow row;
    row.day_key = day_key;
    row.hour = hour;
    row.chars = chars;
    return row;
}

const Progress &FindProgress(const std::vector<Progress> &progress, const char *id)
{
    const auto found = std::find_if(progress.begin(), progress.end(),
                                    [id](const Progress &item) { return item.definition->id == std::string(id); });
    REQUIRE(found != progress.end());
    return *found;
}

class ScopedTempDir
{
  public:
    ScopedTempDir()
    {
        root_ = test::TempRoot() / L"msime-荣誉室测试";
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
} // namespace

TEST_CASE(honor_catalog_ids_are_unique_and_targets_are_positive)
{
    std::set<std::string> ids;
    const std::set<std::string> tiers = {"sprout", "branch", "ring", "crown"};
    const std::set<std::string> displays = {"count", "percent", "speed", "days"};
    for (const Definition &definition : MsimeHonor::Catalog())
    {
        REQUIRE(ids.insert(definition.id).second);
        REQUIRE(definition.target > 0);
        REQUIRE(definition.title != nullptr);
        REQUIRE(definition.title[0] != L'\0');
        REQUIRE(definition.description != nullptr);
        // 档位与展示单位的字符串直接进消息契约的 enum，写错会让响应被 Validate 拒掉。
        REQUIRE(tiers.count(MsimeHonor::TierId(definition.tier)) == 1);
        REQUIRE(displays.count(MsimeHonor::DisplayId(definition.display)) == 1);
        // 比率用百分比展示、速度用字/分展示，其余用人数或天数；错配会显示成奇怪的量纲。
        if (definition.metric == Metric::FirstWordPermille)
        {
            REQUIRE(definition.min_sample > 0);
            REQUIRE(definition.display == MsimeHonor::Display::Percent);
        }
        if (definition.metric == Metric::FastestSpeed)
            REQUIRE(definition.display == MsimeHonor::Display::Speed);
    }
    REQUIRE(ids.size() >= 20);
}

TEST_CASE(honor_metrics_aggregate_days_hours_and_first_word_counts)
{
    const std::vector<DailyRow> daily = {
        Row(20261026, 0, 0, 0, 0, 5, 5),
        Row(20261027, 1200, 30, 40),
        Row(20261028, 800, 20, 10),
        Row(kToday, 50, 5, 0),
    };
    const std::vector<HourlyRow> hourly = {Hour(20261027, 0, 100), Hour(20261027, 1, 300), Hour(20261027, 23, 900),
                                           Hour(20261028, 4, 200), Hour(20261028, 5, 700), Hour(20261028, 12, 600)};
    const MsimeHonor::Metrics metrics = MsimeHonor::ComputeMetrics(daily, hourly, kToday);

    REQUIRE_EQ(metrics.days, int64_t{3});
    REQUIRE_EQ(metrics.first_word_total, int64_t{5});
    REQUIRE_EQ(metrics.total_chars, int64_t{2155});
    REQUIRE_EQ(metrics.cjk, int64_t{2050});
    REQUIRE_EQ(metrics.latin, int64_t{50});
    REQUIRE_EQ(metrics.punct, int64_t{55});
    REQUIRE_EQ(metrics.today_chars, int64_t{55});
    REQUIRE_EQ(metrics.best_day_chars, int64_t{1270});
    REQUIRE_EQ(metrics.night_chars, int64_t{600});
    REQUIRE_EQ(metrics.max_hour_chars, int64_t{900});
    REQUIRE_EQ(metrics.current_streak, 3);
    REQUIRE_EQ(metrics.longest_streak, 3);
}

TEST_CASE(honor_speed_waits_for_enough_active_time)
{
    // 活跃时长不足一天的最小门槛时只展示进度，不判解锁；跨过门槛才结算。
    const std::vector<Progress> short_day = Evaluate({Row(kToday, 50, 0, 0, 59999)}, {}, kToday, {});
    const Progress &short_speed = FindProgress(short_day, "fast_hand");
    REQUIRE(!short_speed.unlocked);
    REQUIRE(!short_speed.sample_ok);

    const std::vector<Progress> full_minute = Evaluate({Row(kToday, 150, 0, 0, 60000)}, {}, kToday, {});
    const Progress &fast_hand = FindProgress(full_minute, "fast_hand");
    REQUIRE(fast_hand.unlocked);
    REQUIRE(fast_hand.sample_ok);
    REQUIRE_EQ(fast_hand.value, int64_t{150});

    const Progress &speed_200 = FindProgress(full_minute, "speed_200");
    REQUIRE(!speed_200.unlocked);
}

TEST_CASE(honor_zero_char_row_breaks_the_streak)
{
    // 中间只有选词计数的一天不算打字日，「连续」必须从这里断开。
    const std::vector<DailyRow> daily = {Row(20261027, 10), Row(20261028, 0, 0, 0, 0, 1, 1), Row(kToday, 10)};
    const MsimeHonor::Metrics metrics = MsimeHonor::ComputeMetrics(daily, {}, kToday);
    REQUIRE_EQ(metrics.days, int64_t{2});
    REQUIRE_EQ(metrics.current_streak, 1);
    REQUIRE_EQ(metrics.longest_streak, 1);
}

TEST_CASE(honor_store_reads_every_hour_bucket_in_order)
{
    ScopedTempDir temp;
    const fs::path path = temp.Root() / L"stats.db";
    std::string error;
    std::unique_ptr<Store> store = Store::Open(path, error);
    REQUIRE(store != nullptr);

    MsimeStats::Batch batch;
    batch.hours.push_back(MsimeStats::HourDelta{20261002, 23, 7, 1000});
    batch.hours.push_back(MsimeStats::HourDelta{20261001, 4, 3, 1000});
    batch.hours.push_back(MsimeStats::HourDelta{20261001, 0, 5, 1000});
    REQUIRE(store->Upsert(batch, error));

    std::vector<HourlyRow> hourly;
    REQUIRE(store->HourlyRowsAll(hourly, error));
    REQUIRE_EQ(hourly.size(), size_t{3});
    REQUIRE_EQ(hourly[0].day_key, 20261001);
    REQUIRE_EQ(hourly[0].hour, 0);
    REQUIRE_EQ(hourly[1].hour, 4);
    REQUIRE_EQ(hourly[2].day_key, 20261002);
    REQUIRE_EQ(hourly[2].chars, int64_t{7});
}

TEST_CASE(honor_evaluate_unlocks_by_threshold_and_reports_progress)
{
    const std::vector<DailyRow> daily = {Row(kToday, 1500)};
    const std::vector<Progress> progress = Evaluate(daily, {}, kToday, {});

    const Progress &thousand = FindProgress(progress, "thousand_chars");
    REQUIRE(thousand.unlocked);
    REQUIRE_EQ(thousand.value, int64_t{1500});

    const Progress &ten_thousand = FindProgress(progress, "ten_thousand");
    REQUIRE(!ten_thousand.unlocked);
    REQUIRE_EQ(ten_thousand.value, int64_t{1500});
}

TEST_CASE(honor_ratio_achievements_wait_for_a_usable_sample)
{
    const std::vector<Progress> sparse_sample = Evaluate({Row(kToday, 0, 0, 0, 0, 90, 100)}, {}, kToday, {});
    const Progress &small_ratio = FindProgress(sparse_sample, "first_word_80");
    REQUIRE(!small_ratio.unlocked);
    REQUIRE(!small_ratio.sample_ok);
    REQUIRE_EQ(small_ratio.value, int64_t{900});

    const std::vector<Progress> large = Evaluate({Row(kToday, 0, 0, 0, 0, 900, 1000)}, {}, kToday, {});
    const Progress &large_ratio = FindProgress(large, "first_word_80");
    REQUIRE(large_ratio.unlocked);
    REQUIRE(large_ratio.sample_ok);
}

TEST_CASE(honor_stored_unlocks_survive_retention_and_keep_their_day)
{
    // 统计只剩今天，但百万字是上个月按当时的计数解锁的：保留策略删掉旧行之后仍要显示已解锁。
    const std::vector<HonorUnlock> unlocks = {{"million", 20260930}};
    const std::vector<Progress> progress = Evaluate({Row(kToday, 10)}, {}, kToday, unlocks);

    const Progress &million = FindProgress(progress, "million");
    REQUIRE(million.unlocked);
    REQUIRE_EQ(million.unlocked_day_key, 20260930);
    REQUIRE_EQ(million.value, int64_t{10});
}

TEST_CASE(honor_unlock_facts_are_idempotent_and_survive_clearing_counts)
{
    ScopedTempDir temp;
    const fs::path path = temp.Root() / L"stats.db";
    std::string error;
    std::unique_ptr<Store> store = Store::Open(path, error);
    REQUIRE(store != nullptr);

    REQUIRE(store->AddHonorUnlocks({{"million", 20260930}, {"fast_hand", 20261001}}, error));
    // 同一条成就再次结算不能覆盖第一次的解锁日。
    REQUIRE(store->AddHonorUnlocks({{"million", kToday}}, error));

    std::vector<HonorUnlock> unlocks;
    REQUIRE(store->HonorUnlocks(unlocks, error));
    REQUIRE_EQ(unlocks.size(), size_t{2});
    const auto million =
        std::find_if(unlocks.begin(), unlocks.end(), [](const HonorUnlock &unlock) { return unlock.id == "million"; });
    REQUIRE(million != unlocks.end());
    REQUIRE_EQ(million->day_key, 20260930);

    int64_t removed = 0;
    REQUIRE(store->ClearAll(removed, error));
    unlocks.clear();
    REQUIRE(store->HonorUnlocks(unlocks, error));
    REQUIRE_EQ(unlocks.size(), size_t{2});
}

TEST_CASE(honor_unlocks_survive_retention_cleanup)
{
    ScopedTempDir temp;
    const fs::path path = temp.Root() / L"stats.db";
    std::string error;
    std::unique_ptr<Store> store = Store::Open(path, error);
    REQUIRE(store != nullptr);

    // 一天旧统计和一条当月解锁；按保留策略删掉旧计数，解锁记录必须原样保留。
    MsimeStats::Batch batch;
    MsimeStats::DayDelta old_day;
    old_day.day_key = 20260101;
    old_day.cjk = 500;
    batch.days.push_back(old_day);
    REQUIRE(store->Upsert(batch, error));
    REQUIRE(store->AddHonorUnlocks({{"million", 20260101}}, error));

    int64_t removed = 0;
    REQUIRE(store->ClearThrough(20261001, removed, error));
    REQUIRE_EQ(removed, int64_t{1});

    std::vector<HonorUnlock> unlocks;
    REQUIRE(store->HonorUnlocks(unlocks, error));
    REQUIRE_EQ(unlocks.size(), size_t{1});
    REQUIRE_EQ(unlocks[0].id, std::string("million"));
    REQUIRE_EQ(unlocks[0].day_key, 20260101);
}

TEST_CASE(honor_unlocks_survive_daily_retention)
{
    ScopedTempDir temp;
    const fs::path path = temp.Root() / L"stats.db";
    std::string error;
    std::unique_ptr<Store> store = Store::Open(path, error);
    REQUIRE(store != nullptr);

    MsimeStats::Batch batch;
    MsimeStats::DayDelta old_day;
    old_day.day_key = 20260101;
    old_day.cjk = 500;
    batch.days.push_back(old_day);
    REQUIRE(store->Upsert(batch, error));
    REQUIRE(store->AddHonorUnlocks({{"million", 20260101}}, error));

    // 走真正的每日保留策略入口，而不是直接调 ClearThrough：标记读写和 cutoff 计算
    // 同样不能碰解锁记录。
    MsimeStats::RetentionOutcome outcome;
    REQUIRE(MsimeStats::RunDailyRetention(*store, MsimeStats::Retention::Days30, kToday, outcome, error));
    REQUIRE_EQ(outcome.removed_days, int64_t{1});

    std::vector<HonorUnlock> unlocks;
    REQUIRE(store->HonorUnlocks(unlocks, error));
    REQUIRE_EQ(unlocks.size(), size_t{1});
}
