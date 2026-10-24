#pragma once

#include <string>

namespace ChineseConverter
{
/** 一段简体文字的三种繁体转换结果。 */
struct TraditionalForms
{
    // OpenCC 标准写法（s2t）。
    std::string standard;
    // 在标准写法上取台湾写法（t2tw）。
    std::string taiwan;
    // 在标准写法上取香港写法（t2hk）。
    std::string hong_kong;
};
} // namespace ChineseConverter
