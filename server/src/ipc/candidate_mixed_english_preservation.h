#pragma once

#include "engine/core/word_item.h"

#include <string>
#include <vector>

namespace FanyImeIpc
{
// A mixed English row is UI-only, so engine rebuilds would drop it. Keep every English row that still belongs to
// the current prefix, including completions: `pho` → `photo` has pinyin `photo`, not `pho`.
inline std::vector<WordItem> PreservedMixedEnglish(const std::vector<WordItem> &current, const std::string &input)
{
    std::vector<WordItem> preserved;
    if (input.empty())
        return preserved;
    for (const WordItem &item : current)
    {
        if (item.source != CandidateSource::EnglishDictionary || item.word.size() < input.size())
            continue;
        if (item.word.compare(0, input.size(), input) == 0)
            preserved.push_back(item);
    }
    return preserved;
}
} // namespace FanyImeIpc
