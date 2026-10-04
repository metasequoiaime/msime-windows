#include "tests/includes/test_framework.h"

#include "statistics/stats_aggregate.h"
#include "statistics/stats_frames.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace
{
using ::FanyImeStatsEvent;

// The reference tool's tests pin the bucketing in a fixed UTC+8 zone; the
// aggregator accepts the resolver as a parameter precisely so these assertions
// do not depend on the machine's timezone.
MsimeStats::LocalTimeParts Utc8(MsimeStats::TimePoint utc)
{
    return MsimeStats::UtcPartsOf(utc + std::chrono::hours(8));
}

// At8 builds the instant that reads as the given UTC+8 wall-clock time.
MsimeStats::TimePoint At8(int year, int month, int day, int hour, int minute, int second)
{
    return MsimeStats::TimeFromUtcParts(year, month, day, hour - 8, minute, second);
}

uint64_t FiletimeAt8(int year, int month, int day, int hour, int minute, int second)
{
    return MsimeStats::TimeToFiletime(At8(year, month, day, hour, minute, second));
}

FanyImeStatsEvent MakeEvent(uint64_t filetime)
{
    FanyImeStatsEvent event;
    event.timestamp_utc_ft = filetime;
    return event;
}
} // namespace

TEST_CASE(stats_active_delta_ms_boundaries)
{
    const MsimeStats::TimePoint base = At8(2026, 9, 20, 10, 0, 0);
    const auto ms = [](int64_t value) { return std::chrono::milliseconds(value); };

    REQUIRE_EQ(MsimeStats::ActiveDeltaMs(MsimeStats::TimePoint{}, base), 0);
    REQUIRE_EQ(MsimeStats::ActiveDeltaMs(base, base), 0);
    REQUIRE_EQ(MsimeStats::ActiveDeltaMs(base, base + ms(1)), 1);
    REQUIRE_EQ(MsimeStats::ActiveDeltaMs(base, base + ms(MsimeStats::kActiveGapLimitMs - 1)), 9999);
    REQUIRE_EQ(MsimeStats::ActiveDeltaMs(base, base + ms(MsimeStats::kActiveGapLimitMs)), 10000);
    REQUIRE_EQ(MsimeStats::ActiveDeltaMs(base, base + ms(MsimeStats::kActiveGapLimitMs + 1)), 0);
    REQUIRE_EQ(MsimeStats::ActiveDeltaMs(base, base - std::chrono::seconds(1)), 0);
    REQUIRE_EQ(MsimeStats::ActiveDeltaMs(base, MsimeStats::TimePoint{}), 0);
}

TEST_CASE(stats_day_key_and_hour_follow_the_requested_zone)
{
    // 2026-09-20 23:30 in UTC+8 is still 2026-09-20 locally.
    const MsimeStats::LocalTimeParts local = Utc8(At8(2026, 9, 20, 23, 30, 0));
    REQUIRE_EQ(MsimeStats::DayKeyOf(local), 20260920);
    REQUIRE_EQ(local.hour, 23);

    // The same instant is still 2026-09-20 in UTC.
    const MsimeStats::TimePoint instant = At8(2026, 9, 20, 23, 30, 0);
    REQUIRE_EQ(MsimeStats::DayKeyOf(MsimeStats::UtcPartsOf(instant)), 20260920);

    // 17:00 UTC is already the next day in UTC+8; the requested zone decides
    // the bucket, not the timestamp's own.
    const MsimeStats::TimePoint late = MsimeStats::TimeFromUtcParts(2026, 9, 20, 17, 0, 0);
    REQUIRE_EQ(MsimeStats::DayKeyOf(Utc8(late)), 20260921);
    REQUIRE_EQ(Utc8(late).hour, 1);
}

TEST_CASE(stats_add_days_across_month_and_year)
{
    REQUIRE_EQ(MsimeStats::AddDays(20260920, -1), 20260919);
    REQUIRE_EQ(MsimeStats::AddDays(20261001, -1), 20260930);
    REQUIRE_EQ(MsimeStats::AddDays(20260101, -1), 20251231);
    REQUIRE_EQ(MsimeStats::AddDays(20260228, 1), 20260301);
    REQUIRE_EQ(MsimeStats::AddDays(20240228, 1), 20240229); // leap year
    REQUIRE_EQ(MsimeStats::AddDays(20261231, 1), 20270101);
}

TEST_CASE(stats_day_key_to_time_is_utc_midnight)
{
    REQUIRE(MsimeStats::DayKeyToTime(20260920) == MsimeStats::TimeFromUtcParts(2026, 9, 20, 0, 0, 0));
}

TEST_CASE(stats_accumulator_batches_and_active_time)
{
    MsimeStats::Accumulator accumulator(Utc8);
    FanyImeStatsEvent first = MakeEvent(FiletimeAt8(2026, 9, 20, 10, 0, 0));
    first.cjk = 2;
    FanyImeStatsEvent second = MakeEvent(FiletimeAt8(2026, 9, 20, 10, 0, 3));
    second.cjk = 1;
    second.punct = 1;
    FanyImeStatsEvent third = MakeEvent(FiletimeAt8(2026, 9, 20, 10, 0, 8));
    third.cjk = 1;
    third.other = 1;

    MsimeStats::Batch batch = accumulator.Add({first, second, third});
    REQUIRE_EQ(batch.days.size(), 1u);
    REQUIRE_EQ(batch.hours.size(), 1u);
    REQUIRE_EQ(batch.days[0].day_key, 20260920);
    REQUIRE_EQ(batch.days[0].cjk, 4);
    REQUIRE_EQ(batch.days[0].punct, 1);
    REQUIRE_EQ(batch.days[0].other, 1);
    REQUIRE_EQ(batch.days[0].active_ms, 8000);
    REQUIRE_EQ(batch.hours[0].hour, 10);
    REQUIRE_EQ(batch.hours[0].chars, 6);
    REQUIRE_EQ(batch.hours[0].active_ms, 8000);

    // A pause longer than the threshold contributes no active time.
    FanyImeStatsEvent after_pause = MakeEvent(FiletimeAt8(2026, 9, 20, 10, 1, 0));
    after_pause.latin = 3;
    batch = accumulator.Add({after_pause});
    REQUIRE_EQ(batch.days[0].active_ms, 0);
    REQUIRE_EQ(batch.days[0].latin, 3);
}

TEST_CASE(stats_accumulator_is_global_across_day_and_hour_boundaries)
{
    MsimeStats::Accumulator accumulator(Utc8);
    FanyImeStatsEvent first = MakeEvent(FiletimeAt8(2026, 9, 20, 23, 59, 58));
    first.cjk = 1;
    MsimeStats::Batch batch = accumulator.Add({first});
    REQUIRE_EQ(batch.days[0].day_key, 20260920);
    REQUIRE_EQ(batch.hours[0].hour, 23);

    // Two seconds later it is a new day and a new hour, but still active.
    FanyImeStatsEvent second = MakeEvent(FiletimeAt8(2026, 9, 21, 0, 0, 0));
    second.cjk = 1;
    batch = accumulator.Add({second});
    REQUIRE_EQ(batch.days[0].day_key, 20260921);
    REQUIRE_EQ(batch.hours[0].hour, 0);
    REQUIRE_EQ(batch.days[0].active_ms, 2000);
    REQUIRE_EQ(batch.hours[0].active_ms, 2000);
}

TEST_CASE(stats_accumulator_out_of_order_and_clock_rollback)
{
    MsimeStats::Accumulator accumulator(Utc8);
    FanyImeStatsEvent reference = MakeEvent(FiletimeAt8(2026, 9, 20, 10, 0, 10));
    reference.cjk = 1;
    accumulator.Add({reference});

    // Out-of-order event from a slower process: characters counted, no time.
    FanyImeStatsEvent slow = MakeEvent(FiletimeAt8(2026, 9, 20, 10, 0, 5));
    slow.cjk = 2;
    MsimeStats::Batch batch = accumulator.Add({slow});
    REQUIRE_EQ(batch.days[0].active_ms, 0);
    REQUIRE_EQ(batch.days[0].cjk, 2);

    // The accumulator clock must not have moved backwards.
    REQUIRE(accumulator.HasLastEvent());
    REQUIRE(accumulator.LastEventTime() == At8(2026, 9, 20, 10, 0, 10));

    // The next in-order event continues from the furthest point seen.
    FanyImeStatsEvent next = MakeEvent(FiletimeAt8(2026, 9, 20, 10, 0, 14));
    next.cjk = 1;
    batch = accumulator.Add({next});
    REQUIRE_EQ(batch.days[0].active_ms, 4000);
}

TEST_CASE(stats_accumulator_merges_multiple_events_into_one_day_delta)
{
    MsimeStats::Accumulator accumulator(Utc8);
    FanyImeStatsEvent first = MakeEvent(FiletimeAt8(2026, 9, 20, 9, 0, 0));
    first.cjk = 1;
    FanyImeStatsEvent second = MakeEvent(FiletimeAt8(2026, 9, 20, 9, 0, 1));
    second.cjk = 1;
    FanyImeStatsEvent third = MakeEvent(FiletimeAt8(2026, 9, 20, 20, 0, 0));
    third.latin = 1;

    const MsimeStats::Batch batch = accumulator.Add({first, second, third});
    REQUIRE_EQ(batch.days.size(), 1u);
    REQUIRE_EQ(batch.days[0].cjk, 2);
    REQUIRE_EQ(batch.days[0].latin, 1);
    REQUIRE_EQ(batch.days[0].active_ms, 1000);
    REQUIRE_EQ(batch.hours.size(), 2u);
    REQUIRE_EQ(batch.hours[0].hour, 9);
    REQUIRE_EQ(batch.hours[1].hour, 20);
}

TEST_CASE(stats_accumulator_zero_timestamp)
{
    MsimeStats::Accumulator accumulator(Utc8);
    FanyImeStatsEvent reference = MakeEvent(FiletimeAt8(2026, 9, 20, 10, 0, 0));
    reference.cjk = 1;
    accumulator.Add({reference});

    FanyImeStatsEvent broken = MakeEvent(0);
    broken.cjk = 3;
    const MsimeStats::Batch batch = accumulator.Add({broken});
    REQUIRE_EQ(batch.days.size(), 1u);
    REQUIRE_EQ(batch.days[0].day_key, 20260920);
    REQUIRE_EQ(batch.days[0].active_ms, 0);
    REQUIRE_EQ(batch.days[0].cjk, 3);
}

TEST_CASE(stats_accumulator_reset_clears_the_active_clock)
{
    MsimeStats::Accumulator accumulator(Utc8);
    FanyImeStatsEvent reference = MakeEvent(FiletimeAt8(2026, 9, 20, 10, 0, 0));
    reference.cjk = 1;
    accumulator.Add({reference});
    REQUIRE(accumulator.HasLastEvent());

    accumulator.Reset();
    REQUIRE(!accumulator.HasLastEvent());
    REQUIRE(accumulator.LastEventTime() == MsimeStats::TimePoint{});

    // After a reset the next event starts a fresh chain: no active time even
    // though it arrives one second after the previous (discarded) event.
    FanyImeStatsEvent next = MakeEvent(FiletimeAt8(2026, 9, 20, 10, 0, 1));
    next.cjk = 1;
    const MsimeStats::Batch batch = accumulator.Add({next});
    REQUIRE_EQ(batch.days[0].active_ms, 0);
}

TEST_CASE(stats_accumulator_batch_is_sorted)
{
    // Events belonging to several days in one frame must still come back sorted
    // by day key and by (day, hour), so the caller can write them deterministically.
    MsimeStats::Accumulator accumulator(Utc8);
    FanyImeStatsEvent next_day = MakeEvent(FiletimeAt8(2026, 9, 21, 9, 0, 0));
    next_day.cjk = 1;
    FanyImeStatsEvent evening = MakeEvent(FiletimeAt8(2026, 9, 20, 20, 0, 0));
    evening.latin = 1;
    FanyImeStatsEvent morning = MakeEvent(FiletimeAt8(2026, 9, 20, 8, 0, 0));
    morning.digit = 1;

    const MsimeStats::Batch batch = accumulator.Add({next_day, evening, morning});
    REQUIRE_EQ(batch.days.size(), 2u);
    REQUIRE_EQ(batch.days[0].day_key, 20260920);
    REQUIRE_EQ(batch.days[1].day_key, 20260921);
    REQUIRE_EQ(batch.hours.size(), 3u);
    REQUIRE_EQ(batch.hours[0].day_key, 20260920);
    REQUIRE_EQ(batch.hours[0].hour, 8);
    REQUIRE_EQ(batch.hours[1].hour, 20);
    REQUIRE_EQ(batch.hours[2].day_key, 20260921);
    REQUIRE_EQ(batch.hours[2].hour, 9);
}

TEST_CASE(stats_chars_per_minute_guard)
{
    REQUIRE_EQ(MsimeStats::CharsPerMinute(100, 0), 0.0);
    REQUIRE_EQ(MsimeStats::CharsPerMinute(120, 60'000), 120.0);
    REQUIRE_EQ(MsimeStats::CharsPerMinute(0, 60'000), 0.0);
    // 负活跃时间与零字符同样没有测量值。
    REQUIRE_EQ(MsimeStats::CharsPerMinute(120, -1), 0.0);
}

// 分母下限：活跃时间不足一分钟时按一分钟算，而不是按真实活跃时间外推。
// 第一条和最后一条取自本机 stats.db 里 20261004 / 20261002 的原始记录，其余是边界值。
TEST_CASE(stats_chars_per_minute_floors_tiny_active_time)
{
    // 81 字 / 1.9 秒活跃：按真实时间算是 2503 字/分，按下限算是 81 字/分。
    REQUIRE_EQ(MsimeStats::CharsPerMinute(81, 1942), 81.0);
    // 下限之下一律取满一分钟，值与该分钟数成正比。
    REQUIRE_EQ(MsimeStats::CharsPerMinute(50, 30'000), 50.0);
    REQUIRE_EQ(MsimeStats::CharsPerMinute(50, 1), 50.0);
    // 恰好一分钟不受影响；超过下限后回到真实速率。
    REQUIRE_EQ(MsimeStats::CharsPerMinute(120, MsimeStats::kMinSpeedActiveMs), 120.0);
    REQUIRE_EQ(MsimeStats::CharsPerMinute(120, MsimeStats::kMinSpeedActiveMs + 60'000), 60.0);
    REQUIRE_EQ(MsimeStats::CharsPerMinute(1220, 603'977), 1220.0 / (603'977 / 60000.0));
}
