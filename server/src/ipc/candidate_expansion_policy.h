#pragma once

#include "global/globals.h"
#include "ipc/candidate_selection_policy.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace FanyImeIpc
{
// Session expansion rebuilds local candidates; asynchronous results only live in the UI list.
// Quick phrases are rebuilt by the caller, so carry over only asynchronous sources, deduplicating
// against the expanded list before applying the same ordering policy as worker callbacks.
inline void SetExpandedCandidatesKeepingPagePosition(Global::CandidateUiState &ui, std::vector<WordItem> expanded,
                                                     const EnglishPlacement &english_placement = {})
{
    for (const auto &candidate : ui.items)
    {
        switch (candidate.source)
        {
        case CandidateSource::CloudSuggestion:
        case CandidateSource::AiSuggestion:
        case CandidateSource::EnglishDictionary:
        case CandidateSource::Emoji:
        case CandidateSource::Kaomoji:
        case CandidateSource::DateTime:
            if (std::none_of(expanded.begin(), expanded.end(),
                             [&](const WordItem &item) { return item.word == candidate.word; }))
                expanded.push_back(candidate);
            break;
        default:
            break;
        }
    }
    NormalizeMixedCandidateOrder(expanded, 1, english_placement);
    const int current_page = ui.page_index;
    const int current_selection = ui.selected_index_in_page;
    ui.set_items(std::move(expanded));
    ui.page_index = current_page;
    ui.selected_index_in_page = current_selection;
}
} // namespace FanyImeIpc
