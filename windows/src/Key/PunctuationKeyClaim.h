#pragma once

#include "../Composition/CustomPunctuationPick.h"
#include "../Composition/PunctuationKeyMap.h"

#include <cstring>

// 一次字符键按下时，自定义标点接不接管它；没有输入时，标点这一节吃不吃它。同步分类（_IsKeyEaten）与延迟分类
// （_ClassifyDeferredKeyDown）都把各自知道的状态填进 PunctuationKeyInput 转给 DecidePunctuationKey。有输入时
// 标点键吃不吃另由两条路径各自的输入键判定按字符决定，不看自定义标点。
// 不依赖 TSF 与 Windows 头文件，单独测试见 windows/tests/custom_punctuation.cpp。
namespace PunctuationKeyCodes
{
// Win32 的虚拟键码。KeyEventSink.cpp 里 static_assert 它们与 Windows 头文件的 VK_* 相同。
inline constexpr unsigned kMultiply = 0x6A;
inline constexpr unsigned kAdd = 0x6B;
inline constexpr unsigned kSubtract = 0x6D;
inline constexpr unsigned kDecimal = 0x6E;
inline constexpr unsigned kDivide = 0x6F;

// 美式键盘主键盘上这个标点键的虚拟键码（VK_OEM_*）；key 不是 FanyImeCustomPunctuation::kKeys 里的键时为 0。
inline constexpr unsigned MainKeyboardKey(char key)
{
    switch (key)
    {
    case '`':
        return 0xC0;
    case '-':
        return 0xBD;
    case '=':
        return 0xBB;
    case '[':
        return 0xDB;
    case ']':
        return 0xDD;
    case '\\':
        return 0xDC;
    case ';':
        return 0xBA;
    case '\'':
        return 0xDE;
    case ',':
        return 0xBC;
    case '.':
        return 0xBE;
    case '/':
        return 0xBF;
    default:
        return 0;
    }
}
} // namespace PunctuationKeyCodes

struct PunctuationKeyInput
{
    wchar_t character = 0;
    // 换算过 VK_PACKET 之后的虚拟键码。
    unsigned virtualKey = 0;
    bool shiftDown = false;
    // 中文标点状态（英文标点状态、标点锁定为英文时为假）。
    bool punctuationOpen = false;
    unsigned enumerationCommaKeys = 0;

    // 下面三项合成「输入进行中」，见 InputInProgress。
    // 有组合，或键入缓冲里有字。
    bool compositionActive = false;
    // 候选窗开着（任何一种）。
    bool candidateListShown = false;
    // 造词的撤回窗口：组合已经空了，正在造的词还在。
    bool createWordPending = false;

    // 候选窗开着，且不是边打边出的那种：标点这一节不收这个键。
    bool candidateListRejectsPunctuation = false;
    // 默认映射认的键不看上一项：延迟分类吃默认标点键时不看候选窗。只作用于默认键，自定义键不受影响。
    bool defaultKeysIgnoreCandidateList = false;
};

// 正在打拼音、候选窗开着，或在造词的撤回窗口里。
inline bool InputInProgress(const PunctuationKeyInput &input)
{
    return input.compositionActive || input.candidateListShown || input.createWordPending;
}

struct PunctuationKeyDecision
{
    // 标点这一节吃这个键（FUNCTION_PUNCTUATION）。
    bool claimed = false;
    // 这次按键出符号时用的自定义标点；没有接管时 active 为假。不管这个键最后由哪一节吃掉，出符号都用它。
    CustomPunctuationPick custom;
};

// 自定义标点只接管主键盘上那 11 个键：虚拟键码、Shift 与字符都要和映射里那一行的触发键对得上，
// 小键盘的 - + . / * 与 VK_PACKET 送来的字符都不接管。只在中文标点状态下接管。
// 只有自定义标点认的键（例 = + |）在输入进行中不吃，维持没有自定义标点时的处理；默认映射认的键照旧吃。
inline PunctuationKeyDecision DecidePunctuationKey(const PunctuationKeyInput &input,
                                                   const FanyImeCustomPunctuation::Table &table)
{
    PunctuationKeyDecision decision;
    if (!input.punctuationOpen)
        return decision;

    // 表按字符查；查到的那一行只在这次按键正是它的触发键时接管：同一字符的别的虚拟键（小键盘、VK_PACKET、
    // 102 键键盘多出的键）和 Shift 对不上的按法都不接管。
    for (const FanyImeCustomPunctuation::KeyInfo &key : FanyImeCustomPunctuation::kKeys)
    {
        const bool shifted = key.shifted == input.character;
        if ((shifted || static_cast<wchar_t>(key.key) == input.character) &&
            input.virtualKey == PunctuationKeyCodes::MainKeyboardKey(key.key) && input.shiftDown == shifted)
        {
            if (const std::wstring *symbol = table.Find(input.character))
            {
                decision.custom.active = true;
                std::memcpy(decision.custom.symbol, symbol->c_str(), symbol->size() * sizeof(wchar_t));
            }
        }
    }

    // 小键盘的 / 留给宿主，主键盘的 / 才按顿号设置出顿号。
    const bool defaultKey = input.virtualKey != PunctuationKeyCodes::kDivide &&
                            PunctuationKeyMap::IsDefaultKey(input.character, input.enumerationCommaKeys);
    const bool defaultClaim =
        defaultKey && (input.defaultKeysIgnoreCandidateList || !input.candidateListRejectsPunctuation);
    // 候选窗开着时 InputInProgress 已经为真，自定义键不必再看 candidateListRejectsPunctuation。
    const bool customClaim = decision.custom.active && !InputInProgress(input);
    decision.claimed = defaultClaim || customClaim;
    return decision;
}

// 候选上屏后接这个键时，键本身按 ASCII 跟在候选后面：小键盘的 . / - +，以及没被自定义标点接管的 - +。
inline bool AppendsAsciiAfterCandidate(unsigned virtualKey, wchar_t character, const CustomPunctuationPick &custom)
{
    return virtualKey == PunctuationKeyCodes::kDecimal || virtualKey == PunctuationKeyCodes::kDivide ||
           ((character == L'-' || character == L'+') && !custom.active);
}
