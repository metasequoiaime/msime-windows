#pragma once

#include "../../../engine/contracts/custom_punctuation.h"

#include <string_view>

// 一次按键在自定义标点表里查到的结果。吃键时查一次表，之后随按键一路带到出符号：Worker 中途换表，
// 已经吃掉的键照样出吃键时那张表里的符号。只含定长数组，可以放进按键状态里按值复制。
struct CustomPunctuationPick
{
    // 自定义标点接管了这次按键。
    bool active = false;
    // 以 0 结尾的 UTF-16 符号；active 为假时为空。
    wchar_t symbol[FanyImeCustomPunctuation::kMaxSymbolUnits + 1] = {};

    std::wstring_view Symbol() const
    {
        return active ? std::wstring_view(symbol) : std::wstring_view();
    }
};
