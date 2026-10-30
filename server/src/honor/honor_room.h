#pragma once

// 荣誉室的成就目录与结算规则。
//
// 数据全部来自本机统计数据库的每日与每小时计数，不读输入内容，也不联网。
// 成就一旦解锁就写进 stats_honor 表，之后按保留策略删掉的旧统计不会让它重新锁上。
//
// 这个头文件不包含任何 Windows 头：结算逻辑是纯函数，可被便携测试目标直接构建。

#include "statistics/stats_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace MsimeHonor
{
// 成就分四档，用杉树长成来命名，避免照搬铜银金。
enum class Tier
{
    Sprout, // 新芽
    Branch, // 枝叶
    Ring,   // 年轮
    Crown,  // 参天
};

const char *TierId(Tier tier);
const wchar_t *TierName(Tier tier);

// 成就展示单位，前端据此决定数字后缀与进度条的读法。
enum class Display
{
    Count,
    Percent,
    Speed,
    Days,
};

const char *DisplayId(Display display);

// 结算用到的聚合量，全部由每日与每小时计数推出。
enum class Metric
{
    TotalChars,
    CjkChars,
    LatinChars,
    PunctChars,
    DaysRecorded,
    LongestStreak,
    CurrentStreak,
    BestDayChars,
    TodayChars,
    TotalActiveMs,
    FastestSpeed,      // 字/分，只取活跃时长够长的那些天
    FirstWordPermille, // 首选命中率，千分比
    NightChars,        // 本地时间 0 点到 5 点之间上屏的字数
    MaxHourChars,      // 单小时最高上屏字数
};

struct Definition
{
    const char *id;
    Tier tier;
    Metric metric;
    int64_t target;
    // 比率与速度类成就的样本门槛，样本不足时只展示进度，不判解锁。
    int64_t min_sample = 0;
    Display display = Display::Count;
    const wchar_t *title;
    const wchar_t *description;
};

const std::vector<Definition> &Catalog();

// Metrics 是 Catalog 与统计行之间的唯一换算层。
struct Metrics
{
    int64_t total_chars = 0;
    int64_t cjk = 0;
    int64_t latin = 0;
    int64_t digit = 0;
    int64_t punct = 0;
    int64_t other = 0;
    int64_t days = 0;
    int current_streak = 0;
    int longest_streak = 0;
    int64_t today_chars = 0;
    int64_t best_day_chars = 0;
    int64_t total_active_ms = 0;
    double fastest_speed = 0.0;
    bool speed_sample = false;
    int64_t first_word_hits = 0;
    int64_t first_word_total = 0;
    int64_t night_chars = 0;
    int64_t max_hour_chars = 0;
};

Metrics ComputeMetrics(const std::vector<MsimeStats::DailyRow> &daily, const std::vector<MsimeStats::HourlyRow> &hourly,
                       int today_day_key);

// Unlock 与 stats_honor 的一行同形，直接用统计模块的类型，免得在两个命名空间之间来回转换。
using Unlock = MsimeStats::HonorUnlock;

struct Progress
{
    const Definition *definition = nullptr;
    // value 是原始进度，可能超过 target；进度条自己截断，百分比要显示真实值。
    int64_t value = 0;
    bool unlocked = false;
    bool sample_ok = true;
    int unlocked_day_key = 0;
};

// Evaluate 把统计行与已解锁记录合成一份完整清单。unlocks 里没有的成就只算进度，
// 是否真的落库由调用方决定：写入的那一天才是解锁日。
std::vector<Progress> Evaluate(const std::vector<MsimeStats::DailyRow> &daily,
                               const std::vector<MsimeStats::HourlyRow> &hourly, int today_day_key,
                               const std::vector<Unlock> &unlocks);
} // namespace MsimeHonor
