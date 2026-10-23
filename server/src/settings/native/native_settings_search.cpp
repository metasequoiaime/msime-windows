#include "settings/native/native_settings_search.h"

namespace native_settings
{
namespace
{
// Folds the forms a user is unlikely to distinguish when typing: ASCII case, and the full-width
// block U+FF01..U+FF5E down onto ASCII. Titles mix them freely ("ＡＩ 与云联想"), and a search box
// that misses "AI" against "ＡＩ" is the kind of thing people notice immediately.
wchar_t FoldChar(wchar_t c)
{
    if (c >= L'A' && c <= L'Z')
        return static_cast<wchar_t>(c - L'A' + L'a');
    if (c >= 0xFF01 && c <= 0xFF5E)
        return FoldChar(static_cast<wchar_t>(c - 0xFF01 + 0x21));
    return c;
}

// Position where every character of `query` appears in `text` in order. Returns -1 when they do
// not. A lower score is better: it is the span the match occupies, so the same query ranks
// "候选框" above a long description that mentions 候 and 选 far apart.
int MatchSpan(const std::wstring &text, const std::wstring &query)
{
    if (query.empty())
        return -1;
    size_t textIndex = 0;
    size_t first = std::wstring::npos;
    size_t last = std::wstring::npos;
    for (wchar_t raw : query)
    {
        const wchar_t wanted = FoldChar(raw);
        bool found = false;
        while (textIndex < text.size())
        {
            if (FoldChar(text[textIndex]) == wanted)
            {
                if (first == std::wstring::npos)
                    first = textIndex;
                last = textIndex;
                ++textIndex;
                found = true;
                break;
            }
            ++textIndex;
        }
        if (!found)
            return -1;
    }
    return static_cast<int>(last - first) - static_cast<int>(query.size()) + 1;
}
} // namespace

std::vector<SettingsIndexEntry> SearchSettings(const std::vector<SettingsIndexEntry> &entries,
                                               const std::wstring &query, size_t limit)
{
    std::vector<SettingsIndexEntry> hits;
    if (query.empty() || limit == 0)
        return hits;

    struct Scored
    {
        size_t order;
        bool inTitle;
        int span;
        const SettingsIndexEntry *entry;
    };
    std::vector<Scored> scored;
    scored.reserve(entries.size());

    for (size_t i = 0; i < entries.size(); ++i)
    {
        const SettingsIndexEntry &entry = entries[i];
        // A row whose page is gone would link to nothing.
        if (entry.row.expired())
            continue;
        const int titleSpan = MatchSpan(entry.title, query);
        if (titleSpan >= 0)
        {
            scored.push_back({i, true, titleSpan, &entry});
            continue;
        }
        const int descriptionSpan = MatchSpan(entry.description, query);
        if (descriptionSpan >= 0)
            scored.push_back({i, false, descriptionSpan, &entry});
    }

    std::stable_sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) {
        if (a.inTitle != b.inTitle)
            return a.inTitle;
        if (a.span != b.span)
            return a.span < b.span;
        return a.order < b.order;
    });

    for (const Scored &hit : scored)
    {
        if (hits.size() >= limit)
            break;
        hits.push_back(*hit.entry);
    }
    return hits;
}
} // namespace native_settings
