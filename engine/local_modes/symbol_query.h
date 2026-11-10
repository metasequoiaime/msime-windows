#pragma once

#include "local_query_result.h"

#include <filesystem>
#include <string>

namespace metasequoia::local_modes
{
// Shift+S 符号模式：按 others.db 的 symbol_code 查编码（万象分组码 xl、sx、szq，或 typst/codex 的符号名
// alpha、arrowr），整串等于编码的排最前，其余按编码前缀、短编码在前。编码只认字母，不分大小写。
LocalQueryResult query_symbols(const std::string &code, int limit = 200);
LocalQueryResult query_symbols(const std::string &code, const std::filesystem::path &database_path, int limit = 200);

// 符号混输：只认整串精确等于编码、且建库时标了可混输的行（编码读不成拼音，或数字读音下的 ① ⑴ ⒈）。
LocalQueryResult query_mixed_symbols(const std::string &code, const std::filesystem::path &database_path,
                                     int limit = 3);

// 按「词」查符号：用于候选展开面板把一个已成的中文词（高亮候选）映射到它的符号。与上面两条不同——
// 那两条走 symbol_code 的编码键、只认 ASCII 输入；这里直接对 symbol_catalog 表的 keywords 列做子串
// 匹配（与 query_emoji_by_keyword、query_kaomoji_by_keyword 同语义：只要某个关键词含有查询词就命中，
// 整词命中排在子串命中之前，避免精确行被 LIMIT 截掉）。symbol_catalog 主键是 (symbol, category)，
// 同一符号可能挂在多个类别下，结果按符号去重。keywords 列没有索引，整表扫描；只在面板打开时调用，
// 不在每键路径上。keyword 为空或 limit<=0 返回空。
LocalQueryResult query_symbols_by_keyword(const std::string &keyword, int limit = 9);
LocalQueryResult query_symbols_by_keyword(const std::string &keyword, const std::filesystem::path &database_path,
                                          int limit = 9);
} // namespace metasequoia::local_modes
