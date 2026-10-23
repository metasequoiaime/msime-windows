#pragma once

#include "../core/word_item.h"
#include "engine/ngram/language_model.h"
#include "quanpin_utils.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace quanpin
{

// Phrase-graph + Viterbi beam search over dictionary spans.
// The graph (which words can cover which syllable spans) comes from the
// dictionary; the path score comes from the kenlm trigram in
// engine/ngram. Each hypothesis carries the language model state, so an edge is
// scored as log10 P(word | previous two words) exactly like libime's decoder.
// Dictionary weight only survives as a bounded tiebreak: it decides the order of
// words the language model cannot distinguish (both unseen), and cannot
// outweigh a real n-gram difference.
//
// Without a model (sc.lm missing or unreadable) the decoder falls back to the
// previous uncalibrated unigram heuristic so the IME still produces sentences.
//
// Ranking when merging into an existing candidate list:
//   1. SQLite rows whose key equals the typed syllables exactly
//      (CandidateSource::Database / UserDatabase)
//   2. Lattice full-cover sentences (CandidateSource::Generated)
//   3. Google-pinyin Fallback, prefix-range rows, and everything else
// Lattice never displaces the leading run of exact-key hits (e.g. 高碳钢 for
// gktjgh). Fallback must not block lattice (e.g.
// 高碳钢镊子 ahead of 高谈刚捏子). Callers must not reorder these two
// afterwards: the dictionary layer used to hoist Fallback back to the front,
// which silently inverted the ranking.
// test_quanpin_lattice_precedes_google_fallback guards the end result.
//
// Both sentence sources share generated_sentence_insert_position, so the
// dictionary layer inserts the Google fallback at the same spot instead of at
// the head of the list. A whole-sentence guess must never outrank a phrase the
// dictionary actually has under the typed key.
//
// merge_lattice_candidates only runs at 2+ complete syllables. Single-syllable
// keys are fully covered by exact SQLite lookup. Abbreviated quanpin segments
// (g'k't) are rejected so WordItem.canonical_pinyin stays a complete
// pronunciation.
//
// Lattice WordItem.weight is log_prob * 1000 and is often negative. List
// order is the ranking; do not sort these rows by weight.
//
// merge_lattice_candidates inserts at most four rows, not the whole n-best: each
// reranker's pick, the lattice's own pick (Generated, or Collocation when octagram is
// on), and when both point at the same sentence only one of them. The rest of the
// n-best exists so the reranker has something to choose from -- several near-identical
// sentences are a decoder detail, not a candidate list.

struct LatticeLexeme
{
    std::string key;
    std::string value;
    std::int64_t weight = 0;
    // 从边分里扣掉的分（与路径分同单位）。模糊读音的行（key 与跨度不同）靠它让位于敲出的读音。
    double penalty = 0.0;
};

// 句中辅助码：第 syllable 个音节（0 起，按 decode 收到的 syllables 计）只能解成 accept
// 认可的汉字（UTF-8 单字）。约束在建图时筛边，Viterbi、n-best 和神经重排看到的全是
// 满足约束的路径；一条都走不通就没有整句，不会退回无约束的结果。
struct LatticeCharConstraint
{
    size_t syllable = 0;
    std::function<bool(const std::string &hanzi)> accept;
};

using WordLatticeLookup = std::function<std::vector<LatticeLexeme>(const Segments &span)>;

struct LatticePath
{
    std::string sentence;
    std::string key;
    double log_prob = 0;
    std::vector<std::string> words;
};

// 字级搭配打分函数。context_tail 是路径已累计词文本的尾部（解码器截到
// kCollocationTailCodepoints 个码点），word 是候选词，is_rear 标记该词是句子的
// 最后一个词。见 WordLatticeOptions::collocation_scorer。
using LatticeCollocationScorer =
    std::function<double(std::string_view context_tail, std::string_view word, bool is_rear)>;

// octagram 的 query() 返回自然对数域的搭配强度（.gram 值 = int(ln(频次) * 10000)），
// 而词格主分是 log10（engine/ngram/language_model.h）。装配 lambda 用它把 query() 结果
// 换到 log10 域，让 collocation_weight 退化为纯标定系数（1.0 ≈ librime 的同相对量级），
// 而不是替一个域错位的量附带做格式转换。数值 = 1/ln(10)，写成字面量是因为
// std::log 在 C++17 不是 constexpr。
constexpr double kLnToLog10 = 0.4342944819032518;

struct WordLatticeOptions
{
    int beam = 32;
    int nbest = 5;
    // Whether the lattice's own best path should become a visible candidate. Neural-only mode sets
    // this to false: the lattice still supplies n-best paths to the reranker, but only its pick is shown.
    bool include_lattice_best = true;
    // octagram 激活时把解码最优路径（paths.front()）作为一条 CandidateSource::Collocation 行
    // 展示。单一注入后 octagram 只影响解码，这一行就是它的呈现（〔万象〕/〔八股〕/〔墨奇〕）。
    // 与 include_lattice_best 同时开且指向同一句时，去重只留一条——Collocation 先取，标签
    // 因此归属 octagram（它对这句有贡献，标得更诚实）。
    bool include_collocation_best = false;
    // Each visible source contributes at most one row. When its first choice duplicates an earlier
    // candidate, continue down that source's own ranking until a distinct sentence is found.
    bool show_next_on_duplicate = false;
    // Cap on lexemes per span (injected lookups). The DB lookup applies the
    // same cap itself, inside query_exact_segmentations_keyed_flat.
    int span_limit = 32;
    int max_phrase_syllables = 7;
    // The trigram that scores paths. Null (or an unloadable model) selects the
    // heuristic below. Borrowed, not owned; must outlive the call.
    const ngram::LanguageModel *language_model = nullptr;
    // Weight of the dictionary tiebreak, in log10 units per decade of
    // msime.db weight. Deliberately small: the largest plausible weight spread
    // is ~7 decades, so the tiebreak tops out around 0.07 and can only reorder
    // words the model scores identically (in practice, two unknown words that
    // both took the -7.78 penalty).
    double dictionary_tiebreak = 0.01;
    // 生僻读音惩罚的权重（log10 单位）。语言模型只看汉字，分不出「卷」读 gun、
    // 「而」读 neng 这种词库里权重为 0 的多音字读音，于是 gun'qi 出「卷七」、
    // neng'fa'sheng 出「而发生」。每一行按「自己的权重 / 该拼音跨度的权重和」
    // 算读音占比，占比低于 reading_prior_share 的部分按 log10 扣分。0 表示关闭。
    double reading_prior = 1.0;
    // 免罚线：占比高于它的读音一分不扣。不能整体按 log10 P(词|拼音) 扣，那等于
    // 把字频算两遍（语言模型已经算过一次），「跑得很快」会被「跑的很快」压掉——
    // 得/地 各占 de 的 7%~9%，本来就是常见读音。真正的生僻读音占比是 1e-5 量级，
    // 离这条线还差好几个数量级。
    double reading_prior_share = 0.01;
    // 惩罚下限（正数，实际按 -reading_prior_floor 截断）。权重 0 的行不至于被判
    // 无穷小，语言模型仍有翻盘余地。
    double reading_prior_floor = 5.0;
    // Only used when language_model is unusable. Heuristic unigram normalizer
    // vs phrase-length bonus; single-char msime.db weights are corpus counts
    // while phrase weights are a smaller scale. Never calibrated.
    double unigram_z = 1e6;
    double phrase_length_bonus = 3.0;
    // 见 LatticeCharConstraint。按音节下标升序，同一音节至多一条。
    std::vector<LatticeCharConstraint> char_constraints;
    // 覆盖受约束音节的跨度改用这个查询，筛完约束再按 span_limit 截断。普通查询在 SQL 里
    // 就截掉了 span_limit 之外的行，ji 这种几百个字的音节，辅助码要找的生僻字早被截没了。
    // 为空时退回普通 lookup。
    WordLatticeLookup constrained_lookup;
    // 字级搭配加成：给「前文尾部字符 + 当前词」的字符级搭配打一个加成分，乘以
    // collocation_weight 后叠加到该边的路径分上。为 octagram（.gram）这类与词表
    // 零耦合的语法模型预留——它们只认字符搭配，不认词表 id，我们词库里没有的词
    // 查不到就返回常数惩罚，不伤害只错过。scorer 返回 log10 域的值（装配方用
    // kLnToLog10 把 .gram 的自然对数换算过来），weight 是标定系数，空函数（默认）= 关闭。
    LatticeCollocationScorer collocation_scorer;
    // 搭配项的标定系数（log10 域）；0 = 关闭（scorer 非空也不生效）。
    double collocation_weight = 0.0;
    // 调用方已经按同一套打分解出来的 n-best（双拼直接辅助码的解析器选切分时顺带解出了最优整句）。
    // 非空时 merge_lattice_candidates 直接用它，不再重解一遍，其余合并逻辑不变。全拼从不设置。
    std::vector<LatticePath> precomputed_paths;
};

// Optional second opinion on the decoded n-best, applied before the paths become candidates.
// engine/neural supplies one that reorders them by a character-level Transformer; anything else
// that can rank finished sentences would plug in the same way. Reordering only -- a reranker must
// not add, drop, or edit paths, because their WordItem fields are derived from them afterwards.
//
// Returns whether it actually ranked the paths. False means it declined -- the model abstained, or
// (in the async case) its answer is not back yet -- and the order on return is still the lattice's.
// The caller needs this to attribute the surviving row: a reranker that ran and agreed leaves the
// same order as one that never ran, and those two are not the same thing to report.
using LatticeReranker = std::function<bool(std::vector<LatticePath> &paths)>;

struct SourcedLatticeReranker
{
    LatticeReranker rerank;
    CandidateSource source = CandidateSource::Generated;
};

// Index where a whole-sentence candidate belongs: just past the leading run of
// Database/UserDatabase hits whose key equals the typed syllables exactly.
// Prefix-range and fuzzy rows are not exact hits even when they happen to have
// the same syllable count (gun'qi scans up 滚球 = gun'qiu), so they rank below
// the sentence. Shared by the lattice merge and by the dictionary layer's
// Google fallback so both land in the same place.
size_t generated_sentence_insert_position(const std::vector<WordItem> &candidates, const Segments &syllables);

std::vector<LatticePath> decode_word_lattice(const Segments &syllables, const WordLatticeLookup &lookup,
                                             const WordLatticeOptions &options = {});

// 容错槽位词格的一条边：覆盖槽区间 [start, end)，读音与词条由调用方查好——key 是覆盖
// 区间的完整合法读音（可展开槽已代入具体音节），penalty 是该读音相对字面槽的手误代价
// （log10，单位与该区间相同：纠错表目标按 kAutocorrectWeightToLog 折算，句中简拼展开
// 按常数 kJianpinExpandPenaltyLog 折算——它是意图不是手误，折算系数刻意更小）。
struct FaultLatticeEdge
{
    size_t start = 0;
    size_t end = 0;
    std::string word;
    std::string key;
    std::int64_t weight = 0;
    double penalty = 0.0;
};

// 容错槽位词格解码：edges_by_start[pos] 是从槽 pos 出发的全部边。与 decode_word_lattice
// 同一套 Viterbi（kenlm 打分、beam、n-best 去重），但边由调用方按容错槽位预先查出，
// 手误代价已折进边分。刻意不装配搭配 scorer（octagram 禁令：这条解码只用于切分/纠错
// 仲裁，裁决只看 kenlm）；无语言模型时退回普通词格的启发式。
std::vector<LatticePath> decode_fault_lattice(size_t slot_count,
                                              const std::vector<std::vector<FaultLatticeEdge>> &edges_by_start,
                                              const WordLatticeOptions &options = {});

// `rerank_source` is the source stamped on the reranker's pick, so the candidate list can say which
// component chose that row. The lattice's own pick is stamped CandidateSource::Generated when
// options.include_lattice_best is true, or CandidateSource::Collocation when
// options.include_collocation_best is true (octagram is on; it owns the row when both are set).
// When neither is set, no lattice row is inserted until a reranker has produced a result.
void merge_lattice_candidates(std::vector<WordItem> &candidates, const Segments &syllables,
                              const WordLatticeLookup &lookup, const std::string &typed_pinyin,
                              const WordLatticeOptions &options = {}, const LatticeReranker &rerank = {},
                              CandidateSource rerank_source = CandidateSource::Generated);

// Runs every enabled reranker independently over the same original n-best. Each completed reranker
// may contribute one visible row. Existing candidates (including Unigram) and Trigram reserve their
// rows first. If both neural models have the same best remaining row, NeuralKeyboard owns it and is
// displayed first; otherwise NeuralDesktop keeps its best remaining row and is displayed first.
// A Unigram or Trigram best shared by another enabled source is promoted ahead of the neural rows.
void merge_lattice_candidates(std::vector<WordItem> &candidates, const Segments &syllables,
                              const WordLatticeLookup &lookup, const std::string &typed_pinyin,
                              const WordLatticeOptions &options, const std::vector<SourcedLatticeReranker> &rerankers);

} // namespace quanpin
