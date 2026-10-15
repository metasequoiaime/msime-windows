#pragma once

#include "../core/word_item.h"

#include <filesystem>
#include <string>
#include <vector>

namespace metasequoia::local_modes
{
// U 模式（部件拆字反查）的候选。`code` 是唤醒键 U 之后的部分：去掉撇号、只含小写 a-z 的
// 部件拼音连写串，例如 晶 = 日+日+日 → "ririri"。按 code 前缀匹配，命中按字频降序，
// 最多 limit 条；空 code、非小写字母或表缺失都返回空集合。
std::vector<WordItem> query_radical(const std::string &code, int limit = 50);
// 显式指定拆字表路径的重载；供测试或非默认部署布局使用。
std::vector<WordItem> query_radical(const std::filesystem::path &decomposition_path, const std::string &code,
                                    int limit = 50);
} // namespace metasequoia::local_modes
