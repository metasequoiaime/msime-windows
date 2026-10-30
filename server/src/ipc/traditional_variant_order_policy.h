#pragma once

#include "ipc/traditional_region_policy.h"

#include <algorithm>
#include <string>
#include <vector>

namespace FanyImeIpc
{
/**
 * 一个简体词的全部繁体写法排成候选顺序：本地区写法、另一地区写法、OpenCC 标准写法，
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
} // namespace FanyImeIpc
