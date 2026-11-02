#pragma once

#include "../core/word_item.h"
#include "quanpin_utils.h"
#include "word_lattice.h"
#include <sqlite3.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace quanpin
{
// msime.db 的 latin_words：T恤、AA制、卡拉OK 这类拉丁字母加汉字的词。键里每个字母自成一段
// （t'xu、ka'la'o'k），按打出的字母串匹配，不看切分：kalaok 切成 ka'lao'k 也要认出卡拉OK。
// 词不多（一千来条），载入后整表放在内存里。旧词库没有这张表时就是空的。
// 出厂词由建库脚本写；用户在设置页加的词也写在这张表里，另记进用户日志的 user_latin_words，
// 换词库时由 replay 写回（见 user_dictionary::add_user_latin_words）。
struct LatinWord
{
    std::string code; // 键去掉分隔符，即要敲的字母串
    std::string key;
    std::string value;
    std::int64_t weight = 0;
    // 车牌（浙E、鄂A）：只在整串正好是它时单独出候选，不补全、不进整句。浙E 的 zhe'e 全是完整音节，
    // 进了词格会混进普通整句（就浙E养）；补全会让 zhe 冒出一串车牌。
    bool exact_only = false;

    bool operator==(const LatinWord &other) const
    {
        return key == other.key && value == other.value && weight == other.weight && exact_only == other.exact_only;
    }
};

// 建表语句，与 scripts/dictionary_build/pinyin_db.py 的 CREATE_LATIN_WORDS 同一结构。用户在设置页加词、
// 回放用户字母词时，词库可能是还没有这张表的旧版本，用它补建。
inline constexpr const char *kLatinWordsTableSql =
    "CREATE TABLE IF NOT EXISTS main.latin_words(\"key\" TEXT NOT NULL,\"value\" TEXT NOT NULL,"
    "\"weight\" INTEGER NOT NULL,\"exact_only\" INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(\"key\",\"value\"))"
    " WITHOUT ROWID";

class LatinWords
{
  public:
    void load(sqlite3 *db);
    bool empty() const
    {
        return words_.empty();
    }
    const std::vector<LatinWord> &all() const
    {
        return words_;
    }
    // 编码与 code 完全相同的词，权重高的在前。
    std::vector<const LatinWord *> exact(const std::string &code) const;
    // 同上，但不含 exact_only 的词：词格里用这个。
    std::vector<const LatinWord *> exact_in_sentence(const std::string &code) const;
    // 编码以 code 开头、比它长的词（不含 exact_only），权重高的在前，至多 limit 个。
    std::vector<const LatinWord *> completions(const std::string &code, std::size_t limit) const;
    // segments 里每个不是完整音节的段（t、k）是否都落在某个整段对齐的字母词里。
    // 是的话词格能用字母词把这条切分解通，否则那些段只能当简拼，词格解不出整句。
    bool covers_incomplete_segments(const Segments &segments) const;

  private:
    // 按 code 升序，同 code 按权重降序。
    std::vector<LatinWord> words_;
    std::size_t max_code_length_ = 0;
};

// 词典对象持有的那一份字母词。词库被别的连接写过（data_version 不同于 loaded_version，或词典清缓存时
// 置了 stale）就重读；读出来和手上那份完全相同时沿用原对象，双拼按它反推的按键表也就不用重建——调频
// 每选一次词都会让 data_version 变一次，不能每次都重建。用 shared_ptr：重读时，正在解词格的那一轮仍
// 握着旧的一份。statement_cache 是词典的语句缓存（随词典析构 finalize），查 data_version 的语句放在里面。
std::shared_ptr<const LatinWords> refresh_latin_words(std::shared_ptr<const LatinWords> current, bool &stale,
                                                      sqlite3_int64 &loaded_version, sqlite3 *db,
                                                      std::unordered_map<std::string, sqlite3_stmt *> &statement_cache);

// 用户加的一条字母词，规范成 latin_words 的一行。
struct LatinEntry
{
    std::string key; // t'xu、ka'la'o'k：字母各一段（小写），汉字各一个完整音节
    std::string value;
    bool exact_only = false; // 车牌，见 LatinWord::exact_only
};

// 词只含英文字母和汉字、两样都有，且不太长。返回空串表示合格，否则是给用户看的原因。
std::string latin_word_problem(const std::string &word);
// 车牌：省份简称加一个大写字母（浙E、鄂A），与建库脚本的 _LICENSE_PLATE 同一规则。
bool is_license_plate(const std::string &word);
// 按读音规范一条字母词。读音的写法都认：t'xu（本输入法导出的格式）、T xu（Rime 用户词库）、
// kǎ'lā'OK（万象：字母串原样一段，汉字带声调）、xu4 这种数字调、不分隔的 txu。字母要与词里的字母
// 逐个对上，汉字各对一个完整音节；切法不唯一（fangan：fang'an / fan'gan）时要求用 ' 分开。
// 失败返回空，error 是给用户看的原因。
std::optional<LatinEntry> parse_latin_entry(const std::string &word, const std::string &reading, std::string &error);
// 汉字的读音已经按顺序给出（自动注音）：字母照抄，汉字依次取 han_syllables。
std::optional<LatinEntry> make_latin_entry(const std::string &word, const std::vector<std::string> &han_syllables,
                                           std::string &error);
// 词里依次每一段连续汉字（T恤衫 → 恤衫；A股B股 → 股、股），给自动注音按段注，免得字母打断上下文。
std::vector<std::string> latin_word_han_runs(const std::string &word);

// 一个字母配不出词，单敲 t 不该冒出 T台、T恤。
inline constexpr std::size_t kLatinWordMinimumCode = 2;
// 补全从三个键起才补，至多两条：tx 是常用简拼（同学、提醒），补全只会挤占它们。
inline constexpr std::size_t kLatinWordMinimumCompletionCode = 3;
inline constexpr std::size_t kLatinWordCompletionLimit = 2;

// 把整串命中（exact）和补全（completions）的字母词插进候选。整串命中和「键与输入完全相等」的词库行
// 同档，排在它们之后、整句之前；补全再让过紧跟着的那几条整句。词格对整串也会解出同一个词（txu 的
// 整句就是 T恤），那是猜出来的一行，换成这里的词条。selection_code 是选中后要吃掉的输入。
void insert_latin_candidates(std::vector<WordItem> &result, const std::vector<const LatinWord *> &exact,
                             const std::vector<const LatinWord *> &completions, const std::string &selection_code,
                             const Segments &typed_syllables);

// 词格的跨度查询里补上字母词：跨度里拼起来的字母等于某个字母词的编码时，把它放在 rows 最前
// （词格每个跨度只取前 span_limit 行）。词库查询本来就跳过含 t、k 这类不完整音节的跨度，所以
// 单字母段只能由字母词覆盖，不会被简拼展开成「他」「特」。
void prepend_latin_lexemes(const LatinWords &words, const Segments &span, const ngram::LanguageModel *model,
                           std::vector<LatticeLexeme> &rows);

// 一段切分里的字母拼起来（去掉分隔符）；混进非小写字母时返回空串。
std::string latin_code(const Segments &segments);
// 输入串去掉分隔符、转小写；混进字母以外的字符时返回空串。
std::string latin_code(const std::string &raw_input);
// 词里有拉丁字母：神经重排的字表里没有它们，见 merge_lattice_candidates。
bool contains_latin_letter(const std::string &text);
} // namespace quanpin
