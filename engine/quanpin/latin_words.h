#pragma once

#include "../core/word_item.h"
#include "quanpin_utils.h"
#include "word_lattice.h"
#include <sqlite3.h>
#include <cstdint>
#include <string>
#include <vector>

namespace quanpin
{
// msime.db 的 latin_words：T恤、AA制、卡拉OK 这类拉丁字母加汉字的词。键里每个字母自成一段
// （t'xu、ka'la'o'k），按打出的字母串匹配，不看切分：kalaok 切成 ka'lao'k 也要认出卡拉OK。
// 词不多（一千来条），载入后整表放在内存里。旧词库没有这张表时就是空的。
struct LatinWord
{
    std::string code; // 键去掉分隔符，即要敲的字母串
    std::string key;
    std::string value;
    std::int64_t weight = 0;
    // 车牌（浙E、鄂A）：只在整串正好是它时单独出候选，不补全、不进整句。浙E 的 zhe'e 全是完整音节，
    // 进了词格会混进普通整句（就浙E养）；补全会让 zhe 冒出一串车牌。
    bool exact_only = false;
};

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
