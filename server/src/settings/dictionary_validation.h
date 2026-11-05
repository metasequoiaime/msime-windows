#pragma once

#include "engine/quanpin/quanpin_query.h"

#include <cstddef>
#include <string>
#include <unordered_map>

namespace SettingsDictionary::Validation
{
// 缺省权重按词库区分。拼音表与五笔表的量级差三个数量级——实测拼音 2 字词
// p50=2805、max=9931703，五笔 2 字词 p50=10、max=110。同一个 10000 落在拼音的
// p50~p60 合理区间，搬到五笔就是全库 max 的 91 倍，导入的词会碾压原生词条。
inline constexpr int kDefaultPinyinImportWeight = 10000;
inline constexpr int kDefaultWubiImportWeight = 30;

// 单字到全码的映射，供 ComposeWubiPhraseCode 按 86 词组取码规则组合词码。
using WubiCharCodes = std::unordered_map<std::string, std::string>;

// 按 86 词组取码规则由纯汉字词组推算五笔码：
//   1 字  该字全码原样
//   2 字  首字前 2 位 + 次字前 2 位
//   3 字  首字 1 位 + 次字 1 位 + 末字前 2 位
//   4+字 首、次、三、末各 1 位
// 多字词恒为四位；单字保持自然码长，**不补 z**——wubi86 表里 26600 条单字行本就按
// 自然码长存放，补位会让「节」既在 abj 又在 abjz 变成重复词条。
// 任一字查不到全码则返回空串，由调用方决定如何报错。
// 规则本身实测可复现词库 99.94% 的词码，未命中的 0.06% 是作者为避重码手工改过的。
std::string ComposeWubiPhraseCode(const std::string &word, const WubiCharCodes &char_codes);

bool NormalizeFullPinyin(const std::string &input, quanpin::Segments &segments, std::string &normalized,
                         std::size_t expected_syllables = 0);
bool ShouldSkipImportLine(const std::string &line, bool &in_yaml_header);
bool ParseCodedImportLine(const std::string &line, std::string &word, std::string &code, int &weight,
                          std::string &message, int default_weight = kDefaultPinyinImportWeight);
bool QuickPhraseFitsNamedPipe(const std::string &phrase);

// 快捷短语正文里的换行统一成 \n：编辑框里敲出来的是 \n，粘贴进来的可能是 \r\n 或裸 \r。
std::string NormalizeQuickPhraseText(const std::string &text);

// 导入导出走单行 TSV，字段内的换行/Tab/反斜杠写成 \\n、\\t、\\\\；未知转义原样保留，
// 免得手写文件里的正则或路径被吃掉。
std::string EscapeQuickPhraseField(const std::string &text);
std::string UnescapeQuickPhraseField(const std::string &text);
} // namespace SettingsDictionary::Validation
