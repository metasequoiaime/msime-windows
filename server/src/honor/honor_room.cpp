#include "honor/honor_room.h"

#include "statistics/stats_overview.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace MsimeHonor
{
namespace
{
// 深夜成就的终点：本地时间 0 点到 4 点 59 分。
constexpr int kNightLastHour = 4;

int64_t MetricValue(Metric metric, const Metrics &metrics)
{
    switch (metric)
    {
    case Metric::TotalChars:
        return metrics.total_chars;
    case Metric::CjkChars:
        return metrics.cjk;
    case Metric::LatinChars:
        return metrics.latin;
    case Metric::PunctChars:
        return metrics.punct;
    case Metric::DaysRecorded:
        return metrics.days;
    case Metric::LongestStreak:
        return metrics.longest_streak;
    case Metric::CurrentStreak:
        return metrics.current_streak;
    case Metric::BestDayChars:
        return metrics.best_day_chars;
    case Metric::TodayChars:
        return metrics.today_chars;
    case Metric::TotalActiveMs:
        return metrics.total_active_ms;
    case Metric::FastestSpeed:
        return static_cast<int64_t>(std::llround(metrics.fastest_speed));
    case Metric::FirstWordPermille:
        return metrics.first_word_total > 0 ? metrics.first_word_hits * 1000 / metrics.first_word_total : 0;
    case Metric::NightChars:
        return metrics.night_chars;
    case Metric::MaxHourChars:
        return metrics.max_hour_chars;
    }
    return 0;
}

bool SampleReady(const Definition &definition, const Metrics &metrics)
{
    switch (definition.metric)
    {
    case Metric::FastestSpeed:
        return metrics.speed_sample;
    case Metric::FirstWordPermille:
        return metrics.first_word_total >= definition.min_sample;
    default:
        return true;
    }
}
} // namespace

const char *TierId(Tier tier)
{
    switch (tier)
    {
    case Tier::Sprout:
        return "sprout";
    case Tier::Branch:
        return "branch";
    case Tier::Ring:
        return "ring";
    case Tier::Crown:
        return "crown";
    }
    return "sprout";
}

const wchar_t *TierName(Tier tier)
{
    switch (tier)
    {
    case Tier::Sprout:
        return L"新芽";
    case Tier::Branch:
        return L"枝叶";
    case Tier::Ring:
        return L"年轮";
    case Tier::Crown:
        return L"参天";
    }
    return L"新芽";
}

const char *DisplayId(Display display)
{
    switch (display)
    {
    case Display::Count:
        return "count";
    case Display::Percent:
        return "percent";
    case Display::Speed:
        return "speed";
    case Display::Days:
        return "days";
    }
    return "count";
}

const std::vector<Definition> &Catalog()
{
    // 门槛刻意按真实打字量铺开：新芽一天能拿完，参天要按年算。
    static const std::vector<Definition> catalog = {
        {"first_commit", Tier::Sprout, Metric::TotalChars, 1, 0, Display::Count, L"初次上屏", L"累计输入 1 个字。"},
        {"thousand_chars", Tier::Sprout, Metric::TotalChars, 1000, 0, Display::Count, L"千字文",
         L"累计输入 1,000 个字。"},
        {"first_day", Tier::Sprout, Metric::DaysRecorded, 1, 0, Display::Days, L"第一天", L"在统计里留下 1 天的记录。"},
        {"week_streak", Tier::Sprout, Metric::LongestStreak, 7, 0, Display::Days, L"一周不断更",
         L"连续 7 天有输入记录。"},

        {"ten_thousand", Tier::Branch, Metric::TotalChars, 10000, 0, Display::Count, L"万字户",
         L"累计输入 10,000 个字。"},
        {"hundred_thousand", Tier::Branch, Metric::TotalChars, 100000, 0, Display::Count, L"十万加",
         L"累计输入 100,000 个字。"},
        {"month_streak", Tier::Branch, Metric::LongestStreak, 30, 0, Display::Days, L"月满", L"连续 30 天有输入记录。"},
        {"busy_day", Tier::Branch, Metric::BestDayChars, 5000, 0, Display::Count, L"五千字的一天",
         L"单日输入达到 5,000 个字。"},
        {"hundred_days", Tier::Branch, Metric::DaysRecorded, 100, 0, Display::Days, L"百日积累", L"累计记录 100 天。"},
        {"first_word_80", Tier::Branch, Metric::FirstWordPermille, 800, 1000, Display::Percent, L"顺手拈来",
         L"首选命中率达到 80%，至少 1,000 次选词。"},

        {"million", Tier::Ring, Metric::TotalChars, 1000000, 0, Display::Count, L"百万字",
         L"累计输入 1,000,000 个字。"},
        {"quarter_streak", Tier::Ring, Metric::LongestStreak, 100, 0, Display::Days, L"百日不断",
         L"连续 100 天有输入记录。"},
        {"twenty_thousand_day", Tier::Ring, Metric::BestDayChars, 20000, 0, Display::Count, L"两万字的一天",
         L"单日输入达到 20,000 个字。"},
        {"year_days", Tier::Ring, Metric::DaysRecorded, 365, 0, Display::Days, L"一年之期", L"累计记录 365 天。"},
        {"night_owl", Tier::Ring, Metric::NightChars, 10000, 0, Display::Count, L"夜猫子",
         L"凌晨 0 点到 5 点累计输入 10,000 个字。"},
        {"punct_master", Tier::Ring, Metric::PunctChars, 50000, 0, Display::Count, L"标点富翁",
         L"累计输入 50,000 个标点。"},
        {"latin_master", Tier::Ring, Metric::LatinChars, 50000, 0, Display::Count, L"洋文通",
         L"累计输入 50,000 个英文字符。"},
        {"fast_hand", Tier::Ring, Metric::FastestSpeed, 100, 0, Display::Speed, L"快笔",
         L"最快速度达到每分钟 100 字。"},

        {"ten_million", Tier::Crown, Metric::TotalChars, 10000000, 0, Display::Count, L"千万字",
         L"累计输入 10,000,000 个字。"},
        {"year_streak", Tier::Crown, Metric::LongestStreak, 365, 0, Display::Days, L"全年无休",
         L"连续 365 天有输入记录。"},
        {"fifty_thousand_day", Tier::Crown, Metric::BestDayChars, 50000, 0, Display::Count, L"五万字的一天",
         L"单日输入达到 50,000 个字。"},
        {"first_word_95", Tier::Crown, Metric::FirstWordPermille, 950, 10000, Display::Percent, L"心领神会",
         L"首选命中率达到 95%，至少 10,000 次选词。"},
        {"night_owl_100k", Tier::Crown, Metric::NightChars, 100000, 0, Display::Count, L"深夜长明",
         L"凌晨 0 点到 5 点累计输入 100,000 个字。"},
        {"speed_200", Tier::Crown, Metric::FastestSpeed, 200, 0, Display::Speed, L"疾风",
         L"最快速度达到每分钟 200 字。"},
    };
    return catalog;
}

Metrics ComputeMetrics(const std::vector<MsimeStats::DailyRow> &daily, const std::vector<MsimeStats::HourlyRow> &hourly,
                       int today_day_key)
{
    std::vector<MsimeStats::DailyRow> sorted = daily;
    std::sort(sorted.begin(), sorted.end(), [](const MsimeStats::DailyRow &left, const MsimeStats::DailyRow &right) {
        return left.day_key < right.day_key;
    });
    // 只写了选词计数、没有字数也没有活跃时长的行不算打字日。选词路径独立写库，
    // 统计页也按同一条件过滤，否则「连续 N 天」会和统计页显示的天数对不上。
    std::vector<MsimeStats::DailyRow> typed;
    typed.reserve(sorted.size());
    for (const MsimeStats::DailyRow &row : sorted)
    {
        if (row.Total() > 0 || row.active_ms > 0)
            typed.push_back(row);
    }

    Metrics metrics;
    metrics.days = static_cast<int64_t>(typed.size());
    for (const MsimeStats::DailyRow &row : sorted)
    {
        metrics.total_chars += row.Total();
        metrics.cjk += row.cjk;
        metrics.latin += row.latin;
        metrics.digit += row.digit;
        metrics.punct += row.punct;
        metrics.other += row.other;
        metrics.total_active_ms += row.active_ms;
        metrics.first_word_hits += row.first_word_hits;
        metrics.first_word_total += row.first_word_total;
        metrics.best_day_chars = std::max(metrics.best_day_chars, row.Total());
        if (row.day_key == today_day_key)
        {
            metrics.today_chars += row.Total();
        }
        // 速度只在活跃时长够长的日子里比较，和统计页的口径一致。
        if (row.active_ms >= MsimeStats::kFastestSpeedMinActiveMs)
        {
            metrics.speed_sample = true;
            metrics.fastest_speed = std::max(metrics.fastest_speed, row.Speed());
        }
    }
    metrics.current_streak = MsimeStats::CurrentStreak(typed, today_day_key);
    metrics.longest_streak = MsimeStats::LongestStreak(typed);
    for (const MsimeStats::HourlyRow &row : hourly)
    {
        if (row.hour >= 0 && row.hour <= kNightLastHour)
        {
            metrics.night_chars += row.chars;
        }
        metrics.max_hour_chars = std::max(metrics.max_hour_chars, row.chars);
    }
    return metrics;
}

std::vector<Progress> Evaluate(const std::vector<MsimeStats::DailyRow> &daily,
                               const std::vector<MsimeStats::HourlyRow> &hourly, int today_day_key,
                               const std::vector<Unlock> &unlocks)
{
    const Metrics metrics = ComputeMetrics(daily, hourly, today_day_key);
    std::unordered_map<std::string, int> unlocked_days;
    unlocked_days.reserve(unlocks.size());
    for (const Unlock &unlock : unlocks)
    {
        unlocked_days.emplace(unlock.id, unlock.day_key);
    }

    std::vector<Progress> result;
    result.reserve(Catalog().size());
    for (const Definition &definition : Catalog())
    {
        Progress progress;
        progress.definition = &definition;
        progress.sample_ok = SampleReady(definition, metrics);
        const int64_t raw = MetricValue(definition.metric, metrics);
        progress.value = std::max(raw, int64_t{0});
        const auto found = unlocked_days.find(definition.id);
        if (found != unlocked_days.end())
        {
            progress.unlocked = true;
            progress.unlocked_day_key = found->second;
        }
        else
        {
            progress.unlocked = progress.sample_ok && raw >= definition.target;
        }
        result.push_back(progress);
    }
    return result;
}
} // namespace MsimeHonor
