#pragma once

#include "../core/scheme_type.h"
#include "../core/word_item.h"
#include "../shuangpin/shuangpin_profile.h"

#include <filesystem>
#include <string>
#include <vector>

namespace metasequoia::local_modes
{
// U 模式（部件拆字反查）的候选。`code` 是唤醒键 U 之后的部分：去掉撇号、只含小写 a-z 的
// 部件拼音连写串，例如 晶 = 日+日+日 → "ririri"。按 code 前缀匹配：全码命中排在仅前缀
// 命中之前，各自按字频降序；同一个字的几种拆法只出一次。最多 limit 条；空 code、非小写
// 字母或表缺失都返回空集合。拆字表是会话级资源，调用方按自己的 RuntimePaths 解析路径。
// 每个候选的读音（第 4 列，可能为空，多音字用逗号分隔）填在 WordItem::reading 里，供候选窗反显。
std::vector<WordItem> query_radical(const std::filesystem::path &decomposition_path, const std::string &code,
                                    int limit = 50);
// 按输入方案查：全拼同上。双拼下 code 是部件的双拼码连写（小鹤 口 = kz，微软双拼的 ; 韵母也算），
// 每两键解成一个部件音节——拆字码只由完整音节组成，双拼没有切分歧义；末尾落单的一键是下一个
// 部件的开头，按它的声母（小鹤 v → zh）或零声母引导键（微软 o → a/e/o 开头）查前缀。双拼码的
// 命中排在前面，原串再按全拼查一遍排在后面，在双拼下打全拼仍然查得到。
std::vector<WordItem> query_radical(const std::filesystem::path &decomposition_path, const std::string &code,
                                    SchemeType scheme, const ShuangpinProfile &profile, int limit = 50);
// 把拆字表读进进程内缓存。首次查询要解析十几万行，放在按键路径之外先调一次可以避开那一下。
void preload_radical_table(const std::filesystem::path &decomposition_path);
} // namespace metasequoia::local_modes
