#pragma once

#include "engine/core/word_item.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace FanyImeIpc
{
// A full rebuild republished by async enrichment only needs to reach the screen when the page the user can
// actually see changed. A new sentence row appended to the second page is not a reason to repaint the first
// one; the neural reranker converges by adding such rows, and every repaint used to cost a visible frame.
inline bool VisibleCandidatePageChanged(const std::vector<WordItem> &before, const std::vector<WordItem> &after,
                                        std::size_t page_size)
{
    if (page_size == 0)
        page_size = 1;
    const std::size_t visible = (std::min)(page_size, (std::max)(before.size(), after.size()));
    for (std::size_t index = 0; index < visible; ++index)
    {
        if (index >= before.size() || index >= after.size())
            return true;
        if (before[index].word != after[index].word)
            return true;
    }
    return false;
}
} // namespace FanyImeIpc
