#pragma once
#include "../core/fuzzy_pinyin_options.h"
#include "../core/word_item.h"
#include "quanpin_utils.h"
#include <algorithm>
#include <array>
#include <unordered_set>

namespace quanpin
{
// 模糊读音的折扣。开模糊音等于说「这个音我可能没分清」，模糊读音应当与敲出的读音按词频混排，
// 而不是整组垫在后面：min 下的「明」（286 万）该排在「敏」（20 万）前，更该排在 min 那几十个
// 权重为 0 的生僻字前。折扣保证敲出读音的常用字（「民」90 万）仍压得住模糊读音。
constexpr double kFuzzyWeightRatio = 0.2;
// 词格里路径每走一个模糊音节扣的分（log10）。拿 eval_quanpin_sentence 的合法输入集加模糊音
// 例句标定：只开 in/ing 时 0.7~1.0 都不改变合法输入的首选，lin'yi'bian 照样解出「另一边」；
// 规则全开时 wo'xiang 要 1.5 才不被 ian/iang 读成「我先」，但那时 an/ang 的「晚宴上买」又会
// 压过「蜿蜒山脉的另一边」。取 1.0（模糊读音的句概率要高出 10 倍才翻盘），按常见的只开几条
// 规则调。
constexpr double kFuzzyLatticePenaltyLog = 1.0;
// 词格每个跨度最多再查几条模糊变体。跨度最长 7 个音节，规则全开时变体按乘积增长。
constexpr std::size_t kFuzzyLatticeVariantLimit = 16;

inline std::vector<std::string> fuzzy_syllables(const std::string &syllable, metasequoia::FuzzyPinyinOptions options)
{
    using R = metasequoia::FuzzyPinyinRule;
    struct Pair
    {
        const char *a;
        const char *b;
        R rule;
    };
    static const std::array<Pair, 6> initials = {{{"z", "zh", R::Z_ZH},
                                                  {"c", "ch", R::C_CH},
                                                  {"s", "sh", R::S_SH},
                                                  {"n", "l", R::N_L},
                                                  {"f", "h", R::F_H},
                                                  {"r", "l", R::R_L}}};
    static const std::array<Pair, 5> finals = {{{"an", "ang", R::AN_ANG},
                                                {"en", "eng", R::EN_ENG},
                                                {"in", "ing", R::IN_ING},
                                                {"ian", "iang", R::IAN_IANG},
                                                {"uan", "uang", R::UAN_UANG}}};
    static const auto legal = [] {
        const auto &list = intact_pinyin_list();
        return std::unordered_set<std::string>(list.begin(), list.end());
    }();
    // Only complete syllables expand: incomplete prefixes retain normal completion behavior.
    if (!options.rules || !legal.count(syllable))
        return {syllable};
    std::string initial;
    if (syllable.rfind("zh", 0) == 0 || syllable.rfind("ch", 0) == 0 || syllable.rfind("sh", 0) == 0)
        initial = syllable.substr(0, 2);
    else if (std::string("bpmfdtnlgkhjqxrzcsyw").find(syllable[0]) != std::string::npos)
        initial = syllable.substr(0, 1);
    const std::string final = syllable.substr(initial.size());
    std::vector<std::string> starts{initial}, ends{final}, result{syllable};
    for (const auto &pair : initials)
        if (options.enabled(pair.rule))
        {
            if (initial == pair.a)
                starts.push_back(pair.b);
            else if (initial == pair.b)
                starts.push_back(pair.a);
        }
    for (const auto &pair : finals)
        if (options.enabled(pair.rule))
        {
            if (final == pair.a)
                ends.push_back(pair.b);
            else if (final == pair.b)
                ends.push_back(pair.a);
        }
    for (const auto &start : starts)
        for (const auto &end : ends)
        {
            const auto candidate = start + end;
            if (legal.count(candidate) && std::find(result.begin(), result.end(), candidate) == result.end())
                result.push_back(candidate);
        }
    return result;
}

// A bounded beam prevents exponential queries for phrases with many enabled rules.
inline std::vector<Segments> fuzzy_segmentations(const Segments &segments, metasequoia::FuzzyPinyinOptions options,
                                                 std::size_t limit = 64)
{
    if (!options.rules || segments.empty() || limit == 0)
        return {};
    std::vector<Segments> paths{{}};
    for (const auto &syllable : segments)
    {
        const auto alternatives = fuzzy_syllables(syllable, options);
        std::vector<Segments> next;
        for (const auto &path : paths)
            for (const auto &alternative : alternatives)
            {
                if (next.size() == limit)
                    break;
                auto copy = path;
                copy.push_back(alternative);
                next.push_back(std::move(copy));
            }
        paths = std::move(next);
    }
    paths.erase(std::remove(paths.begin(), paths.end(), segments), paths.end());
    return paths;
}

// 把模糊候选归并进精确候选。group 是候选覆盖的输入长度，大的在前。精确列表的次序原样保留：
// 整句、保护位、调频与纠错裁决的结果都留在原处（不能按 group 重排它——纠错读法的字母比敲的少，
// 重排会把 jiuzheeyang 裁决出的「就这样」压到字面读法下面）。模糊行按 group、权重排好后
// 逐个插到第一个「比它短」或「同长而权重低于它折后权重」的精确词库行前面。
template <typename Group>
std::vector<WordItem> merge_fuzzy_candidates(std::vector<WordItem> exact, std::vector<WordItem> fuzzy, Group group)
{
    // 词格和 Google 整句会猜出模糊读音的整词：qingdian 打开 in/ing 时，词格可能把
    // 「亲电」当整句先生成出来，而词库里 qin'dian 本来就有这一行。原来按词去重保留先到的
    // 那一项，留下的是猜出来的整句（权重是路径分，不是词频），真正带频率的词库行被删掉。
    // 选它时 adjust_candidate_ranking 只认 Database/UserDatabase，一行权重都写不进去，
    // 用户怎么选都调不动（#1172）。同一条整句的 canonical 是猜出来的读音，和词库行的
    // 模糊读音本来就可能不同（zhixin 的「执行」猜成 zhi'xin，词库行在 zhi'xing），所以
    // 只按词比对：同词时让出猜出来的整句，留下词库行，调频才能落到真实的权重上。
    // 精确读音的词库行不算猜出来的整句，照旧由它去重掉模糊行。
    std::unordered_set<std::string> fuzzy_dictionary_words;
    for (const auto &item : fuzzy)
    {
        if (item.source == CandidateSource::Database || item.source == CandidateSource::UserDatabase)
        {
            fuzzy_dictionary_words.insert(item.word);
        }
    }
    if (!fuzzy_dictionary_words.empty())
    {
        exact.erase(std::remove_if(exact.begin(), exact.end(),
                                   [&](const WordItem &item) {
                                       const bool guessed_sentence = item.sentence_association && !item.fuzzy &&
                                                                     (item.source == CandidateSource::Fallback ||
                                                                      item.source == CandidateSource::Generated ||
                                                                      item.source == CandidateSource::Collocation ||
                                                                      item.source == CandidateSource::NeuralKeyboard ||
                                                                      item.source == CandidateSource::NeuralDesktop);
                                       return guessed_sentence && fuzzy_dictionary_words.count(item.word) != 0;
                                   }),
                    exact.end());
    }
    std::unordered_set<std::string> seen;
    for (const auto &item : exact)
        seen.insert(item.word);
    fuzzy.erase(std::remove_if(fuzzy.begin(), fuzzy.end(),
                               [&](const WordItem &item) { return !seen.insert(item.word).second; }),
                fuzzy.end());
    std::stable_sort(fuzzy.begin(), fuzzy.end(), [&](const WordItem &a, const WordItem &b) {
        const auto ga = group(a), gb = group(b);
        return ga != gb ? ga > gb : a.weight > b.weight;
    });
    const auto ranked_by_weight = [](const WordItem &item) {
        return !item.fuzzy &&
               (item.source == CandidateSource::Database || item.source == CandidateSource::UserDatabase);
    };
    std::vector<WordItem> merged;
    merged.reserve(exact.size() + fuzzy.size());
    std::size_t i = 0, j = 0;
    while (i < exact.size() || j < fuzzy.size())
    {
        bool take_fuzzy = i == exact.size();
        if (!take_fuzzy && j < fuzzy.size() && ranked_by_weight(exact[i]))
        {
            const auto ge = group(exact[i]), gf = group(fuzzy[j]);
            take_fuzzy = ge != gf ? gf > ge
                                  : static_cast<double>(exact[i].weight) <
                                        static_cast<double>(fuzzy[j].weight) * kFuzzyWeightRatio;
        }
        merged.push_back(std::move(take_fuzzy ? fuzzy[j++] : exact[i++]));
    }
    return merged;
}
} // namespace quanpin
