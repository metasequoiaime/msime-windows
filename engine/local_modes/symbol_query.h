#pragma once

#include "local_query_result.h"
#include "../core/scheme_type.h"
#include "../shuangpin/shuangpin_profile.h"

#include <filesystem>
#include <string>

namespace metasequoia::local_modes
{
// Shift+S 符号模式：按 others.db 的 symbol_code 查编码（万象分组码 xl、sx、szq，或 typst/codex 的符号名
// alpha、arrowr），整串等于编码的排最前，其余按编码前缀、短编码在前。编码只认字母，不分大小写。
LocalQueryResult query_symbols(const std::string &code, int limit = 200);
LocalQueryResult query_symbols(const std::string &code, const std::filesystem::path &database_path, int limit = 200);
// 按输入方案查：全拼同上。双拼下先按原始编码查，再把编码按双拼每两键读成全拼查一遍
// （shuangpin::decode_syllable_pairs，小鹤 sj → san 的 三③），整串命中在前、前缀命中在后，各层里原始
// 编码在前；用 ; 当 ing 韵母的方案里 ; 也是编码键。
LocalQueryResult query_symbols(const std::string &code, SchemeType scheme, const ShuangpinProfile &profile,
                               int limit = 200);
LocalQueryResult query_symbols(const std::string &code, SchemeType scheme, const ShuangpinProfile &profile,
                               const std::filesystem::path &database_path, int limit = 200);

// 符号混输：只认整串精确等于编码、且建库时标了可混输的行（编码读不成拼音，或数字读音下的 ① ⑴ ⒈）。
LocalQueryResult query_mixed_symbols(const std::string &code, const std::filesystem::path &database_path,
                                     int limit = 3);
} // namespace metasequoia::local_modes
