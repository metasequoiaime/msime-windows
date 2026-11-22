#pragma once

#include <cstddef>

// V 模式（数字转中文、算式计算）的输入形状。V 后面跟数字或左括号就进 V 模式：
//
//   V123        一百二十三、壹佰贰拾叁……
//   V123.45     壹佰贰拾叁元肆角伍分……
//   V1+2*3      7
//
// 双拼和五笔只认大写 V（小写 v 是双拼的编码键，也是五笔的码字，五笔下 v+数字是选词）；全拼大小写
// 都认，全拼里 v 不会出现在开头。小写 v 只有后面接了数字或左括号才算 V 模式，vip 这类输入照旧走
// 拼音和英文混输。
//
// 进了 V 模式后，数字和 . + - * / ( ) ** 都是编码键，数字不再选词：字母是选词键，a/A 是第一个候选、
// b/B 第二个，依此类推，空格上屏高亮的那个。Shift+数字不再是选词键，Shift+4 打出的 ￥ 直接上屏货币格式
// 大写的金额（见 IsCurrencySymbol），回车（含小键盘回车）直接上屏算术结果。
//
// TSF 在同步吃键阶段按这里判断，Server 按同一条规则改输入串，两边必须一致，否则输入串会分叉。
// TSF 看不到配置和方案，由 Server 用 VModeChanged 告诉它当前的 Trigger。
namespace FanyImeVModeInput
{
inline constexpr char kUpperPrefix = 'V';
inline constexpr char kLowerPrefix = 'v';

// VModeChanged 的载荷就是这个值的十进制字符："0" / "1" / "2"。
enum class Trigger
{
    Off = 0,           // 关着，或者其他方案（日文、专用英文等）
    UppercaseOnly = 1, // 双拼、五笔：只认大写 V
    AnyCase = 2,       // 全拼：V 和 v 都认
};

template <typename Char> constexpr bool IsDigit(Char ch)
{
    return ch >= Char('0') && ch <= Char('9');
}

// 算式里除数字以外的字符。
template <typename Char> constexpr bool IsOperator(Char ch)
{
    return ch == Char('.') || ch == Char('+') || ch == Char('-') || ch == Char('*') || ch == Char('/') ||
           ch == Char('(') || ch == Char(')');
}

template <typename Char> constexpr bool IsInputChar(Char ch)
{
    return IsDigit(ch) || IsOperator(ch);
}

// 紧跟在 V 后面、能开启 V 模式的字符。只认数字和左括号：小写 v 后面紧跟的 - . 等键在全拼里本来是翻页、
// 标点，不能被抢走。
template <typename Char> constexpr bool IsStartChar(Char ch)
{
    return IsDigit(ch) || ch == Char('(');
}

// 候选选词键：字母，大小写都认。V 模式的编码键里没有字母，所以它们空出来当快捷键。
template <typename Char> constexpr bool IsSelectionLetter(Char ch)
{
    return (ch >= Char('a') && ch <= Char('z')) || (ch >= Char('A') && ch <= Char('Z'));
}

// a/A → 0，b/B → 1……z/Z → 25；不是字母时 -1。
template <typename Char> constexpr int SelectionIndex(Char ch)
{
    return ch >= Char('a') && ch <= Char('z')   ? ch - Char('a')
           : ch >= Char('A') && ch <= Char('Z') ? ch - Char('A')
                                                : -1;
}

// 货币符号：中文键盘上 Shift+4 打出全角 ￥（U+FFE5），半角 ¥（U+00A5）也认。比较的是宽字符，
// 所以不接受窄字符，免得 char 装不下。
constexpr bool IsCurrencySymbol(unsigned int ch)
{
    return ch == 0x00A5u || ch == 0xFFE5u;
}

template <typename Char> constexpr bool IsPrefix(Char ch, Trigger trigger)
{
    return (trigger != Trigger::Off && ch == Char(kUpperPrefix)) ||
           (trigger == Trigger::AnyCase && ch == Char(kLowerPrefix));
}

// text 是不是一段 V 模式输入：开头是 V（或全拼的 v），后面紧跟数字或左括号。单独一个 V / v 还不算，它照旧
// 是普通的组合（英文、拼音的开头），接上第一个数字才进 V 模式。V 后面再混进别的键（比如字母）仍算 V 模式，
// 只是没有候选。
template <typename Char> constexpr bool IsComposition(const Char *text, std::size_t size, Trigger trigger)
{
    return text != nullptr && size >= 2 && IsPrefix(text[0], trigger) && IsStartChar(text[1]);
}

// 光标停在 text[caret] 时能否插入 ch：ch 是 V 模式的编码键，插入后整串仍是一段 V 模式输入。所以 V 后面
// 第一个键只能是数字或左括号，也不能把运算符插到 V 和第一个数字之间。
template <typename Char>
constexpr bool AcceptsAt(const Char *text, std::size_t size, std::size_t caret, Char ch, Trigger trigger)
{
    if (text == nullptr || size == 0 || caret < 1 || caret > size || !IsInputChar(ch) || !IsPrefix(text[0], trigger))
    {
        return false;
    }
    const Char second = caret == 1 ? ch : text[1];
    return IsStartChar(second);
}

constexpr Trigger TriggerFromPayload(wchar_t ch)
{
    return ch == L'2' ? Trigger::AnyCase : ch == L'1' ? Trigger::UppercaseOnly : Trigger::Off;
}

constexpr wchar_t PayloadFromTrigger(Trigger trigger)
{
    return trigger == Trigger::AnyCase ? L'2' : trigger == Trigger::UppercaseOnly ? L'1' : L'0';
}
} // namespace FanyImeVModeInput
