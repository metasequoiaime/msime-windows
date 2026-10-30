#pragma once

#include "local_query_result.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace metasequoia::local_modes
{
using QuickPhraseQueryResult = LocalQueryResult;

// K 模式：按编码前缀匹配，权重降序。
QuickPhraseQueryResult query_quick_phrases(const std::string &prefix, int limit = 100);
QuickPhraseQueryResult query_quick_phrases(const std::string &prefix, const std::filesystem::path &database_path,
                                           int limit = 100);

// 混进全拼/双拼普通候选的那组：按编码精确匹配，组内按权重降序。
QuickPhraseQueryResult query_quick_phrases_by_code(const std::string &code, int limit = 20);
QuickPhraseQueryResult query_quick_phrases_by_code(const std::string &code, const std::filesystem::path &database_path,
                                                   int limit = 20);

// 快捷短语组的槽位只数这些候选：词库、用户词库和整句。云/AI/英文/表情/颜文字按各自的
// 异步槽位插队，固定位置的候选按自己的位置放，都不参与计数。同一底词的繁体写法只数
// 第一写法（variant_rank 为 0 的那项）。
bool counts_toward_quick_phrase_slot(const WordItem &item);

// 把同码快捷短语整组插到第 slot 个普通候选之前（slot 为 0 即首位，固定位置候选仍在它们
// 自己的位置上）。与快捷短语同文字的其他候选会被去掉，只留快捷短语那份；列表里已有的
// 快捷短语先整体移除，重复调用结果不变。
void place_quick_phrases(std::vector<WordItem> &candidates, std::vector<WordItem> phrases, std::size_t slot);
} // namespace metasequoia::local_modes
