#pragma once

// Data structures and calendar helpers for the built-in input statistics.
//
// This is a direct port of the former standalone MSIME-Stats tool
// (internal/stats/stats.go and internal/stats/overview.go), which was archived
// when the feature moved into the Server. Field names and arithmetic mirror
// that source; the thresholds and derived-metric semantics were calibrated on
// real input data and must not drift. The one deliberate exception is the
// speed metric: it counts readable characters only (cjk + latin), see
// DailyRow::SpeedChars().
//
// The header stays free of Windows headers: the day-key helpers are pure
// calendar arithmetic and the aggregator's timezone lookup is injected, so the
// derivation layer can also be built by the portable test target.

#include <chrono>
#include <cstdint>
#include <vector>

namespace MsimeStats
{
using TimePoint = std::chrono::system_clock::time_point;

// A pause of at most this much between two consecutive commits counts as
// active typing time. Calibrated on real input data (#429): 5s counted normal
// thinking pauses as active time and inflated the speed metric.
inline constexpr int64_t kActiveGapLimitMs = 10'000;

// Lower bound on the denominator of the speed metric. active_ms only ever
// counts the gaps *between* consecutive commits: the wait before the first key
// and the reaction after the last one are not in it. On a sparse day that
// remainder is all there is, and dividing by it reports a rate nobody typed --
// 81 readable characters over 1.9 s of counted gaps read as 2503 chars/min.
// Crediting a full minute keeps the figure a conservative lower bound instead of
// a measurement the sample cannot support, and costs nothing on real days: at
// active_ms just below the floor the two denominators are nearly identical.
inline constexpr int64_t kMinSpeedActiveMs = 60'000;

// Minimum active time a day needs before it may win the "fastest day" metric.
// Without it a day with a handful of characters in two seconds would dominate
// the ranking. This stays a separate gate rather than a consequence of
// kMinSpeedActiveMs: the floor only caps a short day at "chars per one credited
// minute", which is still a plausible-looking rate, so without this gate
// exactly those floored short days would take the ranking.
inline constexpr int64_t kFastestSpeedMinActiveMs = 60'000;

// LocalTimeParts is a resolved local calendar time: exactly what the day/hour
// bucketing needs. The resolver producing it is injectable (see
// stats_aggregate.h) so tests never depend on the machine's timezone.
struct LocalTimeParts
{
    int year = 1970;
    int month = 1; // 1-12
    int day = 1;   // 1-31
    int hour = 0;  // 0-23
};

// DayKeyOf packs a calendar day into the YYYYMMDD integer used as the SQLite
// primary key. An integer keeps the key trivial and the ordering stable.
inline int DayKeyOf(int year, int month, int day)
{
    return year * 10000 + month * 100 + day;
}

inline int DayKeyOf(const LocalTimeParts &parts)
{
    return DayKeyOf(parts.year, parts.month, parts.day);
}

namespace detail
{
// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// algorithms). Calendar arithmetic must not go through std::mktime: that would
// pull the machine's timezone and DST state into a pure function.
inline int64_t DaysFromCivil(int year, int month, int day)
{
    const int64_t y = (month <= 2) ? static_cast<int64_t>(year) - 1 : year;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = static_cast<unsigned>(month > 2 ? month - 3 : month + 9);
    const unsigned doy = (153u * mp + 2u) / 5u + static_cast<unsigned>(day) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

inline void CivilFromDays(int64_t days, int &year, int &month, int &day)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    day = static_cast<int>(doy - (153u * mp + 2u) / 5u + 1u);
    const int month_of_year = static_cast<int>(mp) + (mp < 10u ? 3 : -9);
    month = month_of_year;
    year = static_cast<int>(y + (month_of_year <= 2 ? 1 : 0));
}
} // namespace detail

// AddDays offsets a day key by whole days. The date math happens on calendar
// fields, so it stays correct across month/year boundaries and leap days
// without touching any instant or timezone.
inline int AddDays(int day_key, int days)
{
    const int64_t shifted = detail::DaysFromCivil(day_key / 10000, (day_key / 100) % 100, day_key % 100) + days;
    int year = 0;
    int month = 0;
    int day = 0;
    detail::CivilFromDays(shifted, year, month, day);
    return DayKeyOf(year, month, day);
}

// TimeFromUtcParts builds a time point from UTC calendar fields. Hour, minute
// and second may be outside their nominal ranges and simply offset the result,
// which lets callers express instants in a fixed offset without a timezone
// library (tests use it to model UTC+8).
inline TimePoint TimeFromUtcParts(int year, int month, int day, int hour, int minute, int second)
{
    const int64_t seconds =
        detail::DaysFromCivil(year, month, day) * 86400 + static_cast<int64_t>(hour) * 3600 + minute * 60 + second;
    return TimePoint(std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds(seconds)));
}

// UtcPartsOf splits a time point into UTC calendar fields. Instants before 1970
// are floored, not truncated toward zero.
inline LocalTimeParts UtcPartsOf(TimePoint time)
{
    constexpr int64_t kNsPerSecond = 1'000'000'000;
    constexpr int64_t kNsPerDay = 86'400 * kNsPerSecond;
    const int64_t nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    int64_t days = nanoseconds / kNsPerDay;
    int64_t remainder = nanoseconds % kNsPerDay;
    if (remainder < 0)
    {
        remainder += kNsPerDay;
        --days;
    }
    LocalTimeParts parts;
    detail::CivilFromDays(days, parts.year, parts.month, parts.day);
    parts.hour = static_cast<int>(remainder / (3600 * kNsPerSecond));
    return parts;
}

// DayKeyToTime converts a YYYYMMDD key to UTC midnight. Only the calendar
// fields matter; the time zone is a fixed vessel for date arithmetic.
inline TimePoint DayKeyToTime(int day_key)
{
    return TimeFromUtcParts(day_key / 10000, (day_key / 100) % 100, day_key % 100, 0, 0, 0);
}

// CharsPerMinute converts a character count and an active duration to the
// speed metric.
//
// Zero characters or zero active time yield 0 instead of a division result.
// Zero active time must stay 0 rather than being rescued by the
// kMinSpeedActiveMs floor: no active time at all is a missing measurement, not a
// small one. A positive but tiny duration is divided by kMinSpeedActiveMs rather
// than by its own value (see the constant for why).
inline double CharsPerMinute(int64_t chars, int64_t active_ms)
{
    if (chars <= 0 || active_ms <= 0)
    {
        return 0.0;
    }
    const int64_t denominator_ms = active_ms < kMinSpeedActiveMs ? kMinSpeedActiveMs : active_ms;
    return static_cast<double>(chars) / (static_cast<double>(denominator_ms) / 60000.0);
}

// DailyRow is one row of stats_daily: per-day category counts plus the active
// time attributed to that day.
struct DailyRow
{
    int day_key = 0;
    int64_t cjk = 0;
    int64_t latin = 0;
    int64_t digit = 0;
    int64_t punct = 0;
    int64_t other = 0;
    int64_t active_ms = 0;

    // Total returns the day's character count across all categories. This is
    // the production metric used by the cards, best day, calendar and detail
    // list; it is deliberately not the speed metric.
    int64_t Total() const
    {
        return cjk + latin + digit + punct + other;
    }

    // SpeedChars returns the characters that count toward the speed metric.
    // Speed deliberately ignores digits, punctuation and "other": they are
    // not typed prose and inflated the reading on real data. This is the only
    // intentional divergence from the standalone tool, which measured speed on
    // Total(); every threshold and formula elsewhere is unchanged.
    int64_t SpeedChars() const
    {
        return cjk + latin;
    }

    // Speed returns readable characters per active minute; 0 when there is no
    // active time at all, and a rate floored at one minute of active time when
    // there is only a little (see CharsPerMinute).
    double Speed() const
    {
        return CharsPerMinute(SpeedChars(), active_ms);
    }
};

// HourlyRow is one row of stats_hourly.
struct HourlyRow
{
    int day_key = 0;
    int hour = 0;
    int64_t chars = 0;
    int64_t active_ms = 0;
};

// DayDelta is the per-day contribution of one batch of events.
struct DayDelta
{
    int day_key = 0;
    int64_t cjk = 0;
    int64_t latin = 0;
    int64_t digit = 0;
    int64_t punct = 0;
    int64_t other = 0;
    int64_t active_ms = 0;
};

// HourDelta is the per-hour contribution of one batch of events.
struct HourDelta
{
    int day_key = 0;
    int hour = 0;
    int64_t chars = 0;
    int64_t active_ms = 0;
};

// Batch is the accumulated contribution of one frame, ready to be written in a
// single transaction. The vectors are sorted by day key and by (day, hour) for
// deterministic behaviour.
struct Batch
{
    std::vector<DayDelta> days;
    std::vector<HourDelta> hours;

    // Empty reports whether the batch carries nothing to write.
    bool Empty() const
    {
        return days.empty() && hours.empty();
    }
};

// CategoryCounts holds the five per-category totals.
struct CategoryCounts
{
    int64_t cjk = 0;
    int64_t latin = 0;
    int64_t digit = 0;
    int64_t punct = 0;
    int64_t other = 0;

    // Total returns the sum across all categories.
    int64_t Total() const
    {
        return cjk + latin + digit + punct + other;
    }
};

// Overview is the single payload the settings page renders: cards, calendar,
// hourly distribution, category split, speed card and the detail list all read
// from it. The frontend computes nothing beyond formatting.
struct Overview
{
    bool has_data = false;
    // FirstDayKey/LastDayKey delimit the recorded range (0 when empty).
    int first_day_key = 0;
    int last_day_key = 0;
    // Days is the number of days that have a record (zero-record days do not
    // exist as rows and never enter the average denominator).
    int days = 0;
    int64_t total_chars = 0;
    int64_t total_active_ms = 0;

    int today_day_key = 0;
    int64_t today_chars = 0;
    int64_t today_active_ms = 0;

    double average_per_day = 0.0;
    int current_streak = 0;
    int longest_streak = 0;

    int best_day_key = 0;
    int64_t best_day_chars = 0;

    double today_speed = 0.0;
    double average_speed = 0.0;
    double fastest_speed = 0.0;
    int fastest_day_key = 0;

    CategoryCounts categories;
    // TodayHourly is the 24-bucket character distribution of today.
    std::vector<int64_t> today_hourly;
    // Daily is every recorded day, ascending by day key (calendar + details).
    std::vector<DailyRow> daily;
};
} // namespace MsimeStats
