#pragma once

#include "engine/core/word_item.h"
#include "ipc/candidate_text_policy.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace FanyImeIpc
{
/** （全拼键 key, 简体底词 word）记住的写法，没有记录时返回空。 */
using RememberedSpellingLookup =
    std::function<std::optional<std::string>(const std::string &key, const std::string &word)>;

/** 恒返回空的查询函数，给没有写法记录可查的展开用。 */
inline RememberedSpellingLookup NoRememberedSpellings()
{
    return [](const std::string &, const std::string &) -> std::optional<std::string> { return std::nullopt; };
}

/** 读当前时刻的函数，单位毫秒，读数不回退。 */
using MillisecondClock = std::function<std::int64_t()>;

/**
 * 给 lookup 加累计用时上限：返回新的查询函数，从它第一次被调用开始用 now 计时，累计用时达到 budget_ms 之后
 * 不再调用 lookup，一律返回空。已经开始的那次查询不打断，耗时超过上限也返回它自己的结果。lookup 不持有函数时
 * 一律返回空。一次展开用一个。
 */
inline RememberedSpellingLookup WithLookupBudget(RememberedSpellingLookup lookup, std::int64_t budget_ms,
                                                 MillisecondClock now)
{
    return [lookup = std::move(lookup), budget_ms, now = std::move(now), started_at = std::optional<std::int64_t>()](
               const std::string &key, const std::string &word) mutable -> std::optional<std::string> {
        if (!lookup)
            return std::nullopt;
        const std::int64_t current = now();
        if (!started_at)
            started_at = current;
        if (current - *started_at >= budget_ms)
            return std::nullopt;
        return lookup(key, word);
    };
}

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

/** 改动的字段齐不齐：key、word 都不为空，Remember 时 spelling 也不为空。不齐的改动不写库。 */
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
 * 选中、置顶或固定 item 有没有可能改写法记录，只看 item 自己的字段，不查转换器：
 * 不是日语候选、有底词、有显示的写法，且不是这组当前排第一的写法（variant_rank > 0）。
 * 为假时 VariantChoiceForSelection 一定返回空，调用方据此先跳过地区默认写法的计算（要过三遍 OpenCC）。
 */
inline bool SelectionCanChangeVariantChoice(const WordItem &item)
{
    return item.scheme != SchemeType::JapaneseRomaji && item.variant_rank > 0 && !item.output_text.empty() &&
           !item.word.empty();
}

/**
 * 选中、置顶或固定展开后的一项（item）之后要对写法记录做的改动，不用改时返回空。
 * key 是这一项的全拼键，推不出来时传空；default_first_spelling 是这个底词按地区排序（不计记录）的第一写法。
 * 只有这一项不是这组当前排第一的写法（variant_rank > 0）时才改：它就是地区默认第一写法时删掉记录，
 * 否则记下它的写法。
 * 所以选回默认只在记录仍然生效（把别的写法顶到了第一位）时才删得掉。记录已经失效时（写法不再属于这个词、
 * 也拼不出来，或在列表里归给了别的底词），组首本来就是地区默认写法，选它不删记录；这条记录不改变显示。
 */
inline std::optional<VariantChoiceChange> VariantChoiceForSelection(const VariantChoiceScope &scope,
                                                                    const WordItem &item, const std::string &key,
                                                                    const std::string &default_first_spelling)
{
    if (!RemembersVariantChoices(scope) || !SelectionCanChangeVariantChoice(item) || key.empty())
        return std::nullopt;
    if (item.output_text == default_first_spelling)
        return VariantChoiceChange{VariantChoiceAction::Forget, key, item.word, {}};
    return VariantChoiceChange{VariantChoiceAction::Remember, key, item.word, item.output_text};
}

/** 一次造词里已选各段的情况。 */
struct ComposedWordSegments
{
    // 各段选中时的字形都相同。
    bool same_character_set;
    // 每一段显示的都是它底词的第一写法。
    bool all_first_spellings;
};

/**
 * 算上当前这一段，一次造词各段的字形是否仍相同。same_before_segment 是到上一段为止的结果，
 * first_segment_character_set 是第一段的字形（当前段就是第一段时也已填好），segment_character_set 是当前段的字形。
 */
inline bool SameCharacterSetAfterSegment(bool same_before_segment, const std::string &first_segment_character_set,
                                         const std::string &segment_character_set)
{
    return same_before_segment && first_segment_character_set == segment_character_set;
}

/**
 * 造好一个词上屏之后要对写法记录做的改动，不用改时返回空。key 是整词的全拼键，推不出来（词不能落库）时传空；
 * default_first_spelling 是整词按地区排序（不计记录）的第一写法。各段字形不全相同时上屏串是混排，不改记录。
 * 字形相同时，上屏的 committed_text 等于 default_first_spelling 就删掉（key, word）的记录；
 * 否则有一段没选第一写法时记下 committed_text。
 */
inline std::optional<VariantChoiceChange> VariantChoiceForComposedWord(const VariantChoiceScope &scope,
                                                                       const ComposedWordSegments &segments,
                                                                       const std::string &key, const std::string &word,
                                                                       const std::string &committed_text,
                                                                       const std::string &default_first_spelling)
{
    if (!RemembersVariantChoices(scope) || key.empty() || word.empty() || committed_text.empty())
        return std::nullopt;
    if (!segments.same_character_set)
        return std::nullopt;
    if (committed_text == default_first_spelling)
        return VariantChoiceChange{VariantChoiceAction::Forget, key, word, {}};
    if (segments.all_first_spellings)
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
