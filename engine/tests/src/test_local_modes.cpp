#include "../../local_modes/date_time_query.h"
#include "../../local_modes/emoji_query.h"
#include "../../local_modes/kaomoji_query.h"
#include "../../local_modes/quick_phrase_query.h"
#include "../../local_modes/radical_query.h"
#include "../../local_modes/v_mode_query.h"
#include "../../core/data_path.h"

#include <sqlite3.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <cstdio>

namespace
{
using metasequoia::local_modes::LocalDateTime;

class Database
{
  public:
    explicit Database(const std::filesystem::path &path)
    {
        if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database_) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to create the quick-phrase test database.");
        }
    }

    ~Database()
    {
        sqlite3_close(database_);
    }

    void execute(const char *sql)
    {
        char *error = nullptr;
        if (sqlite3_exec(database_, sql, nullptr, nullptr, &error) != SQLITE_OK)
        {
            const std::string message = error == nullptr ? "SQLite operation failed." : error;
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

  private:
    sqlite3 *database_ = nullptr;
};

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

LocalDateTime sample_time()
{
    return {2026, 8, 9, 0, 14, 30, 0};
}

template <std::size_t Size>
void require_words(const std::vector<WordItem> &actual, const std::array<const char *, Size> &expected,
                   const char *message)
{
    require(actual.size() == expected.size(), message);
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        require(actual[index].word == expected[index] && actual[index].source == CandidateSource::Generated &&
                    actual[index].weight == static_cast<std::int64_t>(expected.size() - index),
                message);
    }
}
} // namespace

int run_test()
{
    const LocalDateTime now = sample_time();
    const std::array<const char *, 17> expected_dates = {
        "2026年8月9日",
        "2026-08-09",
        "2026/08/09",
        "2026.08.09",
        "20260809",
        "26年8月9日",
        "8月9日",
        "08-09",
        "0809",
        "2026年8月9日 星期日",
        "8月9日 周日",
        "2026-08-09 Sun",
        "2026-08-09 14:30",
        "8月9日 14:30",
        "二〇二六年八月九日",
        "贰零贰陆年捌月零玖日",
        "丙午年六月二十七日",
    };
    for (const char *keyword : std::array<const char *, 3>{"rq", "riqi", "date"})
    {
        require_words(metasequoia::local_modes::query_date_time(keyword, &now), expected_dates,
                      "A date alias did not preserve the Windows candidate order.");
    }

    const std::array<const char *, 13> expected_times = {
        "14:30",
        "14:30:00",
        "1430",
        "143000",
        "下午2:30",
        "下午2点30分",
        "下午两点半",
        "2:30 PM",
        "2:30pm",
        "02:30:00 PM",
        "2026-08-09 14:30:00",
        "2026年8月9日 14:30",
        "8月9日 下午2:30",
    };
    for (const char *keyword : std::array<const char *, 3>{"sj", "shijian", "time"})
    {
        require_words(metasequoia::local_modes::query_date_time(keyword, &now), expected_times,
                      "A time alias did not preserve the Windows candidate order.");
    }

    const std::array<const char *, 4> expected_sunday = {"星期日", "星期天", "Sunday", "Sun"};
    for (const char *keyword : std::array<const char *, 3>{"xq", "xingqi", "week"})
    {
        require_words(metasequoia::local_modes::query_date_time(keyword, &now), expected_sunday,
                      "A weekday alias did not preserve the Windows candidate order.");
    }

    LocalDateTime monday = now;
    monday.weekday = 1;
    const std::array<const char *, 3> expected_monday = {"星期一", "Monday", "Mon"};
    require_words(metasequoia::local_modes::query_date_time("week", &monday), expected_monday,
                  "Monday candidates were formatted incorrectly.");

    require(!metasequoia::local_modes::is_date_time_keyword("today") &&
                metasequoia::local_modes::is_date_time_keyword("week"),
            "Date/time keyword recognition diverged from Windows.");
    require(metasequoia::local_modes::query_date_time("today", &now).empty() &&
                metasequoia::local_modes::query_date_time("rq", &now, 0).empty() &&
                metasequoia::local_modes::query_date_time("rq", &now, -1).empty() &&
                metasequoia::local_modes::query_date_time("rq", &now, 3).size() == 3,
            "Date/time query limit or unknown-keyword handling was incorrect.");

    // 指定的日期：星期按那一天算，不带此刻的时分。
    const std::array<const char *, 15> expected_christmas = {
        "2024年12月25日",
        "2024-12-25",
        "2024/12/25",
        "2024.12.25",
        "20241225",
        "24年12月25日",
        "12月25日",
        "12-25",
        "1225",
        "2024年12月25日 星期三",
        "12月25日 周三",
        "2024-12-25 Wed",
        "二〇二四年十二月二十五日",
        "贰零贰肆年壹贰月贰伍日",
        "甲辰年十一月二十五日",
    };
    for (const char *input : std::array<const char *, 2>{"20241225", "2024/12/25"})
    {
        require_words(metasequoia::local_modes::query_date_time(input, &now), expected_christmas,
                      "A specific date did not produce the date formats for that day.");
    }
    const auto month_day = metasequoia::local_modes::query_date_time("1/5", &now);
    require(!month_day.empty() && month_day.front().word == "2026年1月5日" &&
                month_day[9].word == "2026年1月5日 星期一" && month_day.front().pinyin == "date:ymd_cn",
            "A month/day input did not take the current year.");
    const std::array<const char *, 5> expected_year_month = {"2024年12月", "2024-12", "2024/12", "2024.12",
                                                             "二〇二四年十二月"};
    require_words(metasequoia::local_modes::query_date_time("2024/12", &now), expected_year_month,
                  "A year/month input did not produce the year/month formats.");

    // 指定的时间：写了秒就只给带秒的格式，没写秒就不补 :00；日期加时间只给带日期的格式。
    const std::array<const char *, 7> expected_morning = {
        "09:05", "0905", "上午9:05", "上午9点05分", "上午九点五分", "9:05 AM", "9:05am",
    };
    require_words(metasequoia::local_modes::query_date_time("9:05", &now), expected_morning,
                  "A specific time did not produce the hour/minute formats.");
    const std::array<const char *, 3> expected_seconds = {"21:05:09", "210509", "09:05:09 PM"};
    require_words(metasequoia::local_modes::query_date_time("21:05:09", &now), expected_seconds,
                  "A specific time with seconds did not keep the seconds.");
    const std::array<const char *, 3> expected_date_time = {"2024-12-25 14:30:00", "2024年12月25日 14:30",
                                                            "12月25日 下午2:30"};
    require_words(metasequoia::local_modes::query_date_time("2024122514:30", &now), expected_date_time,
                  "A specific date and time did not produce the combined formats.");
    const std::array<const char *, 1> expected_date_time_seconds = {"2024-12-25 08:00:07"};
    require_words(metasequoia::local_modes::query_date_time("2024122508:00:07", &now), expected_date_time_seconds,
                  "A specific date and time with seconds dropped the seconds.");
    require(metasequoia::local_modes::query_date_time("2024122508", &now).front().word == "2024-12-25 08:00:00",
            "A date with only the hour did not default the minutes to zero.");

    // 取值不对、没写完或形状不对都不出候选，也不算查询。
    for (const char *input : std::array<const char *, 9>{"20241325", "20230229", "2024/2/30", "24:00", "9:60", "1225",
                                                         "2024/", "12/25/1", "2024122514:30:61"})
    {
        require(metasequoia::local_modes::query_date_time(input, &now).empty() &&
                    !metasequoia::local_modes::is_date_time_query(input, &now),
                "An invalid or incomplete date/time input produced candidates.");
    }
    require(metasequoia::local_modes::is_date_time_query("20240229", &now) &&
                metasequoia::local_modes::is_date_time_query("rq", &now) &&
                metasequoia::local_modes::date_time_category("2024/12") == "date" &&
                metasequoia::local_modes::date_time_category("2024122514") == "time" &&
                metasequoia::local_modes::date_time_category("9:05") == "time",
            "Specific date/time recognition or grouping was incorrect.");

    // V 模式：数字转中文。
    const std::array<const char *, 4> expected_v_integer = {"一百二十三", "壹佰贰拾叁", "壹佰贰拾叁元整", "一二三"};
    require_words(metasequoia::local_modes::query_v_mode("123"), expected_v_integer,
                  "V mode did not convert an integer to Chinese.");
    const std::array<const char *, 3> expected_v_money = {"壹佰贰拾叁元肆角伍分", "一百二十三点四五",
                                                          "壹佰贰拾叁点肆伍"};
    require_words(metasequoia::local_modes::query_v_mode("123.45"), expected_v_money,
                  "V mode did not put the money form first for a decimal.");
    const std::array<const char *, 5> expected_v_thousands = {"一万二千三百四十五", "壹万贰仟叁佰肆拾伍",
                                                              "壹万贰仟叁佰肆拾伍元整", "一二三四五", "12,345"};
    require_words(metasequoia::local_modes::query_v_mode("12345"), expected_v_thousands,
                  "V mode did not add the thousands-separated form.");
    using metasequoia::local_modes::chinese_financial_number;
    using metasequoia::local_modes::chinese_money;
    using metasequoia::local_modes::chinese_number_reading;
    const std::array<std::array<const char *, 2>, 12> readings = {{
        {"0", "零"},
        {"10", "十"},
        {"15", "十五"},
        {"110", "一百一十"},
        {"1001", "一千零一"},
        {"10010", "一万零一十"},
        {"100000", "十万"},
        {"20000", "二万"},
        {"100000001", "一亿零一"},
        {"1000010000", "十亿零一万"},
        {"007", "七"},
        {"1234567890123456", "一千二百三十四万亿五千六百七十八亿九千零一十二万三千四百五十六"},
    }};
    for (const auto &reading : readings)
        require(chinese_number_reading(reading[0]) == reading[1], "A Chinese number reading was wrong.");
    require(chinese_number_reading("12345678901234567").empty() && chinese_financial_number("10") == "壹拾" &&
                chinese_financial_number("10010") == "壹万零壹拾",
            "Chinese financial numbers or the reading limit were wrong.");
    require(chinese_money("100", "05") == "壹佰元零伍分" && chinese_money("0", "45") == "肆角伍分" &&
                chinese_money("0", "05") == "伍分" && chinese_money("123", "4") == "壹佰贰拾叁元肆角" &&
                chinese_money("0", "") == "零元整" && chinese_money("1", "456").empty(),
            "Chinese money amounts were wrong.");

    // V 模式：算式计算。
    const std::array<const char *, 2> expected_v_expression = {"7", "1+2*3=7"};
    require_words(metasequoia::local_modes::query_v_mode("1+2*3"), expected_v_expression,
                  "V mode did not evaluate an arithmetic expression.");
    const std::array<std::array<const char *, 2>, 7> expressions = {{
        {"(1+2)*3", "9"},
        {"10/4", "2.5"},
        {"0.1+0.2", "0.3"},
        {"1/3", "0.3333333333"},
        {"2*-3", "-6"},
        {"-(2-5)", "3"},
        {"7-7", "0"},
    }};
    for (const auto &expression : expressions)
    {
        std::string result;
        require(metasequoia::local_modes::evaluate_v_mode_expression(expression[0], result) && result == expression[1],
                "An arithmetic expression evaluated to the wrong result.");
    }
    for (const char *input :
         std::array<const char *, 9>{"1/0", "1+", "(1+2", "1+2)", "1..2", "1.", "1.2.3", "12a", "2(3)"})
    {
        require(metasequoia::local_modes::query_v_mode(input).empty(),
                "An incomplete or invalid V-mode input produced candidates.");
    }

    const auto suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::filesystem::path quick_phrase_directory =
        std::filesystem::temp_directory_path() / ("metasequoia-quick-phrase-" + suffix);
    std::filesystem::create_directories(quick_phrase_directory);
    const std::filesystem::path quick_phrase_database = quick_phrase_directory / "msime.db";
    {
        Database database(quick_phrase_database);
        database.execute("CREATE TABLE quick_parases(key TEXT,value TEXT,weight INTEGER)");
        database.execute("INSERT INTO quick_parases VALUES('ab','highest weight',20)");
        database.execute("INSERT INTO quick_parases VALUES('aa','tie b',10)");
        database.execute("INSERT INTO quick_parases VALUES('aa','tie a',10)");
        database.execute("INSERT INTO quick_parases VALUES('b','outside prefix',100)");
    }

    const auto quick_phrases = metasequoia::local_modes::query_quick_phrases("a", quick_phrase_database, 10);
    require(!quick_phrases.diagnostic.has_value() && quick_phrases.candidates.size() == 3 &&
                quick_phrases.candidates[0].pinyin == "ab" && quick_phrases.candidates[0].word == "highest weight" &&
                quick_phrases.candidates[1].pinyin == "aa" && quick_phrases.candidates[1].word == "tie a" &&
                quick_phrases.candidates[2].word == "tie b" &&
                quick_phrases.candidates[0].source == CandidateSource::QuickPhrase,
            "Quick-phrase prefix ordering diverged from Windows.");
    const auto limited_quick_phrases = metasequoia::local_modes::query_quick_phrases("a", quick_phrase_database, 2);
    require(!limited_quick_phrases.diagnostic.has_value() && limited_quick_phrases.candidates.size() == 2,
            "Quick-phrase query did not honor its limit.");
    require(metasequoia::local_modes::query_quick_phrases("", quick_phrase_database, 10).candidates.empty() &&
                metasequoia::local_modes::query_quick_phrases("A", quick_phrase_database, 10).candidates.empty() &&
                metasequoia::local_modes::query_quick_phrases("a1", quick_phrase_database, 10).candidates.empty() &&
                metasequoia::local_modes::query_quick_phrases("a", quick_phrase_database, 0).candidates.empty(),
            "Quick-phrase query accepted an invalid prefix or limit.");

    const auto exact_quick_phrases =
        metasequoia::local_modes::query_quick_phrases_by_code("aa", quick_phrase_database, 10);
    require(!exact_quick_phrases.diagnostic.has_value() && exact_quick_phrases.candidates.size() == 2 &&
                exact_quick_phrases.candidates[0].word == "tie a" && exact_quick_phrases.candidates[1].word == "tie b",
            "Exact quick-phrase query diverged from its code.");
    require(
        metasequoia::local_modes::query_quick_phrases_by_code("a", quick_phrase_database, 10).candidates.empty() &&
            metasequoia::local_modes::query_quick_phrases_by_code("A", quick_phrase_database, 10).candidates.empty() &&
            metasequoia::local_modes::query_quick_phrases_by_code("aa", quick_phrase_database, 0).candidates.empty(),
        "Exact quick-phrase query matched a prefix or accepted an invalid code or limit.");

    const auto missing_quick_phrases =
        metasequoia::local_modes::query_quick_phrases("secret", quick_phrase_directory / "private-missing.db", 10);
    require(missing_quick_phrases.candidates.empty() && missing_quick_phrases.diagnostic.has_value() &&
                missing_quick_phrases.diagnostic->find("secret") == std::string::npos &&
                missing_quick_phrases.diagnostic->find("private-missing") == std::string::npos,
            "A missing quick-phrase database lacked a privacy-safe diagnostic.");

    const std::filesystem::path corrupt_database = quick_phrase_directory / "private-corrupt.db";
    {
        std::ofstream stream(corrupt_database, std::ios::binary);
        stream << "not a sqlite database";
    }
    const auto corrupt_quick_phrases = metasequoia::local_modes::query_quick_phrases("secret", corrupt_database, 10);
    require(corrupt_quick_phrases.candidates.empty() && corrupt_quick_phrases.diagnostic.has_value() &&
                corrupt_quick_phrases.diagnostic->find("secret") == std::string::npos &&
                corrupt_quick_phrases.diagnostic->find("private-corrupt") == std::string::npos,
            "A corrupt quick-phrase database lacked a privacy-safe diagnostic.");

    const std::filesystem::path others_database = quick_phrase_directory / "others.db";
    {
        Database database(others_database);
        database.execute("CREATE TABLE emoji_pinyin(key TEXT,emoji TEXT,sort_order INTEGER)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xiaolian','😀',10)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xiaolian','😄',20)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xiaolian','😀',30)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xl','😀',10)");
        database.execute("INSERT INTO emoji_pinyin VALUES('laugh','😀',10)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xnlm','raw shuangpin match',40)");
        database.execute("CREATE TABLE kaomoji(pinyin TEXT,jianpin TEXT,kaomoji TEXT,sort_order INTEGER)");
        database.execute("INSERT INTO kaomoji VALUES('haixiu','hx','(*/ω＼*)',10)");
        database.execute("INSERT INTO kaomoji VALUES('haixiu','hx','(^_^)',20)");
        database.execute("INSERT INTO kaomoji VALUES('haixiu','hx','(*/ω＼*)',30)");
        database.execute("INSERT INTO kaomoji VALUES('kiss','','( ˘ ³˘)♥',40)");
        database.execute("INSERT INTO kaomoji VALUES('kind','','single prefix',50)");
    }

    const auto emoji = metasequoia::local_modes::query_emoji("XIAOLIAN", SchemeType::Quanpin, others_database, 10);
    require(!emoji.diagnostic.has_value() && emoji.candidates.size() == 2 && emoji.candidates[0].word == "😀" &&
                emoji.candidates[1].word == "😄" && emoji.candidates[0].pinyin == "xiaolian" &&
                emoji.candidates[0].source == CandidateSource::Emoji,
            "Emoji lookup did not normalize, order, or deduplicate full-pinyin matches.");
    require(
        metasequoia::local_modes::query_emoji("xl", SchemeType::Quanpin, others_database, 10).candidates.front().word ==
                "😀" &&
            metasequoia::local_modes::query_emoji("laugh", SchemeType::Quanpin, others_database, 10)
                    .candidates.front()
                    .word == "😀",
        "Emoji lookup did not support jianpin and English keywords.");
    const auto shuangpin_emoji =
        metasequoia::local_modes::query_emoji("xnlm", SchemeType::Shuangpin, others_database, 10);
    require(shuangpin_emoji.candidates.size() == 3 && shuangpin_emoji.candidates[0].word == "😀" &&
                shuangpin_emoji.candidates[1].word == "😄" &&
                shuangpin_emoji.candidates[2].word == "raw shuangpin match",
            "Emoji lookup did not merge raw and Xiaohe-shuangpin prefixes in catalog order.");
    require(
        metasequoia::local_modes::query_emoji("xiaolian", SchemeType::Quanpin, others_database, 1).candidates.size() ==
                1 &&
            metasequoia::local_modes::query_emoji("", SchemeType::Quanpin, others_database, 10).candidates.empty() &&
            metasequoia::local_modes::query_emoji("bad1", SchemeType::Quanpin, others_database, 10)
                .candidates.empty() &&
            metasequoia::local_modes::query_emoji("x", SchemeType::Quanpin, others_database, 0).candidates.empty(),
        "Emoji lookup did not honor its validation and limit rules.");

    const auto kaomoji = metasequoia::local_modes::query_kaomoji("HAIXIU", SchemeType::Quanpin, others_database, 10);
    require(!kaomoji.diagnostic.has_value() && kaomoji.candidates.size() == 2 &&
                kaomoji.candidates[0].word == "(*/ω＼*)" && kaomoji.candidates[1].word == "(^_^)" &&
                kaomoji.candidates[0].pinyin == "haixiu" && kaomoji.candidates[0].source == CandidateSource::Kaomoji,
            "Kaomoji lookup did not normalize, order, or deduplicate full-pinyin matches.");
    require(
        metasequoia::local_modes::query_kaomoji("hx", SchemeType::Quanpin, others_database, 10)
                    .candidates.front()
                    .word == "(*/ω＼*)" &&
            metasequoia::local_modes::query_kaomoji("kiss", SchemeType::Quanpin, others_database, 10)
                    .candidates.front()
                    .word == "( ˘ ³˘)♥" &&
            !metasequoia::local_modes::query_kaomoji("k", SchemeType::Quanpin, others_database, 10).candidates.empty(),
        "Kaomoji lookup did not support jianpin, English, and single-letter prefixes.");
    const auto shuangpin_kaomoji =
        metasequoia::local_modes::query_kaomoji("hx", SchemeType::Shuangpin, others_database, 10);
    require(shuangpin_kaomoji.candidates.size() == 2 && shuangpin_kaomoji.candidates.front().word == "(*/ω＼*)",
            "Kaomoji lookup did not expand Xiaohe shuangpin or deduplicate merged matches.");

    const auto missing_emoji = metasequoia::local_modes::query_emoji(
        "privatecode", SchemeType::Quanpin, quick_phrase_directory / "private-others-missing.db", 10);
    require(missing_emoji.candidates.empty() && missing_emoji.diagnostic.has_value() &&
                missing_emoji.diagnostic->find("privatecode") == std::string::npos &&
                missing_emoji.diagnostic->find("private-others-missing") == std::string::npos,
            "A missing Emoji database lacked a privacy-safe diagnostic.");
    const auto corrupt_kaomoji =
        metasequoia::local_modes::query_kaomoji("privatecode", SchemeType::Quanpin, corrupt_database, 10);
    require(corrupt_kaomoji.candidates.empty() && corrupt_kaomoji.diagnostic.has_value() &&
                corrupt_kaomoji.diagnostic->find("privatecode") == std::string::npos &&
                corrupt_kaomoji.diagnostic->find("private-corrupt") == std::string::npos,
            "A corrupt kaomoji database lacked a privacy-safe diagnostic.");
    // U 模式：拆字表按「码前缀」查，命中按表内顺序（字频降序）取前 limit 条。
    const std::filesystem::path decomposition = quick_phrase_directory / "decomposition-fixture.txt";
    {
        std::ofstream stream(decomposition, std::ios::binary);
        // 故意带上首行 BOM、行尾 CRLF、末尾注释行、少列行和字频非整数的行：加载器的几种防御
        // 都要能过，且被丢弃的行不能变成候选。
        stream << "\xEF\xBB\xBFheng\t一\t18584\r\n"
                  "hengheng\t丁\t900\r\n"
                  "ac\t乙\t500\r\n"
                  "ab\t甲\t400\r\n"
                  "ririri\t晶\t106\r\n"
                  "ririri\t晿\t1\r\n"
                  "riririri\t𣊭\t1\r\n"
                  "koukoukou\t品\t500\r\n"
                  "koukou\t吅\t300\r\n"
                  "ma\t马\t50\r\n"
                  "kou\t口\t10\r\n"
                  "majiong\t駉\t5\r\n"
                  "majiongkou\t駉\t5\r\n"
                  "matongkou\t駉\t4\r\n"
                  "# 末尾注释\r\n"
                  "heng\t少一列\r\n"
                  "heng\t字频非整数\txx\r\n";
    }
    const auto radical_prefixed = metasequoia::local_modes::query_radical(decomposition, "ririri", 50);
    require(radical_prefixed.size() == 3 && radical_prefixed[0].word == "晶" && radical_prefixed[1].word == "晿" &&
                radical_prefixed[2].word == "𣊭" && radical_prefixed[0].source == CandidateSource::Generated &&
                radical_prefixed[0].pinyin.empty(),
            "Radical lookup did not prefix-match in frequency order with the pinyin left to the server.");
    const auto radical_prefix_widening = metasequoia::local_modes::query_radical(decomposition, "heng", 50);
    require(radical_prefix_widening.size() == 2 && radical_prefix_widening[0].word == "一" &&
                radical_prefix_widening[0].weight == 18584 && radical_prefix_widening[1].word == "丁",
            "Radical lookup did not widen a short prefix or dropped the frequency column.");
    const auto radical_limit = metasequoia::local_modes::query_radical(decomposition, "heng", 1);
    require(radical_limit.size() == 1 && radical_limit[0].word == "一",
            "Radical lookup ignored its limit or ranked a shorter match below a longer one.");
    const auto radical_file_order = metasequoia::local_modes::query_radical(decomposition, "a", 50);
    require(radical_file_order.size() == 2 && radical_file_order[0].word == "乙" && radical_file_order[1].word == "甲",
            "Radical lookup ranked by spelling instead of the table's frequency order.");
    // 打全了某个字的拆法，它排在更高频的长码前面，limit 再小也挤不掉。
    const auto radical_exact_first = metasequoia::local_modes::query_radical(decomposition, "kou", 50);
    require(radical_exact_first.size() == 3 && radical_exact_first[0].word == "口" &&
                radical_exact_first[1].word == "品" && radical_exact_first[2].word == "吅" &&
                metasequoia::local_modes::query_radical(decomposition, "kou", 1).front().word == "口",
            "Radical lookup ranked a complete decomposition below longer, more frequent prefix matches.");
    // 一个字的几种拆法共享前缀时只出一次。
    const auto radical_deduplicated = metasequoia::local_modes::query_radical(decomposition, "ma", 50);
    require(radical_deduplicated.size() == 2 && radical_deduplicated[0].word == "马" &&
                radical_deduplicated[1].word == "駉" &&
                metasequoia::local_modes::query_radical(decomposition, "majiong", 50).size() == 1,
            "Radical lookup listed one character once per decomposition sharing the typed prefix.");
    require(
        metasequoia::local_modes::query_radical(decomposition, "", 50).empty() &&
            metasequoia::local_modes::query_radical(decomposition, "Ri", 50).empty() &&
            metasequoia::local_modes::query_radical(decomposition, "fa4", 50).empty() &&
            metasequoia::local_modes::query_radical(decomposition, "zz", 50).empty() &&
            metasequoia::local_modes::query_radical(decomposition, "heng", 0).empty() &&
            metasequoia::local_modes::query_radical(quick_phrase_directory / "private-missing.txt", "heng", 50).empty(),
        "Radical lookup accepted an empty, non-lowercase or unmatched code, a zero limit, or a missing table.");

    std::filesystem::remove_all(quick_phrase_directory);
    return 0;
}

int main()
{
    try
    {
        return run_test();
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "An exception escaped the test body.\n");
        return 1;
    }
}
