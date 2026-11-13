#pragma once

#include "../core/scheme_type.h"
#include "../core/word_item.h"
#include "../shuangpin/shuangpin_profile.h"

#include <string>
#include <vector>

namespace metasequoia::local_modes
{
struct LocalDateTime
{
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned weekday = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
};

LocalDateTime current_local_date_time();
bool is_date_time_keyword(const std::string &keyword);
// Shift+T 后面那一串（不含 T）交给下面几个函数之前的归一：双拼下唤醒词的整拼要按方案读（小鹤 shijian 打成
// uijm、微软 xingqi 打成 x;qi），读出来是唤醒词时返回全拼。原样已是唤醒词（rq、sj、date）、全拼和五笔、
// 或读出来不是唤醒词（指定的日期时间、没打完的词）时原样返回。
std::string date_time_query_input(const std::string &input, SchemeType scheme, const ShuangpinProfile &profile);
// Shift+T 后面那一串（不含 T）出得了候选：是唤醒词，或者是写完了、取值也对的指定日期时间
// （20241225、2024/12/25、12/25、14:30 等，形状见 contracts/date_time_input.h）。只写月日时年份取 now 的。
bool is_date_time_query(const std::string &input, const LocalDateTime *now = nullptr);
// input 是唤醒词时按 now 出整组格式，是指定的日期时间时只出写到了的部分：只写日期不给带时分的格式，
// 只写年月只给年月的几种，写了秒就不给丢掉秒的格式。
// 每条候选的 WordItem::pinyin 是它的格式 ID（如 "date:ymd_dash"）。调频、置顶、固定位置都按格式 ID
// 记：日期文本每天都变，按文字记，明天就对不上了。指定的日期时间沿用同一套 ID，和唤醒词共用学到的顺序。
std::vector<WordItem> query_date_time(const std::string &input, const LocalDateTime *now = nullptr, int limit = 17);
// 所属的一组格式："date" / "time" / "week"，都不是时为空。rq / riqi / date 同属一组，共用一份学到的
// 顺序；指定的日期归 "date"，指定的时间和日期加时间归 "time"。
std::string date_time_category(const std::string &input);
// 这组格式的全部 ID，按出厂顺序。包括此刻给不出文本、query_date_time 跳过的那些。
std::vector<std::string> date_time_format_ids(const std::string &keyword);

// 混输里展开全部格式的那个入口候选。它不上屏：选中时候选框换成 query_date_time 的整组格式。
// pinyin 是前缀加唤醒词，展开时据此重查。
inline constexpr const char *kDateTimeMenuPrefix = "menu:";
WordItem date_time_menu_item(const std::string &keyword);
bool is_date_time_menu_item(const WordItem &item);
std::string date_time_menu_keyword(const WordItem &item);
} // namespace metasequoia::local_modes
