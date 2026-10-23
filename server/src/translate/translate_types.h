#pragma once

#include "english/english_ime.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Translate
{
using Query = EnglishIme::TranslationQuery;
using Result = EnglishIme::TranslationResult;
using Direction = EnglishIme::TranslationDirection;

// 当前生效的提供方，和 [translation].provider 一一对应。
enum class Vendor
{
    Tencent,
    Aliyun,
    Custom,
    NiuTrans,
    Ai,
};

// 一次交给提供方的批次。sentence 只对自定义服务有意义：整句要更长的超时、
// 更大的输出上限和不同的提示词，腾讯/阿里云/小牛拿它当普通文本。
struct Batch
{
    std::vector<std::string> texts;
    std::string source;
    std::string target;
    bool sentence = false;
};

// 提供方的回包。translations 可能比 texts 短，表示时间预算先用完了，
// 后面的条目根本没发出去；调用方不能把它们当成失败写进退避表。
struct Outcome
{
    std::vector<std::string> translations;

    bool attempted(size_t index) const
    {
        return index < translations.size();
    }
};
} // namespace Translate
