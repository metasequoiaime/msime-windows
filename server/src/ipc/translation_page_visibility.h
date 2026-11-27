#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace FanyImeIpc
{
// A translation batch can change glosses for words outside the visible page. Only a changed key that is actually
// on the current page is worth another frame; off-page glosses can wait for that page to be built.
inline bool AnyTranslatedKeyOnPage(const std::vector<std::string> &page_keys,
                                   const std::vector<std::string> &changed_keys)
{
    for (const std::string &key : changed_keys)
    {
        if (std::find(page_keys.begin(), page_keys.end(), key) != page_keys.end())
            return true;
    }
    return false;
}

// Collect the canonical translation lookup keys for the visible slice [start, start + count) of `items`,
// independent of Traditional/Simplified or typed-case display text stored in page_words.
template <typename Item, typename KeyExtractor>
inline std::vector<std::string> VisiblePageTranslationKeys(const std::vector<Item> &items, int start, int count,
                                                           KeyExtractor &&extract_key)
{
    std::vector<std::string> keys;
    if (start < 0 || count <= 0)
        return keys;
    const size_t begin = static_cast<size_t>(start);
    if (begin >= items.size())
        return keys;
    const size_t available = items.size() - begin;
    const size_t length = (std::min)(static_cast<size_t>(count), available);
    keys.reserve(length);
    for (size_t i = 0; i < length; ++i)
    {
        std::string key = extract_key(items[begin + i]);
        if (!key.empty())
            keys.push_back(std::move(key));
    }
    return keys;
}
} // namespace FanyImeIpc
