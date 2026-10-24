#pragma once

#include "engine/core/word_item.h"
#include "ipc/candidate_text_policy.h"

#include <optional>
#include <string>

namespace FanyImeIpc
{
/** 选词发生时的候选环境，决定要不要记住繁体写法。 */
struct VariantChoiceScope
{
    bool traditional_character_set = false;
    bool japanese_scheme = false;
    // 当前候选列表是 U 模式拆字结果（见 IsRadicalCandidateList）。
    bool radical_candidate_list = false;
};

/** 只有候选显示的是繁体写法时（繁体字形、非日语方案、非 U 模式）才读写写法记录。 */
inline bool RemembersVariantChoices(const VariantChoiceScope &scope)
{
    return ShouldConvertCandidateTextToTraditional(scope.traditional_character_set, scope.japanese_scheme,
                                                   scope.radical_candidate_list);
}

enum class VariantChoiceAction
{
    // 记下（key, word）→ spelling，覆盖这个键与底词原有的记录。
    Remember,
    // 删掉（key, word）的记录。
    Forget,
};

/** 对写法记录的一次改动。key 是全拼键，word 是简体底词；Forget 时 spelling 为空。 */
struct VariantChoiceChange
{
    VariantChoiceAction action = VariantChoiceAction::Remember;
    std::string key;
    std::string word;
    std::string spelling;
};

/** 改动的字段齐不齐：key、word 都不为空，Remember 时 spelling 也不为空。不齐的改动不进副本、不写库。 */
inline bool IsCompleteVariantChoiceChange(const VariantChoiceChange &change)
{
    if (change.key.empty() || change.word.empty())
        return false;
    return change.action == VariantChoiceAction::Forget || !change.spelling.empty();
}

/**
 * 候选 item 读写繁体写法记录用的全拼键，就是它的 canonical_pinyin。没有 canonical 全拼的候选返回空，
 * 不读也不写记录：它的 pinyin 字段可能是原始双拼码或简拼，不能当全拼键用。
 */
inline std::string VariantChoiceKeyOf(const WordItem &item)
{
    return item.canonical_pinyin;
}

/**
 * 选中、置顶或固定展开后的一项（item）之后要对写法记录做的改动，不用改时返回空。
 * key 是这一项的全拼键，推不出来时传空；default_first_spelling 是这个底词按地区排序（不计记录）的第一写法。
 * 只有这一项不是这组当前排第一的写法（variant_rank > 0）时才改：它就是地区默认第一写法时删掉记录，
 * 否则记下它的写法。
 */
inline std::optional<VariantChoiceChange> VariantChoiceForSelection(const VariantChoiceScope &scope,
                                                                    const WordItem &item, const std::string &key,
                                                                    const std::string &default_first_spelling)
{
    if (!RemembersVariantChoices(scope) || item.scheme == SchemeType::JapaneseRomaji)
        return std::nullopt;
    if (item.variant_rank <= 0 || item.output_text.empty() || key.empty() || item.word.empty())
        return std::nullopt;
    if (item.output_text == default_first_spelling)
        return VariantChoiceChange{VariantChoiceAction::Forget, key, item.word, {}};
    return VariantChoiceChange{VariantChoiceAction::Remember, key, item.word, item.output_text};
}

/**
 * 造好一个词上屏之后要对写法记录做的改动，不用改时返回空。key 是整词的全拼键，推不出来（词不能落库）时传空；
 * default_first_spelling 是整词按地区排序（不计记录）的第一写法。上屏的 committed_text 等于它时删掉
 * （key, word）的记录；否则有一段没选第一写法（all_first_spellings 为假）时记下 committed_text。
 */
inline std::optional<VariantChoiceChange> VariantChoiceForComposedWord(const VariantChoiceScope &scope,
                                                                       bool all_first_spellings, const std::string &key,
                                                                       const std::string &word,
                                                                       const std::string &committed_text,
                                                                       const std::string &default_first_spelling)
{
    if (!RemembersVariantChoices(scope) || key.empty() || word.empty() || committed_text.empty())
        return std::nullopt;
    if (committed_text == default_first_spelling)
        return VariantChoiceChange{VariantChoiceAction::Forget, key, word, {}};
    if (all_first_spellings)
        return std::nullopt;
    return VariantChoiceChange{VariantChoiceAction::Remember, key, word, committed_text};
}

/**
 * 删除候选 item 时要连带删掉写法记录的底词：不论简体、繁体还是 U 模式都删。只有拼音（全拼、双拼）候选
 * 删的是拼音词库里的底词；五笔、日语候选删的是各自码表或词库里的词，同形的拼音底词还在，返回空。
 * 日语方案下、item 没有底词时也返回空。
 */
inline std::optional<std::string> VariantChoiceWordToForgetOnDelete(const VariantChoiceScope &scope,
                                                                    const WordItem &item)
{
    const bool pinyin_candidate = item.scheme == SchemeType::Quanpin || item.scheme == SchemeType::Shuangpin;
    if (scope.japanese_scheme || !pinyin_candidate || item.word.empty())
        return std::nullopt;
    return item.word;
}
} // namespace FanyImeIpc
