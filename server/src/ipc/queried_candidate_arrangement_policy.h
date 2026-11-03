#pragma once

#include "engine/core/word_item.h"
#include "ordinary_slot_placement_policy.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace FanyImeIpc
{
// 单字母输入组页时最多留几个候选。
inline constexpr size_t kSingleLetterCandidateLimit = 24;

/**
 * 组页时查询结果在固定位置与快捷短语放好之后的整理：单字母输入先截到 kSingleLetterCandidateLimit 个候选，
 * 再用 expand_spellings 展开繁体写法（截的是候选个数，不切开写法组），最后把字母词放进普通候选的位置
 * （PlaceQueriedLatinWords，page_size 是每页候选数）。
 */
template <typename SpellingExpansion>
std::vector<WordItem> ArrangeQueriedCandidates(std::vector<WordItem> items, bool single_letter_input,
                                               SpellingExpansion expand_spellings, size_t page_size)
{
    if (single_letter_input && items.size() > kSingleLetterCandidateLimit)
        items.resize(kSingleLetterCandidateLimit);
    items = expand_spellings(std::move(items));
    PlaceQueriedLatinWords(items, page_size);
    return items;
}
} // namespace FanyImeIpc
