#pragma once

#include "../../../engine/contracts/custom_punctuation.h"
#include "CustomPunctuationPick.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>

// 中文标点状态下一个字符键输出什么。查找顺序：自定义标点 > 顿号设置 > 默认映射（简单表、交替对 " '、
// 嵌套对 < >）。自定义标点接管了的按键不再出它的默认符号，也不推进交替对与嵌套对的状态。
// 自定义标点用吃键时查到的 CustomPunctuationPick（见 windows/src/Key/PunctuationKeyClaim.h），这里不查表。
//
// 不依赖 TSF 与 Windows 头文件，单独测试见 windows/tests/custom_punctuation.cpp。
class PunctuationKeyMap
{
  public:
    // 默认映射（含顿号设置）认不认这个键；不看自定义标点。
    static bool IsDefaultKey(wchar_t wch, unsigned enumerationCommaKeys);

    // 这个键输出的文本；没有映射时为空串。走到默认映射的交替对、嵌套对时推进它们的状态。
    std::wstring Resolve(wchar_t wch, unsigned enumerationCommaKeys, const CustomPunctuationPick &custom);

    // 成对补全替嵌套对的左键补出了右半边之后调用：那个右键不会再按，深度在这里退回。
    // 这次按键由自定义标点接管时什么都不做，因为解析它时深度没有前进。
    void BalanceNestPairAfterAutoClose(wchar_t openingKey, const CustomPunctuationPick &custom);

  private:
    bool doubleQuoteClosingNext_ = false;
    bool singleQuoteClosingNext_ = false;
    unsigned angleDepth_ = 0;
};

// 空格转换（智能标点）认的中文标点。
bool IsSmartPunctuationChinese(wchar_t ch);
// 这段上屏文本能不能被下一个空格转换：能时返回要转换的尾字符，不能时返回 0。成对补全补出的一对、
// 自定义标点出的符号（customSymbol）都不转换。
wchar_t SmartConversionTail(std::wstring_view committed, bool autoClosedPair, bool customSymbol);
// 转换成的英文标点；不在表里时为 0。顿号按出顿号的键位设定转 \ 或 /。
wchar_t SmartPunctuationAsciiFor(wchar_t chinese, unsigned enumerationCommaKeys);

// Server 推来的自定义标点表。Worker 线程换表，按键线程取快照，两边可以同时进行。
class CustomPunctuationStore
{
  public:
    void Replace(const FanyImeCustomPunctuation::Table &table);
    FanyImeCustomPunctuation::Table Snapshot() const;

  private:
    // 一行的定长副本，符号以 0 结尾。
    struct StoredRow
    {
        wchar_t trigger = 0;
        wchar_t symbol[FanyImeCustomPunctuation::kMaxSymbolUnits + 1] = {};
    };

    mutable std::mutex mutex_;
    std::size_t rowCount_ = 0;
    StoredRow rows_[FanyImeCustomPunctuation::kMaxRows] = {};
};
// 常量初始化、平凡析构：存在函数内 static 里时不带守卫位、不登记析构，/Zc:threadSafeInit- 下几条线程
// 同时第一次取用也安全。
static_assert((CustomPunctuationStore(), true), "CustomPunctuationStore must be constant-initializable");
static_assert(std::is_trivially_destructible_v<CustomPunctuationStore>,
              "CustomPunctuationStore must not register a destructor");
