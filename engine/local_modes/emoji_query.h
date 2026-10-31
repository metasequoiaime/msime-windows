#pragma once

#include "local_query_result.h"
#include "../core/scheme_type.h"
#include "../shuangpin/shuangpin_profile.h"

#include <filesystem>
#include <string>

namespace metasequoia::local_modes
{
LocalQueryResult query_emoji(const std::string &code, SchemeType scheme, int limit = 10,
                             const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
LocalQueryResult query_emoji(const std::string &code, SchemeType scheme, const std::filesystem::path &database_path,
                             int limit = 10, const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());

// emoji 混输：只认整串精确等于某个键，不做前缀匹配。双拼下只查展开后的全拼，原始码恰好等于
// 某个简拼或英文键（xl → 😀）不是用户要打的词。
LocalQueryResult query_mixed_emoji(const std::string &code, SchemeType scheme,
                                   const std::filesystem::path &database_path, int limit = 3,
                                   const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());

// 按「词」查 emoji：用于候选展开面板把一个已成的中文词（高亮候选）映射到它的 emoji。与上面两条
// 不同——那两条走 emoji_pinyin 的拼音/英文键、只认 ASCII 输入；这里直接对 emoji 表的 keywords 列做
// 子串匹配：只要某个关键词含有查询词就命中（搜「右」既命中把「右」当独立关键词的 ➡️，也命中只在
// 「向右指」「右边」这类长关键词里带「右」的 👉）。整词命中排在子串命中之前，避免精确行被 LIMIT 截掉。
// keywords 列没有索引，整表扫描；只在面板打开时调用，不在每键路径上。keyword 为空或 limit<=0 返回空。
// 英文词映射将来同样走这里（keywords 里本就含英文关键词），届时只需在调用侧按需归一大小写。
LocalQueryResult query_emoji_by_keyword(const std::string &keyword, int limit = 9);
LocalQueryResult query_emoji_by_keyword(const std::string &keyword, const std::filesystem::path &database_path,
                                        int limit = 9);
} // namespace metasequoia::local_modes
