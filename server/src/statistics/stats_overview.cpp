#include "statistics/stats_overview.h"

#include <algorithm>
#include <set>
#include <utility>

namespace MsimeStats
{
namespace
{
CategoryCounts SumCategories(const std::vector<DailyRow> &rows)
{
    CategoryCounts counts;
    for (const DailyRow &row : rows)
    {
        counts.cjk += row.cjk;
        counts.latin += row.latin;
        counts.digit += row.digit;
        counts.punct += row.punct;
        counts.other += row.other;
    }
    return counts;
}

// BestDay returns the day with the most characters; the earliest wins a tie.
// It is a production metric and counts every category (Total), unlike the
// speed metric which counts readable characters only.
std::pair<int, int64_t> BestDay(const std::vector<DailyRow> &rows)
{
    int day_key = 0;
    int64_t chars = 0;
    for (const DailyRow &row : rows)
    {
        if (row.Total() > chars)
        {
            day_key = row.day_key;
            chars = row.Total();
        }
    }
    return {day_key, chars};
}

// FastestDay returns the highest daily speed over days with enough active time
// to be meaningful. Speed counts readable characters only (DailyRow::Speed).
// The active-time gate is a separate criterion, not a stricter version of
// CharsPerMinute's floor. The floor only caps a short day at chars per one
// credited minute, which still lands inside the range real days report, so a
// burst-typing day that never accumulated a minute of gaps would still take the
// ranking with a rate it never sustained.
std::pair<int, double> FastestDay(const std::vector<DailyRow> &rows)
{
    int day_key = 0;
    double speed = 0.0;
    for (const DailyRow &row : rows)
    {
        if (row.active_ms < kFastestSpeedMinActiveMs)
        {
            continue;
        }
        const double candidate = row.Speed();
        if (candidate > speed)
        {
            day_key = row.day_key;
            speed = candidate;
        }
    }
    return {day_key, speed};
}
} // namespace

Overview ComputeOverview(const std::vector<DailyRow> &rows, const std::vector<HourlyRow> &today_hourly,
                         int today_day_key)
{
    std::vector<DailyRow> sorted = rows;
    std::sort(sorted.begin(), sorted.end(),
              [](const DailyRow &left, const DailyRow &right) { return left.day_key < right.day_key; });

    Overview overview;
    overview.today_day_key = today_day_key;
    overview.today_hourly.assign(24, 0);
    overview.daily = sorted;
    overview.categories = SumCategories(sorted);
    overview.days = static_cast<int>(sorted.size());
    int64_t total_speed_chars = 0;
    int64_t today_speed_chars = 0;
    for (const DailyRow &row : sorted)
    {
        overview.total_chars += row.Total();
        overview.total_active_ms += row.active_ms;
        total_speed_chars += row.SpeedChars();
        if (row.day_key == today_day_key)
        {
            overview.today_chars = row.Total();
            overview.today_active_ms = row.active_ms;
            today_speed_chars = row.SpeedChars();
        }
    }
    for (const HourlyRow &bucket : today_hourly)
    {
        if (bucket.day_key == today_day_key && bucket.hour >= 0 && bucket.hour < 24)
        {
            overview.today_hourly[static_cast<size_t>(bucket.hour)] += bucket.chars;
        }
    }

    overview.has_data = !sorted.empty();
    if (!overview.has_data)
    {
        return overview;
    }

    overview.first_day_key = sorted.front().day_key;
    overview.last_day_key = sorted.back().day_key;
    overview.average_per_day = static_cast<double>(overview.total_chars) / static_cast<double>(sorted.size());
    overview.current_streak = CurrentStreak(sorted, today_day_key);
    overview.longest_streak = LongestStreak(sorted);
    const std::pair<int, int64_t> best = BestDay(sorted);
    overview.best_day_key = best.first;
    overview.best_day_chars = best.second;
    overview.today_speed = CharsPerMinute(today_speed_chars, overview.today_active_ms);
    overview.average_speed = CharsPerMinute(total_speed_chars, overview.total_active_ms);
    const std::pair<int, double> fastest = FastestDay(sorted);
    overview.fastest_day_key = fastest.first;
    overview.fastest_speed = fastest.second;
    return overview;
}

int CurrentStreak(const std::vector<DailyRow> &rows, int today_day_key)
{
    if (rows.empty())
    {
        return 0;
    }
    std::set<int> recorded;
    for (const DailyRow &row : rows)
    {
        recorded.insert(row.day_key);
    }
    int current = today_day_key;
    if (recorded.find(today_day_key) == recorded.end())
    {
        current = AddDays(today_day_key, -1);
    }
    int streak = 0;
    while (recorded.find(current) != recorded.end())
    {
        ++streak;
        current = AddDays(current, -1);
    }
    return streak;
}

int LongestStreak(const std::vector<DailyRow> &rows)
{
    if (rows.empty())
    {
        return 0;
    }
    std::vector<int> keys;
    keys.reserve(rows.size());
    for (const DailyRow &row : rows)
    {
        keys.push_back(row.day_key);
    }
    std::sort(keys.begin(), keys.end());

    int longest = 1;
    int run = 1;
    for (size_t index = 1; index < keys.size(); ++index)
    {
        if (keys[index] == AddDays(keys[index - 1], 1))
        {
            ++run;
        }
        else if (keys[index] == keys[index - 1])
        {
            continue;
        }
        else
        {
            run = 1;
        }
        if (run > longest)
        {
            longest = run;
        }
    }
    return longest;
}
} // namespace MsimeStats
