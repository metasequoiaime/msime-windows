#pragma once

#include "engine/core/word_item.h"
#include "engine/core/input_session_types.h"
#include "engine/common/helpcode_utils.h"
#include "ipc/traditional_variant_expansion_policy.h"
#include "jev_client.h"
#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Jev
{
inline bool SameQuery(const metasequoia::OnlineQuery &a, const metasequoia::OnlineQuery &b)
{
    return a.session_id == b.session_id && a.generation == b.generation && a.scheme == b.scheme &&
           a.identity == b.identity && a.query_text == b.query_text && a.cache_key == b.cache_key &&
           a.pinyin_segments == b.pinyin_segments && a.ai_eligible && b.ai_eligible;
}
// Only local dictionary and model candidates are offered to Jev or moved by its choice.
inline bool IsPoolSource(const WordItem &item)
{
    return item.source == CandidateSource::Database || item.source == CandidateSource::UserDatabase ||
           item.sentence_association || item.source == CandidateSource::NeuralDesktop ||
           item.source == CandidateSource::NeuralKeyboard || item.source == CandidateSource::Collocation;
}

inline std::vector<std::string> CandidatePool(const std::vector<WordItem> &items, size_t syllables)
{
    std::vector<std::string> result;
    if (syllables == 0)
        return result;
    for (const auto &item : items)
    {
        if (!IsPoolSource(item) || HelpcodeUtils::count_han_chars(item.word) != syllables ||
            HelpcodeUtils::count_utf8_chars(item.word) != syllables ||
            std::find(result.begin(), result.end(), item.word) != result.end())
            continue;
        result.push_back(item.word);
        if (result.size() == kMaxCandidates)
            break;
    }
    return result;
}

// Only the slots held by unpinned pool candidates are reordered, so cloud, English, date, emoji and
// user-pinned rows keep their place in the sequence of candidates NormalizeMixedCandidateOrder gave
// them. Within those slots the local first/second choice stay first and the choice becomes the third.
// In the traditional character set a candidate is a spelling group as FanyImeIpc::SpellingGroupStart
// draws it: a group moves as a whole and takes one slot. Rows that do not move keep their place in the
// sequence of groups; their index in items changes when the groups in front of them change size.
// Preserve the full WordItem: pinyin, provenance and dictionary learning still belong to the original
// candidate.
inline bool Promote(std::vector<WordItem> &items, const std::string &word)
{
    // [begin, end) of each spelling group; an unexpanded candidate is a group of one.
    std::vector<std::pair<size_t, size_t>> groups;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (FanyImeIpc::SpellingGroupStart(items, i) != i)
            groups.back().second = i + 1;
        else
            groups.emplace_back(i, i + 1);
    }
    std::vector<size_t> slots;
    for (size_t g = 0; g < groups.size(); ++g)
    {
        const WordItem &head = items[groups[g].first];
        if (IsPoolSource(head) && head.fixed_position == 0)
            slots.push_back(g);
    }
    const auto selected =
        std::find_if(slots.begin(), slots.end(), [&](size_t g) { return items[groups[g].first].word == word; });
    if (selected == slots.end())
        return false;
    auto rank = static_cast<size_t>(selected - slots.begin());
    if (rank <= 2)
        return true;
    std::vector<size_t> order(groups.size());
    for (size_t g = 0; g < groups.size(); ++g)
        order[g] = g;
    for (; rank > 2; --rank)
        std::swap(order[slots[rank]], order[slots[rank - 1]]);
    std::vector<WordItem> reordered;
    reordered.reserve(items.size());
    for (const size_t g : order)
        for (size_t i = groups[g].first; i < groups[g].second; ++i)
            reordered.push_back(std::move(items[i]));
    items = std::move(reordered);
    return true;
}
} // namespace Jev
