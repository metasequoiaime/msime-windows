#include "date_time_query.h"

#include "../contracts/date_time_input.h"
#include "../shuangpin/shuangpin_query.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace metasequoia::local_modes
{
namespace
{
bool is_date_keyword(const std::string &keyword)
{
    return keyword == "rq" || keyword == "riqi" || keyword == "date";
}

bool is_time_keyword(const std::string &keyword)
{
    return keyword == "sj" || keyword == "shijian" || keyword == "time";
}

bool is_week_keyword(const std::string &keyword)
{
    return keyword == "xq" || keyword == "xingqi" || keyword == "week";
}

constexpr const char *kWeekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
constexpr const char *kShortWeekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
constexpr const char *kEnglishWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char *kEnglishFullWeekdays[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                                "Thursday", "Friday", "Saturday"};

unsigned weekday_index(const LocalDateTime &now)
{
    return std::min(now.weekday, 6U);
}

std::string format(const char *pattern, unsigned first, unsigned second = 0, unsigned third = 0)
{
    char buffer[96] = {};
    std::snprintf(buffer, sizeof(buffer), pattern, first, second, third);
    return buffer;
}

std::string chinese_digits(unsigned value)
{
    static constexpr const char *kDigits[] = {"〇", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    std::string result;
    for (const char digit : std::to_string(value))
    {
        result += kDigits[digit - '0'];
    }
    return result;
}

std::string chinese_number(unsigned value)
{
    static constexpr const char *kDigits[] = {"", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    if (value < 10)
    {
        return kDigits[value];
    }
    if (value == 10)
    {
        return "十";
    }
    if (value < 20)
    {
        return std::string("十") + kDigits[value - 10];
    }
    if (value % 10 == 0)
    {
        return std::string(kDigits[value / 10]) + "十";
    }
    return std::string(kDigits[value / 10]) + "十" + kDigits[value % 10];
}

std::string financial_digits(unsigned value, unsigned minimum_digits = 1)
{
    static constexpr const char *kDigits[] = {"零", "壹", "贰", "叁", "肆", "伍", "陆", "柒", "捌", "玖"};
    std::string digits = std::to_string(value);
    if (digits.size() < minimum_digits)
    {
        digits.insert(0, minimum_digits - digits.size(), '0');
    }

    std::string result;
    for (const char digit : digits)
    {
        result += kDigits[digit - '0'];
    }
    return result;
}

// Each entry encodes the month lengths and leap month of a Chinese lunar year.
// The table covers 1900-2100; 1900-01-31 is lunar 1900-01-01.
constexpr std::array<unsigned, 201> kLunarYearInfo = {
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2, 0x04ae0, 0x0a5b6, 0x0a4d0,
    0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977, 0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, 0x1ab54,
    0x02b60, 0x09570, 0x052f2, 0x04970, 0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7,
    0x0c950, 0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557, 0x06ca0, 0x0b550,
    0x15355, 0x04da0, 0x0a5b0, 0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0, 0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570,
    0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0, 0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540,
    0x0b6a0, 0x195a6, 0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570, 0x04af5,
    0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58, 0x055c0, 0x0ab60, 0x096d5, 0x092e0, 0x0c960, 0x0d954, 0x0d4a0, 0x0da50,
    0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5, 0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0,
    0x15176, 0x052b0, 0x0a930, 0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530,
    0x05aa0, 0x076a3, 0x096d0, 0x04bd7, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45, 0x0b5a0, 0x056d0, 0x055b2,
    0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0, 0x14b63, 0x09370, 0x049f8, 0x04970, 0x064b0, 0x168a6,
    0x0ea50, 0x06b20, 0x1a6c4, 0x0aae0, 0x092e0, 0x0d2e3, 0x0c960, 0x0d557, 0x0d4a0, 0x0da50, 0x05d55, 0x056a0, 0x0a6d0,
    0x055d4, 0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0, 0x0b6a6, 0x0ad50, 0x055a0, 0x0aba4, 0x0a5b0, 0x052b0, 0x0b273, 0x06930,
    0x07337, 0x06aa0, 0x0ad50, 0x14b55, 0x04b60, 0x0a570, 0x054e4, 0x0d260, 0x0e968, 0x0d520, 0x0daa0, 0x16aa6, 0x056d0,
    0x04ae0, 0x0a9d4, 0x0a4d0, 0x0d150, 0x0f252, 0x0d520,
};

unsigned lunar_month_days(unsigned year, unsigned month)
{
    return (kLunarYearInfo[year - 1900] & (0x10000U >> month)) ? 30 : 29;
}

unsigned lunar_leap_month(unsigned year)
{
    return kLunarYearInfo[year - 1900] & 0x0fU;
}

unsigned lunar_leap_days(unsigned year)
{
    return lunar_leap_month(year) == 0 ? 0 : ((kLunarYearInfo[year - 1900] & 0x10000U) ? 30 : 29);
}

unsigned lunar_year_days(unsigned year)
{
    unsigned days = 348 + lunar_leap_days(year);
    for (unsigned mask = 0x8000; mask > 0x8; mask >>= 1)
    {
        if ((kLunarYearInfo[year - 1900] & mask) != 0)
        {
            ++days;
        }
    }
    return days;
}

bool is_leap_year(unsigned year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

unsigned days_in_month(unsigned year, unsigned month)
{
    constexpr unsigned kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 0 || month > 12)
    {
        return 0;
    }
    return month == 2 && is_leap_year(year) ? 29 : kDays[month];
}

std::int64_t civil_day_number(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<std::int64_t>(era) * 146097 + day_of_era - 719468;
}

bool days_since_lunar_epoch(const LocalDateTime &date, unsigned &days)
{
    if (date.year == 0 || date.month == 0 || date.month > 12 || date.day == 0 ||
        date.day > days_in_month(date.year, date.month))
    {
        return false;
    }
    const std::int64_t epoch = civil_day_number(1900, 1, 31);
    const std::int64_t current = civil_day_number(static_cast<int>(date.year), date.month, date.day);
    if (current < epoch)
    {
        return false;
    }
    days = static_cast<unsigned>(current - epoch);
    return true;
}

std::string lunar_date(const LocalDateTime &now)
{
    unsigned remaining = 0;
    if (!days_since_lunar_epoch(now, remaining))
    {
        return {};
    }

    unsigned year = 1900;
    while (year <= 2100)
    {
        const unsigned year_days = lunar_year_days(year);
        if (remaining < year_days)
        {
            break;
        }
        remaining -= year_days;
        ++year;
    }
    if (year > 2100)
    {
        return {};
    }

    unsigned month = 1;
    bool is_leap = false;
    for (; month <= 12; ++month)
    {
        const unsigned month_days = lunar_month_days(year, month);
        if (remaining < month_days)
        {
            break;
        }
        remaining -= month_days;

        if (lunar_leap_month(year) == month)
        {
            const unsigned leap_days = lunar_leap_days(year);
            if (remaining < leap_days)
            {
                is_leap = true;
                break;
            }
            remaining -= leap_days;
        }
    }

    static constexpr const char *kStems[] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
    static constexpr const char *kBranches[] = {"子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"};
    return std::string(kStems[(year - 4) % 10]) + kBranches[(year - 4) % 12] + "年" + (is_leap ? "闰" : "") +
           chinese_number(month) + "月" + chinese_number(remaining + 1) + "日";
}

unsigned weekday_of(unsigned year, unsigned month, unsigned day)
{
    // 1970-01-01 是星期四。
    const std::int64_t days = civil_day_number(static_cast<int>(year), month, day);
    return static_cast<unsigned>(((days + 4) % 7 + 7) % 7);
}

// 一种格式：稳定的 ID 加上按此刻算出的文本。文本为空表示这种格式此刻给不出（农历越出表的范围、
// 星期天之外的「星期天」、指定日期时间里没写到的部分），候选里跳过它，ID 仍然留在格式表里，学到的
// 顺序不会因此错位。
struct DateTimeFormat
{
    const char *id;
    std::string text;
};

// 这组格式里哪些给得出：唤醒词取此刻，全都给；指定的日期时间只给写到了的部分，不拿此刻去补。
enum class DateTimeParts
{
    Now,
    Date,                // 年月日
    YearMonth,           // 只有年月
    Time,                // 时分
    TimeWithSeconds,     // 时分秒
    DateTime,            // 年月日加时分
    DateTimeWithSeconds, // 年月日加时分秒
};

std::string text_if(bool available, std::string text)
{
    return available ? std::move(text) : std::string{};
}

std::vector<DateTimeFormat> date_candidates(const LocalDateTime &now, DateTimeParts parts)
{
    const unsigned weekday = weekday_index(now);
    const bool date = parts == DateTimeParts::Now || parts == DateTimeParts::Date;
    const bool date_hm = parts == DateTimeParts::Now;
    const bool year_month = parts == DateTimeParts::YearMonth;
    // 年月几种排在最后：它们后加，学到的顺序里没记过的格式插回出厂下标，排在后面不会挤动旧顺序。
    return {
        {"date:ymd_cn", text_if(date, format("%u年%u月%u日", now.year, now.month, now.day))},
        {"date:ymd_dash", text_if(date, format("%04u-%02u-%02u", now.year, now.month, now.day))},
        {"date:ymd_slash", text_if(date, format("%04u/%02u/%02u", now.year, now.month, now.day))},
        {"date:ymd_dot", text_if(date, format("%04u.%02u.%02u", now.year, now.month, now.day))},
        {"date:ymd_compact", text_if(date, format("%04u%02u%02u", now.year, now.month, now.day))},
        {"date:yy_md_cn", text_if(date, format("%02u年%u月%u日", now.year % 100, now.month, now.day))},
        {"date:md_cn", text_if(date, format("%u月%u日", now.month, now.day))},
        {"date:md_dash", text_if(date, format("%02u-%02u", now.month, now.day))},
        {"date:md_compact", text_if(date, format("%02u%02u", now.month, now.day))},
        {"date:ymd_cn_week", text_if(date, format("%u年%u月%u日 ", now.year, now.month, now.day) + kWeekdays[weekday])},
        {"date:md_cn_week", text_if(date, format("%u月%u日 ", now.month, now.day) + kShortWeekdays[weekday])},
        {"date:ymd_dash_week",
         text_if(date, format("%04u-%02u-%02u ", now.year, now.month, now.day) + kEnglishWeekdays[weekday])},
        {"date:ymd_dash_hm", text_if(date_hm, format("%04u-%02u-%02u ", now.year, now.month, now.day) +
                                                  format("%02u:%02u", now.hour, now.minute))},
        {"date:md_cn_hm",
         text_if(date_hm, format("%u月%u日 ", now.month, now.day) + format("%02u:%02u", now.hour, now.minute))},
        {"date:ymd_chinese", text_if(date, chinese_digits(now.year) + "年" + chinese_number(now.month) + "月" +
                                               chinese_number(now.day) + "日")},
        {"date:ymd_financial", text_if(date, financial_digits(now.year) + "年" + financial_digits(now.month) + "月" +
                                                 financial_digits(now.day, 2) + "日")},
        {"date:lunar", text_if(date, lunar_date(now))},
        {"date:ym_cn", text_if(year_month, format("%u年%u月", now.year, now.month))},
        {"date:ym_dash", text_if(year_month, format("%04u-%02u", now.year, now.month))},
        {"date:ym_slash", text_if(year_month, format("%04u/%02u", now.year, now.month))},
        {"date:ym_dot", text_if(year_month, format("%04u.%02u", now.year, now.month))},
        {"date:ym_chinese", text_if(year_month, chinese_digits(now.year) + "年" + chinese_number(now.month) + "月")},
    };
}

std::vector<DateTimeFormat> time_candidates(const LocalDateTime &now, DateTimeParts parts)
{
    const unsigned hour12 = now.hour % 12 == 0 ? 12 : now.hour % 12;
    const std::string period = now.hour < 12 ? "上午" : "下午";
    const std::string meridiem_upper = now.hour < 12 ? "AM" : "PM";
    const std::string meridiem_lower = now.hour < 12 ? "am" : "pm";
    const std::string colloquial_hour = hour12 == 2 ? "两" : chinese_number(hour12);
    const std::string colloquial_minutes =
        now.minute == 0 ? "" : (now.minute == 30 ? "半" : chinese_number(now.minute) + "分");
    const bool now_parts = parts == DateTimeParts::Now;
    // 写了秒就不给丢掉秒的格式，没写秒就不给补出 :00 的格式；年月日加时间只给带日期的那几种。
    const bool hm = now_parts || parts == DateTimeParts::Time;
    const bool hms = now_parts || parts == DateTimeParts::TimeWithSeconds;
    const bool date_hms = now_parts || parts == DateTimeParts::DateTime || parts == DateTimeParts::DateTimeWithSeconds;
    const bool date_hm = now_parts || parts == DateTimeParts::DateTime;
    return {
        {"time:hm", text_if(hm, format("%02u:%02u", now.hour, now.minute))},
        {"time:hms", text_if(hms, format("%02u:%02u:%02u", now.hour, now.minute, now.second))},
        {"time:hm_compact", text_if(hm, format("%02u%02u", now.hour, now.minute))},
        {"time:hms_compact", text_if(hms, format("%02u%02u%02u", now.hour, now.minute, now.second))},
        {"time:period_hm", text_if(hm, period + format("%u:%02u", hour12, now.minute))},
        {"time:period_hm_cn", text_if(hm, period + format("%u点%02u分", hour12, now.minute))},
        {"time:period_colloquial", text_if(hm, period + colloquial_hour + "点" + colloquial_minutes)},
        {"time:hm_meridiem", text_if(hm, format("%u:%02u ", hour12, now.minute) + meridiem_upper)},
        {"time:hm_meridiem_lower", text_if(hm, format("%u:%02u", hour12, now.minute) + meridiem_lower)},
        {"time:hms_meridiem", text_if(hms, format("%02u:%02u:%02u ", hour12, now.minute, now.second) + meridiem_upper)},
        {"time:ymd_hms", text_if(date_hms, format("%04u-%02u-%02u ", now.year, now.month, now.day) +
                                               format("%02u:%02u:%02u", now.hour, now.minute, now.second))},
        {"time:ymd_cn_hm", text_if(date_hm, format("%u年%u月%u日 ", now.year, now.month, now.day) +
                                                format("%02u:%02u", now.hour, now.minute))},
        {"time:md_cn_period_hm",
         text_if(date_hm, format("%u月%u日 ", now.month, now.day) + period + format("%u:%02u", hour12, now.minute))},
    };
}

std::vector<DateTimeFormat> week_candidates(const LocalDateTime &now)
{
    const unsigned weekday = weekday_index(now);
    return {
        {"week:cn", kWeekdays[weekday]},
        {"week:cn_sunday", weekday == 0 ? "星期天" : ""},
        {"week:en_full", kEnglishFullWeekdays[weekday]},
        {"week:en", kEnglishWeekdays[weekday]},
    };
}

std::vector<DateTimeFormat> formats_for(const std::string &keyword, const LocalDateTime &now)
{
    if (is_date_keyword(keyword))
        return date_candidates(now, DateTimeParts::Now);
    if (is_time_keyword(keyword))
        return time_candidates(now, DateTimeParts::Now);
    if (is_week_keyword(keyword))
        return week_candidates(now);
    return {};
}

// T 后面写的指定日期时间（形状见 contracts/date_time_input.h）：解析出的值和写到了哪些部分。
struct SpecificDateTime
{
    LocalDateTime value;
    DateTimeParts parts = DateTimeParts::Now;
};

unsigned digits_value(const std::string &text, std::size_t start, std::size_t length)
{
    unsigned value = 0;
    for (std::size_t index = start; index < start + length; ++index)
        value = value * 10 + static_cast<unsigned>(text[index] - '0');
    return value;
}

// 只写月日时年份取 today 的；只写时间时日期也取 today 的，但那几种带日期的格式不会给出来。取值不对
// （13 月、2 月 30 日、25 点）时不认。
bool parse_specific(const std::string &body, const LocalDateTime &today, SpecificDateTime &result)
{
    using FanyImeDateTimeInput::Shape;
    const FanyImeDateTimeInput::Match match = FanyImeDateTimeInput::MatchComplete(body.data(), body.size());
    if (match.shape == Shape::None)
        return false;
    const auto group = [&](std::size_t index, std::size_t offset = 0, std::size_t length = 0) {
        const auto &range = match.groups[index];
        return digits_value(body, range.start + offset, length == 0 ? range.length : length);
    };
    const auto optional_group = [&](std::size_t index) { return index < match.group_count ? group(index) : 0U; };

    LocalDateTime value = today;
    value.hour = 0;
    value.minute = 0;
    value.second = 0;
    bool has_date = true;
    switch (match.shape)
    {
    case Shape::Date:
    case Shape::DateTime:
        value.year = group(0, 0, 4);
        value.month = group(0, 4, 2);
        value.day = group(0, 6, 2);
        if (match.shape == Shape::DateTime)
        {
            value.hour = group(0, 8, 2);
            value.minute = optional_group(1);
            value.second = optional_group(2);
            result.parts = match.group_count == 3 ? DateTimeParts::DateTimeWithSeconds : DateTimeParts::DateTime;
        }
        else
        {
            result.parts = DateTimeParts::Date;
        }
        break;
    case Shape::YearMonthDay:
    case Shape::YearMonth:
        value.year = group(0);
        value.month = group(1);
        value.day = match.shape == Shape::YearMonthDay ? group(2) : 1;
        result.parts = match.shape == Shape::YearMonthDay ? DateTimeParts::Date : DateTimeParts::YearMonth;
        break;
    case Shape::MonthDay:
        value.month = group(0);
        value.day = group(1);
        result.parts = DateTimeParts::Date;
        break;
    case Shape::Time:
        has_date = false;
        value.hour = group(0);
        value.minute = group(1);
        value.second = optional_group(2);
        result.parts = match.group_count == 3 ? DateTimeParts::TimeWithSeconds : DateTimeParts::Time;
        break;
    case Shape::None:
        return false;
    }

    if (has_date)
    {
        if (value.year == 0 || value.month == 0 || value.month > 12 || value.day == 0 ||
            value.day > days_in_month(value.year, value.month))
            return false;
        value.weekday = weekday_of(value.year, value.month, value.day);
    }
    if (value.hour > 23 || value.minute > 59 || value.second > 59)
        return false;
    result.value = value;
    return true;
}

std::vector<DateTimeFormat> specific_formats(const SpecificDateTime &specific)
{
    switch (specific.parts)
    {
    case DateTimeParts::Date:
    case DateTimeParts::YearMonth:
        return date_candidates(specific.value, specific.parts);
    case DateTimeParts::Time:
    case DateTimeParts::TimeWithSeconds:
    case DateTimeParts::DateTime:
    case DateTimeParts::DateTimeWithSeconds:
        return time_candidates(specific.value, specific.parts);
    case DateTimeParts::Now:
        break;
    }
    return {};
}

bool is_specific_input(const std::string &input)
{
    return FanyImeDateTimeInput::IsSpecificBody(input.data(), input.size());
}
} // namespace

LocalDateTime current_local_date_time()
{
    const std::time_t current = std::time(nullptr);
    std::tm local = {};
#ifdef _WIN32
    if (current == static_cast<std::time_t>(-1) || localtime_s(&local, &current) != 0)
#else
    if (current == static_cast<std::time_t>(-1) || localtime_r(&current, &local) == nullptr)
#endif
    {
        return {};
    }
    return {static_cast<unsigned>(local.tm_year + 1900), static_cast<unsigned>(local.tm_mon + 1),
            static_cast<unsigned>(local.tm_mday),        static_cast<unsigned>(local.tm_wday),
            static_cast<unsigned>(local.tm_hour),        static_cast<unsigned>(local.tm_min),
            static_cast<unsigned>(local.tm_sec)};
}

bool is_date_time_keyword(const std::string &keyword)
{
    return is_date_keyword(keyword) || is_time_keyword(keyword) || is_week_keyword(keyword);
}

std::string date_time_query_input(const std::string &input, SchemeType scheme, const ShuangpinProfile &profile)
{
    if (scheme != SchemeType::Shuangpin || is_date_time_keyword(input))
        return input;
    for (const std::string &quanpin : shuangpin::decode_syllable_pairs(input, profile))
    {
        if (is_date_time_keyword(quanpin))
            return quanpin;
    }
    return input;
}

bool is_date_time_query(const std::string &input, const LocalDateTime *now)
{
    if (is_date_time_keyword(input))
        return true;
    if (!is_specific_input(input))
        return false;
    SpecificDateTime specific;
    return parse_specific(input, now == nullptr ? current_local_date_time() : *now, specific);
}

std::vector<WordItem> query_date_time(const std::string &input, const LocalDateTime *now, int limit)
{
    if (limit <= 0)
    {
        return {};
    }

    const LocalDateTime current = now == nullptr ? current_local_date_time() : *now;
    std::vector<DateTimeFormat> candidates;
    SpecificDateTime specific;
    if (is_date_time_keyword(input))
        candidates = formats_for(input, current);
    else if (is_specific_input(input) && parse_specific(input, current, specific))
        candidates = specific_formats(specific);
    else
        return {};
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [](const DateTimeFormat &candidate) { return candidate.text.empty(); }),
                     candidates.end());
    const std::size_t result_count = std::min(candidates.size(), static_cast<std::size_t>(limit));
    std::vector<WordItem> results;
    results.reserve(result_count);
    for (std::size_t index = 0; index < result_count; ++index)
    {
        results.emplace_back(candidates[index].id, std::move(candidates[index].text),
                             static_cast<std::int64_t>(result_count - index), CandidateSource::Generated);
    }
    return results;
}

std::string date_time_category(const std::string &input)
{
    if (is_date_keyword(input))
        return "date";
    if (is_time_keyword(input))
        return "time";
    if (is_week_keyword(input))
        return "week";
    // 指定的日期时间按形状分组，不看取值：没写完或取值不对时反正没有候选，分组只用来套学到的顺序。
    switch (FanyImeDateTimeInput::MatchComplete(input.data(), input.size()).shape)
    {
    case FanyImeDateTimeInput::Shape::Date:
    case FanyImeDateTimeInput::Shape::YearMonthDay:
    case FanyImeDateTimeInput::Shape::YearMonth:
    case FanyImeDateTimeInput::Shape::MonthDay:
        return "date";
    case FanyImeDateTimeInput::Shape::DateTime:
    case FanyImeDateTimeInput::Shape::Time:
        return "time";
    case FanyImeDateTimeInput::Shape::None:
        break;
    }
    return {};
}

std::vector<std::string> date_time_format_ids(const std::string &keyword)
{
    std::vector<std::string> ids;
    for (const auto &candidate : formats_for(keyword, LocalDateTime{2000, 1, 1, 6, 0, 0, 0}))
        ids.emplace_back(candidate.id);
    return ids;
}

WordItem date_time_menu_item(const std::string &keyword)
{
    const std::string category = date_time_category(keyword);
    // 📅 / 🕒：和候选里的日期文本区分开，一眼看得出这是展开更多格式的入口，不是要上屏的字。
    const char *label = category == "time"   ? "\xF0\x9F\x95\x92时间"
                        : category == "week" ? "\xF0\x9F\x93\x85星期"
                                             : "\xF0\x9F\x93\x85日期";
    return WordItem(kDateTimeMenuPrefix + keyword, label, 0, CandidateSource::DateTime);
}

bool is_date_time_menu_item(const WordItem &item)
{
    return item.source == CandidateSource::DateTime && item.pinyin.rfind(kDateTimeMenuPrefix, 0) == 0;
}

std::string date_time_menu_keyword(const WordItem &item)
{
    return is_date_time_menu_item(item) ? item.pinyin.substr(std::char_traits<char>::length(kDateTimeMenuPrefix))
                                        : std::string{};
}
} // namespace metasequoia::local_modes
