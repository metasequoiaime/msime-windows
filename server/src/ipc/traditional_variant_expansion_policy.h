#pragma once

#include "conversion/traditional_spelling_index.h"
#include "engine/core/word_item.h"
#include "ipc/candidate_text_policy.h"
#include "ipc/traditional_region_policy.h"
#include "ipc/traditional_variant_order_policy.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace FanyImeIpc
{
/** 繁体模式下一个候选在列表里怎样体现它的繁体写法。 */
enum class VariantExpansion
{
    // 每种写法一项，紧挨着放在原位置。
    AllSpellings,
    // 只出一项，写法取排序后的第一种。
    FirstSpelling,
    // 原样保留，不填写法。
    Unchanged,
};

inline VariantExpansion VariantExpansionFor(const WordItem &item)
{
    if (item.scheme == SchemeType::JapaneseRomaji)
        return VariantExpansion::Unchanged;
    switch (item.source)
    {
    case CandidateSource::Database:
    case CandidateSource::UserDatabase:
        return VariantExpansion::AllSpellings;
    case CandidateSource::Generated:
    case CandidateSource::Fallback:
    case CandidateSource::NeuralDesktop:
    case CandidateSource::NeuralKeyboard:
    case CandidateSource::Collocation:
    case CandidateSource::CloudSuggestion:
    case CandidateSource::AiSuggestion:
        return VariantExpansion::FirstSpelling;
    default:
        return VariantExpansion::Unchanged;
    }
}

namespace traditional_variant_detail
{
// 每种写法归哪个底词，依次判：
// 1. 单字写法被两个以上简体写法的单字底词展开、它的首选底词也在其中时，归首选底词；
// 2. 有底词就等于这种写法、且这个底词是简体写法时，归它；
// 3. 归第一个列出这种写法的底词。
inline std::unordered_map<std::string, std::string> SpellingOwners(
    const std::vector<WordItem> &items, const std::vector<std::vector<std::string>> &listed,
    const ChineseConverter::TraditionalSpellingIndex &index)
{
    std::unordered_map<std::string, std::string> owners;
    // 每种单字写法由哪些简体写法的单字底词展开，同一底词只记一次。
    std::unordered_map<std::string, std::vector<std::string>> simplified_character_listers;
    for (size_t position = 0; position < items.size(); ++position)
    {
        const std::string &word = items[position].word;
        const bool simplified = index.IsSimplifiedSpelling(word);
        const bool simplified_character = simplified && ChineseConverter::IsSingleUtf8CodePoint(word);
        for (const auto &spelling : listed[position])
        {
            const auto owner = owners.emplace(spelling, word);
            if (!owner.second && word == spelling && simplified)
                owner.first->second = word;
            if (!simplified_character || !ChineseConverter::IsSingleUtf8CodePoint(spelling))
                continue;
            auto &listers = simplified_character_listers[spelling];
            if (std::find(listers.begin(), listers.end(), word) == listers.end())
                listers.push_back(word);
        }
    }
    for (const auto &[spelling, listers] : simplified_character_listers)
    {
        if (listers.size() < 2)
            continue;
        const std::string preferred = index.PreferredWordOf(spelling);
        if (std::find(listers.begin(), listers.end(), preferred) != listers.end())
            owners[spelling] = preferred;
    }
    return owners;
}
} // namespace traditional_variant_detail

/**
 * 一个简体词按地区排好序的全部繁体写法：index 的三种转换结果，单字再接 index 给的字表写法；多字词不查字表。
 */
inline std::vector<std::string> TraditionalSpellingsOf(const std::string &simplified,
                                                       const ChineseConverter::TraditionalSpellingIndex &index,
                                                       TraditionalRegion region)
{
    const auto forms = index.FormsOf(simplified);
    const std::vector<std::string> character_table = ChineseConverter::IsSingleUtf8CodePoint(simplified)
                                                         ? index.CharacterTableSpellingsOf(simplified)
                                                         : std::vector<std::string>{};
    return OrderTraditionalVariants(forms.standard, forms.taiwan, forms.hong_kong, character_table, region);
}

/**
 * 把候选列表展开成繁体写法：按 VariantExpansionFor 决定每个候选出几项，每项复制原候选的全部字段，
 * output_text 填写法，variant_rank 是它在这个底词留下的写法里的序号（0 起）。写法取
 * TraditionalSpellingsOf(word, index, region)；没有写法的候选原样保留。
 * 填了写法的项之间，两个不同底词（word）出同一种写法时只留一项，依次按：
 * 单字写法被两个以上简体写法（index.IsSimplifiedSpelling 为真）的单字底词展开、且它的首选底词
 * （index.PreferredWordOf）也在其中时，留首选底词；写法就等于自己底词、且这个底词是简体写法时，留它；
 * 否则留前面的。另一项去掉这种写法，写法全被去掉的候选不出现。同一底词以不同拼音或来源出现多次时
 * 各自保留。原样保留的项不参与比较，也不会被删。
 */
inline std::vector<WordItem> ExpandTraditionalVariants(std::vector<WordItem> items,
                                                       const ChineseConverter::TraditionalSpellingIndex &index,
                                                       TraditionalRegion region)
{
    // 每个候选要列出的写法；原样保留的候选为空。
    std::vector<std::vector<std::string>> listed(items.size());
    for (size_t position = 0; position < items.size(); ++position)
    {
        const VariantExpansion expansion = VariantExpansionFor(items[position]);
        if (expansion == VariantExpansion::Unchanged)
            continue;
        auto &spellings = listed[position];
        spellings = TraditionalSpellingsOf(items[position].word, index, region);
        spellings.erase(std::remove(spellings.begin(), spellings.end(), std::string()), spellings.end());
        if (expansion == VariantExpansion::FirstSpelling && spellings.size() > 1)
            spellings.resize(1);
    }
    const auto owners = traditional_variant_detail::SpellingOwners(items, listed, index);

    std::vector<WordItem> expanded;
    expanded.reserve(items.size());
    for (size_t position = 0; position < items.size(); ++position)
    {
        auto &item = items[position];
        if (listed[position].empty())
        {
            expanded.push_back(std::move(item));
            continue;
        }
        int variant_rank = 0;
        for (auto &spelling : listed[position])
        {
            if (owners.at(spelling) != item.word)
                continue;
            WordItem variant = item;
            variant.output_text = std::move(spelling);
            variant.variant_rank = variant_rank++;
            expanded.push_back(std::move(variant));
        }
    }
    return expanded;
}

/** 每个底词只留它在列表里留下的第一项（variant_rank 为 0 的项），其余项与列表顺序不变。 */
inline std::vector<WordItem> FirstVariantsOnly(const std::vector<WordItem> &items)
{
    std::vector<WordItem> first;
    first.reserve(items.size());
    std::copy_if(items.begin(), items.end(), std::back_inserter(first),
                 [](const WordItem &item) { return item.variant_rank == 0; });
    return first;
}

/**
 * 把展开过的列表收拢成每组写法一项：去掉 variant_rank 大于 0 的项，留下的项清空 output_text。
 * 没有展开过的列表原样返回。展开时整项删掉的候选不在展开后的列表里，收拢也找不回来。
 */
inline std::vector<WordItem> CollapseTraditionalVariants(std::vector<WordItem> items)
{
    items.erase(std::remove_if(items.begin(), items.end(), [](const WordItem &item) { return item.variant_rank > 0; }),
                items.end());
    for (auto &item : items)
        item.output_text.clear();
    return items;
}

/**
 * 在每组写法只占一项的列表上做按 word 删插的重排，再展开回写法。apply_fixed_positions、
 * place_quick_phrases 这类操作按 word 删掉同字候选、只插回一项，直接作用在展开过的列表上会丢写法，
 * 插回的项还可能落进别的组中间。items 可以已经展开过：先收拢成每组一项，交给
 * reorder(std::vector<WordItem> &) 原地改，再交给 expand(std::vector<WordItem>)，返回它的结果。
 */
template <typename Reorder, typename Expand>
std::vector<WordItem> ReorderAsOneItemPerWord(std::vector<WordItem> items, Reorder &&reorder, Expand &&expand)
{
    auto collapsed = CollapseTraditionalVariants(std::move(items));
    reorder(collapsed);
    return expand(std::move(collapsed));
}

/**
 * index 处的候选所在写法组的第一写法的下标；不在组里的候选返回 index 本身，越界的 index 原样返回。
 * 展开把同一底词的写法紧挨着放，第一写法在最前。
 */
inline size_t SpellingGroupStart(const std::vector<WordItem> &items, size_t index)
{
    if (index >= items.size())
        return index;
    while (index > 0 && items[index].variant_rank > 0)
        --index;
    return index;
}

/** 候选列表按每页 page_size 项分页后的一个位置：第几页（0 起）、页内第几项（0 起）。 */
struct CandidatePagePosition
{
    int page_index = 0;
    int index_in_page = 0;
};

/**
 * 列表里第一个底词为 word 的写法组组首（variant_rank 为 0 的项）在每页 page_size 项时的位置。
 * 没有这样的项、或 page_size 不大于 0 时返回空。
 */
inline std::optional<CandidatePagePosition> PagePositionOfWord(const std::vector<WordItem> &items,
                                                               const std::string &word, int page_size)
{
    if (page_size <= 0)
        return std::nullopt;
    const auto head = std::find_if(items.begin(), items.end(),
                                   [&](const WordItem &item) { return item.variant_rank == 0 && item.word == word; });
    if (head == items.end())
        return std::nullopt;
    const int index = static_cast<int>(std::distance(items.begin(), head));
    return CandidatePagePosition{index / page_size, index % page_size};
}

/** 组字途中切换字形后的候选列表，与高亮在其中的位置。 */
struct RespelledCandidateList
{
    std::vector<WordItem> items;
    CandidatePagePosition highlight;
};

/**
 * 组字途中切换字形后的候选列表：items 先收拢成每组写法一项（CollapseTraditionalVariants），再交给
 * expand(std::vector<WordItem>) 按新字形展开，新列表是它的结果。候选的先后顺序不变；写法全归列表里别的底词的候选
 * （「余」「馀」同在列表时的「馀」）切到繁体时不出现，切回简体时也找不回来，要到下一次组页。
 * 高亮落在 highlighted_index 处候选的底词在新列表里的写法组组首，按每页 page_size 项分页
 * （PagePositionOfWord）；highlighted_index 越界、新列表里没有这个底词、或 page_size 不大于 0 时落在第 0 页第 0 项。
 */
template <typename Expand>
RespelledCandidateList RespellCandidateList(std::vector<WordItem> items, size_t highlighted_index, int page_size,
                                            Expand &&expand)
{
    const std::optional<std::string> highlighted_word =
        highlighted_index < items.size() ? std::optional<std::string>(items[highlighted_index].word) : std::nullopt;
    RespelledCandidateList respelled{expand(CollapseTraditionalVariants(std::move(items))), {}};
    if (highlighted_word)
        respelled.highlight =
            PagePositionOfWord(respelled.items, *highlighted_word, page_size).value_or(CandidatePagePosition{});
    return respelled;
}

/**
 * 快捷短语组之后的普通候选被选中时，「越过整组」要学的名次：selected_index 所在写法组之前有几个
 * counts_toward_slot 为真的候选。快捷短语组从 group_index 起；选中项所在组在快捷短语组之前、
 * 或这一组的第一写法不计数、或 selected_index 越界时返回空。同一底词的写法每组只算一个。
 */
template <typename CountsTowardSlot>
std::optional<size_t> QuickPhraseBypassRank(const std::vector<WordItem> &items, size_t selected_index,
                                            size_t group_index, CountsTowardSlot &&counts_toward_slot)
{
    if (selected_index >= items.size())
        return std::nullopt;
    const size_t selected_group = SpellingGroupStart(items, selected_index);
    if (selected_group < group_index || !counts_toward_slot(items[selected_group]))
        return std::nullopt;
    return static_cast<size_t>(std::count_if(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(selected_group),
                                             [&](const WordItem &item) { return counts_toward_slot(item); }));
}

/**
 * 候选显示与上屏的文字。convert 为假（不做简转繁）时是 word；为真时取展开填的写法，
 * 没有展开的候选由 to_output(word) 转换。
 */
template <typename ToOutput>
std::string CandidateOutputTextFor(const WordItem &item, bool convert, ToOutput &&to_output)
{
    if (!convert)
        return item.word;
    if (!item.output_text.empty())
        return item.output_text;
    return to_output(item.word);
}

/**
 * 一条异步候选（云、AI）插入前要查的：列表 items 里已有同一底词（word），或已有填了写法的项显示
 * candidate 的写法（output_text）时为真，这时不插入。candidate 要先按当前字形填好写法；没填写法
 * （简体模式、原样保留的来源）时只比底词，列表里没填写法的项也只比底词。
 */
inline bool ListsWordOrSpellingOf(const std::vector<WordItem> &items, const WordItem &candidate)
{
    return std::any_of(items.begin(), items.end(), [&](const WordItem &item) {
        return item.word == candidate.word ||
               (!candidate.output_text.empty() && item.output_text == candidate.output_text);
    });
}

/** 给候选填繁体写法要看的设置与数据。 */
struct CandidateSpellingSettings
{
    const ChineseConverter::TraditionalSpellingIndex &index;
    TraditionalRegion region;
    // 字形设置是繁体。
    bool traditional_character_set;
    // 当前输入方案。
    SchemeType scheme;
    // 当前候选列表是 U 模式（部件拆字）的结果。
    bool radical_candidate_list;
};

/**
 * 给一条云或 AI 候选填上排序后的第一写法，插进列表的项与插入前判定用的探针都由它填。
 * 按 ShouldConvertCandidateTextToTraditional 不做简转繁时（简体字形、日语方案、U 模式拆字列表）原样返回；
 * 没有写法时也原样返回。
 */
inline WordItem WithFirstTraditionalSpellingOf(WordItem item, const CandidateSpellingSettings &settings)
{
    if (!ShouldConvertCandidateTextToTraditional(settings.traditional_character_set,
                                                 settings.scheme == SchemeType::JapaneseRomaji,
                                                 settings.radical_candidate_list))
        return item;
    auto spelled = ExpandTraditionalVariants({item}, settings.index, settings.region);
    return spelled.empty() ? item : std::move(spelled.front());
}

/**
 * 一条云或 AI 候选（底词 word、拼音 pinyin、来源 source）该不该插进当前列表 items：按 settings 填好它插入后的
 * 第一写法（方案取 settings.scheme），列表里已有同一底词、或已有项显示同一种写法时为假。
 */
inline bool ShouldInsertAsyncCandidate(const std::vector<WordItem> &items, const std::string &pinyin,
                                       const std::string &word, CandidateSource source,
                                       const CandidateSpellingSettings &settings)
{
    WordItem candidate(pinyin, word, 0, source);
    candidate.scheme = settings.scheme;
    return !ListsWordOrSpellingOf(items, WithFirstTraditionalSpellingOf(std::move(candidate), settings));
}
} // namespace FanyImeIpc
