#pragma once

#include "conversion/traditional_spelling_index.h"
#include "ipc/traditional_region_policy.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace FanyImeIpc
{
/** text 按 UTF-8 码位切开，每段一个码位；末尾不完整的码位把剩下的字节当一段。 */
inline std::vector<std::string> SplitUtf8CodePoints(const std::string &text)
{
    std::vector<std::string> code_points;
    for (size_t offset = 0; offset < text.size();)
    {
        const size_t length = (std::min)(
            ChineseConverter::Utf8CodePointLength(static_cast<unsigned char>(text[offset])), text.size() - offset);
        code_points.push_back(text.substr(offset, length));
        offset += length;
    }
    return code_points;
}

/**
 * 一个简体词的全部繁体写法排成候选顺序，不计用户记录：本地区写法、另一地区写法、OpenCC 标准写法，
 * 再接单字字表里的写法（按字表原顺序）。
 * standard 是 s2t 的结果，taiwan、hong_kong 是在它之上 t2tw、t2hk 的结果；character_table 是单字的字表写法
 * （TraditionalSpellingIndex::CharacterTableSpellingsOf 给出的，按字表顺序），多字词传空。重复的写法只留第一次
 * 出现的位置，空串不算一种写法。
 */
inline std::vector<std::string> OrderTraditionalVariants(const std::string &standard, const std::string &taiwan,
                                                         const std::string &hong_kong,
                                                         const std::vector<std::string> &character_table,
                                                         TraditionalRegion region)
{
    const bool taiwan_first = region == TraditionalRegion::Taiwan;
    std::vector<std::string> ordered;
    const auto append = [&](const std::string &spelling) {
        if (!spelling.empty() && std::find(ordered.begin(), ordered.end(), spelling) == ordered.end())
            ordered.push_back(spelling);
    };
    append(taiwan_first ? taiwan : hong_kong);
    append(taiwan_first ? hong_kong : taiwan);
    append(standard);
    for (const auto &spelling : character_table)
        append(spelling);
    return ordered;
}

namespace traditional_variant_detail
{
// 不计用户记录、按地区排好的写法。
inline std::vector<std::string> RegionalSpellingsOf(const std::string &simplified,
                                                    const ChineseConverter::TraditionalSpellingIndex &index,
                                                    TraditionalRegion region)
{
    const auto forms = index.FormsOf(simplified);
    const std::vector<std::string> character_table = ChineseConverter::IsSingleUtf8CodePoint(simplified)
                                                         ? index.CharacterTableSpellingsOf(simplified)
                                                         : std::vector<std::string>{};
    return OrderTraditionalVariants(forms.standard, forms.taiwan, forms.hong_kong, character_table, region);
}
} // namespace traditional_variant_detail

/**
 * spelling 是不是简体词 simplified 的可选写法：两者码位数相同，且逐个位置上 spelling 的字等于 simplified
 * 的字，或是 index 给 simplified 那个字的写法（三种转换结果与字表写法）。空串不是任何词的可选写法。
 */
inline bool IsComposableTraditionalSpelling(const std::string &simplified, const std::string &spelling,
                                            const ChineseConverter::TraditionalSpellingIndex &index)
{
    if (spelling.empty())
        return false;
    const auto base_characters = SplitUtf8CodePoints(simplified);
    const auto spelled_characters = SplitUtf8CodePoints(spelling);
    if (base_characters.size() != spelled_characters.size())
        return false;
    for (size_t position = 0; position < base_characters.size(); ++position)
    {
        if (spelled_characters[position] == base_characters[position])
            continue;
        // 只问在不在里面，地区只影响先后。
        const auto allowed = traditional_variant_detail::RegionalSpellingsOf(base_characters[position], index,
                                                                             TraditionalRegion::Taiwan);
        if (std::find(allowed.begin(), allowed.end(), spelled_characters[position]) == allowed.end())
            return false;
    }
    return true;
}

/**
 * 一个简体词按地区排好序的全部繁体写法：index 的三种转换结果，单字再接 index 给的字表写法；多字词不查字表。
 * 顺序见 OrderTraditionalVariants。remembered 是用户在这个词上选过的写法：在排好的写法里时挪到第一位；
 * 不在里面、但是 simplified 的可选写法（见 IsComposableTraditionalSpelling）时插到第一位；两种情况其余写法都
 * 保留、顺序不变。为空或不是可选写法时不起作用。
 */
inline std::vector<std::string> TraditionalSpellingsOf(const std::string &simplified,
                                                       const ChineseConverter::TraditionalSpellingIndex &index,
                                                       TraditionalRegion region,
                                                       const std::optional<std::string> &remembered = std::nullopt)
{
    auto ordered = traditional_variant_detail::RegionalSpellingsOf(simplified, index, region);
    if (!remembered || remembered->empty())
        return ordered;
    const auto found = std::find(ordered.begin(), ordered.end(), *remembered);
    if (found != ordered.end())
        std::rotate(ordered.begin(), found, found + 1);
    else if (IsComposableTraditionalSpelling(simplified, *remembered, index))
        ordered.insert(ordered.begin(), *remembered);
    return ordered;
}
} // namespace FanyImeIpc
