#pragma once

#include "../core/prefix_completion_options.h"
#include "../core/word_item.h"
#include "engine/ngram/language_model.h"
#include "engine/ngram/octagram/octagram_gram.h"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace neural
{
class SentenceModel;
}

// 长词前缀补全（#1343）：首选已经是四个字以上、覆盖整串输入的词时，到词库里找以它开头的更长的词
// （中华人民 → 中华人民共和国），挑最可能的一条插到首选后面。全拼、双拼共用，词库都是 msime.db 的
// tbl_* 分表。
namespace quanpin
{
using metasequoia::PrefixCompletionOptions;

// 给补全打分的模型，按优先级用第一个可用的：万象等 octagram 语法模型（设置页下载）→ 词格的 sc.lm
// （设置页下载）→ 随包的神经模型。都没有时只看词库权重。sc.lm 在时另外拿它切词挡残片。
struct PrefixCompletionScorers
{
    const gram::GramDb *collocation = nullptr;
    gram::GrammarConfig grammar;
    const ngram::LanguageModel *language_model = nullptr;
    const neural::SentenceModel *neural = nullptr;
};

// 实际给排序打分的是哪一个，评测工具与缓存键用。
enum class PrefixCompletionScorer
{
    None,
    Collocation,
    LanguageModel,
    Neural,
};
PrefixCompletionScorer active_prefix_completion_scorer(const PrefixCompletionScorers &scorers);

// 词库里以前缀开头、比它长的一行，连同打分的各项。
struct PrefixCompletionRow
{
    std::string key;   // 完整读音，zhong'hua'ren'min'gong'he'guo
    std::string value; // 中华人民共和国
    std::int64_t weight = 0;
    // 下面几项由 score_prefix_completions 填。
    double prior = 0.0; // 词库权重的 log10
    // 打分模型给「前缀后面接补出来那几个字」的分：万象是搭配强度（ln 域，未命中为 non_collocation_penalty），
    // sc.lm 与神经模型是条件概率（log10）。没有模型、或这一行没轮到神经模型打分时为 0。
    double tail = 0.0;
    bool scored = false;    // tail 是模型打出来的
    bool confident = false; // 过了门槛，可以显示
};

// 在 msime.db 里找键以 prefix_key 加一个分隔符开头的行；prefix_word 非空时值还要以它开头，为空时只看读音。
// 音节数从前缀加一查到 tbl_others_*，每张表至多 per_table_limit 行；值的字数要和键的音节数相等，且全是
// 汉字。同值只留权重最高的。
std::vector<PrefixCompletionRow> query_prefix_completion_rows(
    sqlite3 *db, std::unordered_map<std::string, sqlite3_stmt *> &statement_cache, const std::string &prefix_key,
    const std::string &prefix_word, int per_table_limit = 64);

// 给每一行打分、判断能不能显示，并按顺序排好（能显示的在前）。prefix_chars 是前缀的字数；prefix_is_word 表示
// 前缀本身是词库里的词（不是整句猜出来的半个词）。规则见 prefix_completion.cpp。
void score_prefix_completions(std::vector<PrefixCompletionRow> &rows, std::size_t prefix_chars, bool prefix_is_word,
                              const PrefixCompletionScorers &scorers);

// 按前缀缓存查询结果：同一个前缀在连续按键里反复出现（打下一个音节的半截时首选不变）。词库被写
// （调频、造词）时随词典的 reset_cache 一起清。
class PrefixCompletionCache
{
  public:
    const std::vector<PrefixCompletionRow> *find(const std::string &key) const;
    void insert(const std::string &key, std::vector<PrefixCompletionRow> rows);
    void clear()
    {
        entries_.clear();
    }

  private:
    std::unordered_map<std::string, std::vector<PrefixCompletionRow>> entries_;
};

// 首选（result[0]）满足条件时，把排在最前、过了门槛的那条补全插到第二位。full_code 是整串输入在这个方案
// 下的编码（全拼是切分，双拼是按键），首选的 pinyin 去掉分隔符后必须与它相同，即首选覆盖了整串输入。补全行的
// pinyin 沿用首选的 pinyin（选中后吃掉同样多的输入），canonical_pinyin 是它自己的读音。
void insert_prefix_completion(std::vector<WordItem> &result, const std::string &full_code,
                              const PrefixCompletionOptions &options, sqlite3 *db,
                              std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                              PrefixCompletionCache &cache, const PrefixCompletionScorers &scorers);

// 首选能不能拿来当前缀：返回它的读音（不能时返回空串）。insert_prefix_completion 与评测工具共用。
std::string prefix_completion_head_key(const std::vector<WordItem> &result, const std::string &full_code,
                                       const PrefixCompletionOptions &options);
} // namespace quanpin
