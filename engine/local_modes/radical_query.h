#pragma once

#include "../core/word_item.h"

#include <filesystem>
#include <string>
#include <vector>

namespace metasequoia::local_modes
{
// U 模式（部件拆字反查）的候选。`code` 是唤醒键 U 之后的部分：去掉撇号、只含小写 a-z 的
// 部件拼音连写串，例如 晶 = 日+日+日 → "ririri"。按 code 前缀匹配：全码命中排在仅前缀
// 命中之前，各自按字频降序；同一个字的几种拆法只出一次。最多 limit 条；空 code、非小写
// 字母或表缺失都返回空集合。拆字表是会话级资源，调用方按自己的 RuntimePaths 解析路径。
std::vector<WordItem> query_radical(const std::filesystem::path &decomposition_path, const std::string &code,
                                    int limit = 50);
// 把拆字表读进进程内缓存。首次查询要解析十几万行，放在按键路径之外先调一次可以避开那一下。
void preload_radical_table(const std::filesystem::path &decomposition_path);
} // namespace metasequoia::local_modes
