#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace FanyImeIpc
{
// A translation batch can change glosses for words outside the visible page. Only a changed key that is actually
// on the first page is worth another frame; page-2 glosses can wait for the page to be built.
inline bool AnyTranslatedKeyOnPage(const std::vector<std::string> &page_words,
                                   const std::vector<std::string> &changed_keys)
{
    for (const std::string &key : changed_keys)
    {
        if (std::find(page_words.begin(), page_words.end(), key) != page_words.end())
            return true;
    }
    return false;
}
} // namespace FanyImeIpc
