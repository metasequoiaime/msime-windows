#include "tests/includes/test_framework.h"

#include "statistics/stats_overview.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace
{
using MsimeStats::DailyRow;
using MsimeStats::HourlyRow;
using MsimeStats::Overview;

int DayKey(int year, int month, int day)
{
    return year * 10000 + month * 100 + day;
}

DailyRow Row(int day_key, int64_t cjk = 0, int64_t latin = 0, int64_t digit = 0, int64_t punct = 0, int64_t other = 0,
             int64_t active_ms = 0)
{
    DailyRow row;
    row.day_key = day_key;
    row.cjk = cjk;
    row.latin = latin;
    row.digit = digit;
    row.punct = punct;
    row.other = other;
    row.active_ms = active_ms;
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

std::vector<DailyRow> Rows(std::initializer_list<int> day_keys)
{
    std::vector<DailyRow> rows;
    rows.reserve(day_keys.size());
    for (const int day_key : day_keys)
    {
        rows.push_back(Row(day_key, 1));
    }
    return rows;
}

bool NearlyEqual(double left, double right)
{
    return std::abs(left - right) <= 0.001;
}

constexpr int kToday = 20260920;
} // namespace

TEST_CASE(stats_overview_empty_state)
{
    const Overview overview = MsimeStats::ComputeOverview({}, {}, kToday);
    REQUIRE(!overview.has_data);
    REQUIRE_EQ(overview.total_chars, 0);
    REQUIRE_EQ(overview.days, 0);
    REQUIRE_EQ(overview.current_streak, 0);
    REQUIRE_EQ(overview.longest_streak, 0);
    REQUIRE_EQ(overview.today_day_key, kToday);
    REQUIRE_EQ(overview.today_hourly.size(), 24u);
    REQUIRE_EQ(overview.average_per_day, 0.0);
    REQUIRE_EQ(overview.today_speed, 0.0);
    REQUIRE_EQ(overview.average_speed, 0.0);
    REQUIRE_EQ(overview.fastest_speed, 0.0);
}

TEST_CASE(stats_overview_single_day)
{
    const std::vector<DailyRow> rows = {Row(kToday, 100, 20, 5, 10, 1, 120'000)};
    const std::vector<HourlyRow> hourly = {Hour(kToday, 9, 50), Hour(kToday, 10, 86)};
    const Overview overview = MsimeStats::ComputeOverview(rows, hourly, kToday);

    REQUIRE(overview.has_data);
    REQUIRE_EQ(overview.days, 1);
    REQUIRE_EQ(overview.total_chars, 136);
    REQUIRE_EQ(overview.today_chars, 136);
    REQUIRE_EQ(overview.today_active_ms, 120'000);
    REQUIRE_EQ(overview.average_per_day, 136.0);
    REQUIRE_EQ(overview.current_streak, 1);
    REQUIRE_EQ(overview.longest_streak, 1);
    REQUIRE_EQ(overview.best_day_key, kToday);
    REQUIRE_EQ(overview.best_day_chars, 136);
    // 速度分子只含 cjk + latin：120 readable chars / 2 min
    REQUIRE(NearlyEqual(overview.today_speed, 60.0));
    REQUIRE(NearlyEqual(overview.average_speed, overview.today_speed));
    REQUIRE(NearlyEqual(overview.fastest_speed, 60.0));
    REQUIRE_EQ(overview.fastest_day_key, kToday);
    REQUIRE_EQ(overview.categories.Total(), 136);
    REQUIRE_EQ(overview.categories.cjk, 100);
    REQUIRE_EQ(overview.today_hourly[9], 50);
    REQUIRE_EQ(overview.today_hourly[10], 86);
    REQUIRE_EQ(overview.today_hourly[11], 0);
}

TEST_CASE(stats_overview_excludes_other_days_from_hourly)
{
    const std::vector<DailyRow> rows = {Row(20260919, 1), Row(kToday, 2)};
    const std::vector<HourlyRow> hourly = {Hour(20260919, 9, 100), Hour(kToday, 9, 7)};
    const Overview overview = MsimeStats::ComputeOverview(rows, hourly, kToday);
    REQUIRE_EQ(overview.today_hourly[9], 7);
}

// Zero-record days are not rows, so they must not enter the average
// denominator: 10 characters over two recorded days is 5/day even when the
// calendar span is twenty days.
TEST_CASE(stats_overview_average_per_day_excludes_days_without_records)
{
    const std::vector<DailyRow> rows = {Row(20260901, 4), Row(kToday, 6)};
    const Overview overview = MsimeStats::ComputeOverview(rows, {}, kToday);
    REQUIRE_EQ(overview.average_per_day, 5.0);
    REQUIRE_EQ(overview.days, 2);
}

TEST_CASE(stats_overview_current_streak)
{
    struct Case
    {
        const char *name;
        std::vector<DailyRow> rows;
        int want;
    };
    const std::vector<Case> cases = {
        {"no rows", {}, 0},
        {"today only", Rows({kToday}), 1},
        {"today and yesterday", Rows({kToday, 20260919}), 2},
        {"today missing, streak alive through yesterday", Rows({20260919, 20260918}), 2},
        {"gap before yesterday stops the streak", Rows({20260918, 20260917}), 0},
        {"gap inside the run", Rows({kToday, 20260919, 20260917}), 2},
        {"future rows do not count", Rows({20260925}), 0},
    };
    for (const Case &test_case : cases)
    {
        REQUIRE_EQ(MsimeStats::CurrentStreak(test_case.rows, kToday), test_case.want);
    }
}

TEST_CASE(stats_overview_longest_streak)
{
    struct Case
    {
        const char *name;
        std::vector<DailyRow> rows;
        int want;
    };
    const std::vector<Case> cases = {
        {"empty", {}, 0},
        {"single", Rows({kToday}), 1},
        {"across month boundary", Rows({20260929, 20260930, 20261001}), 3},
        {"longest in the middle", Rows({20260901, 20260910, 20260911, 20260912, 20260920}), 3},
        {"duplicates do not break a run", Rows({20260910, 20260910, 20260911}), 2},
    };
    for (const Case &test_case : cases)
    {
        REQUIRE_EQ(MsimeStats::LongestStreak(test_case.rows), test_case.want);
    }
}

TEST_CASE(stats_overview_best_day)
{
    const std::vector<DailyRow> rows = {Row(20260918, 10), Row(20260919, 30, 5), Row(kToday, 20)};
    const Overview overview = MsimeStats::ComputeOverview(rows, {}, kToday);
    REQUIRE_EQ(overview.best_day_key, 20260919);
    REQUIRE_EQ(overview.best_day_chars, 35);
}

TEST_CASE(stats_overview_speeds)
{
    const std::vector<DailyRow> rows = {
        // 120 readable chars in 2 min = 60/min; candidate for fastest.
        Row(20260918, 120, 0, 0, 0, 0, 120'000),
        // 600 readable chars in 5 min = 120/min; wins.
        Row(20260919, 600, 0, 0, 0, 0, 300'000),
        // 50 readable chars in 30s: too little active time to compete in the
        // ranking, floored to a minute for today's own speed, and still counted
        // in the overall average at its real duration.
        Row(kToday, 50, 0, 0, 0, 0, 30'000),
    };
    const Overview overview = MsimeStats::ComputeOverview(rows, {}, kToday);

    REQUIRE(NearlyEqual(overview.today_speed, 50.0));          // 50 / floored 1 min
    REQUIRE(NearlyEqual(overview.average_speed, 770.0 / 7.5)); // total 7.5 active minutes
    REQUIRE(NearlyEqual(overview.fastest_speed, 120.0));
    REQUIRE_EQ(overview.fastest_day_key, 20260919);
}

// 零散输入日：分母只按相邻上屏之间的间隔累加，活跃时间可能只有几秒，按它外推会得出
// 没人打过的速率（真实记录：81 个可读字符 / 1.9 秒活跃 = 2503 字/分）。这组数字取自
// 本机 stats.db 的 20261004。
TEST_CASE(stats_overview_today_speed_survives_sparse_typing)
{
    const std::vector<DailyRow> rows = {
        // 10-02 是真实记录里速度最高的一天（约 121 字/分）；10-03 有 31 个可读字符
        // 但完全没有活跃时间，速度是 0——它进不了最快日，也拉不动平均值。
        Row(20261002, 747, 473, 1, 81, 0, 603'977),
        Row(20261003, 0, 31, 0, 0, 0, 0),
        // 今天零星敲了几个键：2/7/10 点的间隔都超过 kActiveGapLimitMs 被丢掉，
        // 只有 8 点那 1.9 秒被计入。
        Row(kToday, 19, 62, 10, 0, 0, 1942),
    };
    const Overview overview = MsimeStats::ComputeOverview(rows, {}, kToday);

    REQUIRE_EQ(overview.today_active_ms, 1942);
    REQUIRE(NearlyEqual(overview.today_speed, 81.0));
    // 今日速度有了合理值，但这一天仍然不够资格拿下最快日：门槛管的是排名，
    // 下限管的是数值，两者不能互相替代。
    REQUIRE_EQ(overview.fastest_day_key, 20261002);
    REQUIRE(NearlyEqual(overview.fastest_speed, 1220.0 / (603'977 / 60000.0)));
    // 平均速度用全期累计活跃时长，本来就远离下限，不受今日这 1.9 秒影响。
    REQUIRE(NearlyEqual(overview.average_speed, 1332.0 / ((603'977 + 1942) / 60000.0)));
}

// 速度分子只数可读字符（cjk + latin）：标点、数字、其他加量都不提速。
TEST_CASE(stats_overview_speed_ignores_digits_punct_and_other)
{
    const std::vector<DailyRow> quiet = {Row(kToday, 30, 30, 0, 0, 0, 60'000)};
    const std::vector<DailyRow> noisy = {Row(kToday, 30, 30, 500, 500, 500, 60'000)};
    const Overview quiet_overview = MsimeStats::ComputeOverview(quiet, {}, kToday);
    const Overview noisy_overview = MsimeStats::ComputeOverview(noisy, {}, kToday);

    REQUIRE(NearlyEqual(noisy_overview.today_speed, 60.0));
    REQUIRE(NearlyEqual(noisy_overview.today_speed, quiet_overview.today_speed));
    REQUIRE(NearlyEqual(noisy_overview.average_speed, quiet_overview.average_speed));
    REQUIRE(NearlyEqual(noisy_overview.fastest_speed, quiet_overview.fastest_speed));
    // 卡片与最高日仍按总字符，不受速度口径影响。
    REQUIRE_EQ(noisy_overview.today_chars, 30 + 30 + 500 * 3);
    REQUIRE_EQ(noisy_overview.best_day_chars, 30 + 30 + 500 * 3);

    // 中文/英文加量才让速度上升：120 readable / 1 min = 120/min。
    const std::vector<DailyRow> more = {Row(kToday, 60, 60, 500, 500, 500, 60'000)};
    const Overview more_overview = MsimeStats::ComputeOverview(more, {}, kToday);
    REQUIRE(NearlyEqual(more_overview.today_speed, 120.0));
    REQUIRE(NearlyEqual(more_overview.average_speed, 120.0));
    REQUIRE(NearlyEqual(more_overview.fastest_speed, 120.0));
}

// 最高日是产量指标（按总字符），与速度指标（按 cjk + latin）是两个口径。
TEST_CASE(stats_overview_best_day_is_production_not_speed)
{
    const std::vector<DailyRow> rows = {
        // 9/18：标点多、总字多，拿下最高日。
        Row(20260918, 10, 0, 0, 200, 0, 60'000),
        // 9/19：可读字符多，拿下最快日。
        Row(20260919, 100, 0, 0, 0, 0, 60'000),
    };
    const Overview overview = MsimeStats::ComputeOverview(rows, {}, kToday);

    REQUIRE_EQ(overview.best_day_key, 20260918);
    REQUIRE_EQ(overview.best_day_chars, 210);
    REQUIRE_EQ(overview.fastest_day_key, 20260919);
    REQUIRE(NearlyEqual(overview.fastest_speed, 100.0));
}

TEST_CASE(stats_overview_speeds_with_no_active_time)
{
    const std::vector<DailyRow> rows = {Row(kToday, 42)};
    const Overview overview = MsimeStats::ComputeOverview(rows, {}, kToday);
    REQUIRE_EQ(overview.today_speed, 0.0);
    REQUIRE_EQ(overview.average_speed, 0.0);
    REQUIRE_EQ(overview.fastest_speed, 0.0);
    REQUIRE_EQ(overview.today_chars, 42);
}

TEST_CASE(stats_overview_daily_is_sorted_and_copied)
{
    std::vector<DailyRow> input = {Row(kToday, 1), Row(20260918, 2), Row(20260919, 3)};
    const Overview overview = MsimeStats::ComputeOverview(input, {}, kToday);
    REQUIRE_EQ(input[0].day_key, kToday);
    REQUIRE_EQ(overview.daily[0].day_key, 20260918);
    REQUIRE_EQ(overview.daily[2].day_key, kToday);
}

TEST_CASE(stats_overview_history_ends_before_today)
{
    const std::vector<DailyRow> rows = {Row(20260901, 10), Row(20260902, 10)};
    const Overview overview = MsimeStats::ComputeOverview(rows, {}, kToday);
    REQUIRE_EQ(overview.today_chars, 0);
    REQUIRE_EQ(overview.current_streak, 0);
    REQUIRE_EQ(overview.longest_streak, 2);
}
