#pragma once

#include "engine/core/word_item.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace FanyImeIpc
{
// A mixed English row is UI-only, so engine rebuilds would drop it. Keep every English row that still belongs to
// the current prefix, including completions: `pho` → `photo` has pinyin `photo`, not `pho`.
// 比的是 pinyin（英文库的小写查询键），不是 word：word 是显示形式，`ue` 查出的 `UEFI` 拿 word 比永远
// 对不上，重建时就被丢掉。input 是小写的 EnglishInputKey。
inline std::vector<WordItem> PreservedMixedEnglish(const std::vector<WordItem> &current, const std::string &input)
{
    std::vector<WordItem> preserved;
    if (input.empty())
        return preserved;
    const auto lower = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
    for (const WordItem &item : current)
    {
        if (item.source != CandidateSource::EnglishDictionary)
            continue;
        const std::string &key = item.pinyin.empty() ? item.word : item.pinyin;
        if (key.size() < input.size())
            continue;
        if (std::equal(input.begin(), input.end(), key.begin(),
                       [&](char expected, char actual) { return lower(actual) == expected; }))
            preserved.push_back(item);
    }
    return preserved;
}

// emoji、颜文字、符号、日期时间的混输行同样只活在 ui.items 里，但它们按整串查出，没法像英文那样用前缀
// 判断是否仍属于当前输入：调用方先用它们合进来时的输入核对，再按来源（各自的开关）挑出要带过重建的行。
template <typename KeepSource>
std::vector<WordItem> PreservedMixedRows(const std::vector<WordItem> &current, const std::string &rows_input,
                                         const std::string &input, KeepSource keep_source)
{
    std::vector<WordItem> preserved;
    if (input.empty() || rows_input != input)
        return preserved;
    for (const WordItem &item : current)
    {
        switch (item.source)
        {
        case CandidateSource::Emoji:
        case CandidateSource::Kaomoji:
        case CandidateSource::Symbol:
        case CandidateSource::DateTime:
            if (keep_source(item.source))
                preserved.push_back(item);
            break;
        default:
            break;
        }
    }
    return preserved;
}
} // namespace FanyImeIpc
