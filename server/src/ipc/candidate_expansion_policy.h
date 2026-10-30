#pragma once

#include "global/globals.h"
#include "ipc/candidate_selection_policy.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace FanyImeIpc
{
// 这些来源只活在界面列表里，会话扩展重建不出来。
inline bool IsUiOnlyCandidateSource(CandidateSource source)
{
    switch (source)
    {
    case CandidateSource::CloudSuggestion:
    case CandidateSource::AiSuggestion:
    case CandidateSource::EnglishDictionary:
    case CandidateSource::Emoji:
    case CandidateSource::Kaomoji:
    case CandidateSource::Symbol:
    case CandidateSource::DateTime:
        return true;
    default:
        return false;
    }
}

// Session expansion rebuilds local candidates; asynchronous results only live in the UI list.
// Quick phrases are rebuilt by the caller, so carry over only asynchronous sources, deduplicating
// against the expanded list before applying the same ordering policy as worker callbacks.
// Returns false when the list did not change.
inline bool SetExpandedCandidatesKeepingPagePosition(Global::CandidateUiState &ui, std::vector<WordItem> expanded,
                                                     const EnglishPlacement &english_placement = {})
{
    if (ui.selection_started)
    {
        // 翻页扩展只追加尚未显示的候选。引擎可能已经有新的重排结果，不能挪动用户正在选的项。
        // 扩展结果是整份本地候选，按「词 + 拼音 + 来源 + 写法」逐条抵掉已显示的那些：同字不同音、
        // 快捷短语与词库同字、同一底词的不同繁体写法都是各自独立的候选，不能只按字去重。界面独有的
        // 异步候选扩展里重建不出来，和非选词路径一样按字挡掉与它同字的本地候选。
        // 两个底词撞写法时归属按整张列表定（见 ExpandTraditionalVariants）：整份候选里多出一个先前没显示的
        // 底词，一种已显示的写法可能改归它，例如追加批里的「著」收回「着」组的台湾写法「著」、排在「臺」词条
        // 前面的「台」收回「臺」。已显示的项不撤，所以新项的写法已经属于列表里另一个底词时丢掉新项，
        // 每种写法在列表里只属于一个底词。追加的项按组从 0 起重新编号（组以展开结果里 variant_rank 为 0 的项
        // 开头），被抵掉或丢掉的项不占序号，组首是这一组剩下的第一种写法。
        // 一组不会前半已显示、后半是新项：已显示项的写法只看它自己的底词、来源与地区，与列表里还有谁无关，
        // 选词期间也不并入异步候选，所以它在追加批里多出的写法一定已显示在别的底词下，会被丢掉。
        const auto identity = [](const WordItem &item) {
            return item.word + '\x1f' + item.pinyin + '\x1f' + std::to_string(static_cast<int>(item.source)) + '\x1f' +
                   item.output_text;
        };
        std::unordered_map<std::string, int> shown;
        std::unordered_set<std::string> ui_only_words;
        // 列表里每种写法（output_text）属于哪个底词。
        std::unordered_map<std::string, std::string> spelling_owners;
        for (const auto &candidate : ui.items)
        {
            ++shown[identity(candidate)];
            if (IsUiOnlyCandidateSource(candidate.source))
                ui_only_words.insert(candidate.word);
            if (!candidate.output_text.empty())
                spelling_owners.emplace(candidate.output_text, candidate.word);
        }
        const size_t size_before = ui.items.size();
        // 当前这组写法里下一个追加项的序号；展开结果里 variant_rank 为 0 的项开始新的一组。
        int next_rank_in_group = 0;
        for (auto &candidate : expanded)
        {
            if (candidate.variant_rank == 0)
                next_rank_in_group = 0;
            const auto it = shown.find(identity(candidate));
            if (it != shown.end() && it->second > 0)
            {
                --it->second;
                continue;
            }
            if (ui_only_words.count(candidate.word) != 0)
                continue;
            if (!candidate.output_text.empty())
            {
                const auto owner = spelling_owners.emplace(candidate.output_text, candidate.word).first;
                if (owner->second != candidate.word)
                    continue;
            }
            candidate.variant_rank = next_rank_in_group++;
            ui.items.push_back(std::move(candidate));
        }
        if (ui.items.size() == size_before)
            return false;
        ui.item_total_count = static_cast<int>(ui.items.size());
        ui.clear_page();
        return true;
    }
    for (const auto &candidate : ui.items)
    {
        if (IsUiOnlyCandidateSource(candidate.source) &&
            std::none_of(expanded.begin(), expanded.end(),
                         [&](const WordItem &item) { return item.word == candidate.word; }))
            expanded.push_back(candidate);
    }
    NormalizeMixedCandidateOrder(expanded, 1, english_placement);
    const int current_page = ui.page_index;
    const int current_selection = ui.selected_index_in_page;
    ui.set_items(std::move(expanded));
    ui.page_index = current_page;
    ui.selected_index_in_page = current_selection;
    return true;
}
} // namespace FanyImeIpc
