#pragma once

#include "mid_sentence_helpcode.h"

#include <cstddef>

// 双拼直接辅助码（万象式，规则见 engine/direct_helpcode/spelling_graph.h）的按键形状。
//
// 直接辅助码不要引导键：辅码字母就是普通字母键，TSF 本来就吃。要额外判断的只有两个键：
//
//   /  四码（双拼 + 两位辅码）后面的终止键：uiab/。不当编码键时它是「上屏高亮候选 + /」。
//   ;  微软/搜狗等双拼的 ing 韵母。平时按「所在一节是奇数键」判断，可辅码会打乱奇偶（uia 后面
//      接 x; 时这一节是偶数键），开着直接辅助码时改成只看前一个键是不是字母。
//
// TSF 在同步吃键阶段先判断，Server 随后按同一条规则决定收不收、引擎按它解析。三处必须一致，否则
// 两边的输入串会分叉。TSF 不链接引擎、没有音节表，所以这里只看形状，/ 到底凑不凑得成四码由引擎
// 判断，凑不成时引擎把它当分隔符处理，不会让整串作废。
//
// 四码也可以用第二位辅码大写来结束（uiaB，设置里和 / 各一个勾）。大写字母本来就是编码键，TSF 不用
// 另外判断；但 / 没勾时 TSF 不能再吃它，所以 DirectHelpcodeChanged 的载荷要带上 / 勾没勾。
namespace FanyImeDirectHelpcode
{
inline constexpr char kSlash = '/';

// DirectHelpcodeChanged 的载荷（一个字符）："0" 关，"1" 开且 / 是四码的终止键，"2" 开但 / 不是编码键。
inline constexpr wchar_t PayloadFor(bool enabled, bool slash)
{
    return !enabled ? L'0' : slash ? L'1' : L'2';
}

inline constexpr bool IsValidPayload(wchar_t payload)
{
    return payload == L'0' || payload == L'1' || payload == L'2';
}

inline constexpr bool EnabledFromPayload(wchar_t payload)
{
    return payload == L'1' || payload == L'2';
}

inline constexpr bool SlashFromPayload(wchar_t payload)
{
    return payload == L'1';
}

template <typename Char> constexpr bool IsAsciiLetter(Char ch)
{
    return (ch >= Char('a') && ch <= Char('z')) || (ch >= Char('A') && ch <= Char('Z'));
}

// 光标停在 text[caret] 时能否插入 /：光标前是「两键音节 + 两个辅码字母」的形状（音节第二键可以
// 是 ; 韵母），且光标后不是已经有一个 /。
template <typename Char> constexpr bool AcceptsSlashAt(const Char *text, std::size_t size, std::size_t caret)
{
    if (caret < 4 || caret > size || (caret < size && text[caret] == Char(kSlash)))
    {
        return false;
    }
    return IsAsciiLetter(text[caret - 1]) && IsAsciiLetter(text[caret - 2]) &&
           (IsAsciiLetter(text[caret - 3]) || text[caret - 3] == Char(';')) && IsAsciiLetter(text[caret - 4]);
}

// 光标停在 text[caret] 时 ; 能否作为 ing 韵母（只对用 ; 当韵母的双拼方案有意义）。
template <typename Char> constexpr bool AcceptsSemicolonFinalAt(const Char *text, std::size_t size, std::size_t caret)
{
    return caret >= 1 && caret <= size && IsAsciiLetter(text[caret - 1]);
}

// 直接辅助码下用引导键进入末尾辅助码（helpcode.shuangpin_direct_helpcode_end_trigger）：引导键是末尾辅助码
// 设置里勾了的反引号、分号（helpcode.shuangpin_helpcode_trigger_backtick / _semicolon；大写字母在直接辅助码
// 里本身是辅码，不算）。Server 用直接辅助码的切分把每个音节上的辅码删掉，输入串改写成「纯双拼 + kEndMarker」
// （uixulpb → uiulpb|），后面的一到两个字母按末尾辅助码筛候选；删掉这个 kEndMarker 时恢复改写前的原串。改写
// 只有 Server 算得出，所以引导键和这个退格 TSF 都不自己改缓冲，而是等 Server 的 CompositionRestored 照着
// 重建；Server 不改写时（前面切不成一个个「完整双拼 + 0~2 个辅码」）就按原来的规则把引导键当标点。
//
// TSF 没有音节表，只能按形状预判吃不吃键，并在交给 Server 时打上 FanyImePipeFlags::DirectHelpcodeEndTrigger
// Rewrite。DirectHelpcodeEndTriggerChanged 的载荷（FanyImeEndHelpcode::PayloadFor，不带大写位）里有键时才看
// 下面两条。

// 光标在 text 末尾时引导键能否开末尾辅助码：串里只有字母、手动分隔符 ' 和四码的 /，第一键是小写字母（大写
// 开头的是 U/T/V 等特殊模式），每一节（' 之间）至少两键，最后一键不是 '，也还没有末尾段。
template <typename Char> constexpr bool AcceptsEndTriggerAt(const Char *text, std::size_t size, std::size_t caret)
{
    if (caret != size || size < 2 || text[0] < Char('a') || text[0] > Char('z') || text[size - 1] == Char('\''))
    {
        return false;
    }
    std::size_t chunk = 0;
    for (std::size_t index = 0; index < size; ++index)
    {
        if (text[index] == Char('\''))
        {
            if (chunk < 2)
            {
                return false;
            }
            chunk = 0;
        }
        else if (IsAsciiLetter(text[index]) || text[index] == Char(kSlash))
        {
            ++chunk;
        }
        else
        {
            return false;
        }
    }
    return chunk >= 2;
}

// 光标在 text 末尾、紧挨着的是末尾段的开头时，退格删的就是进入末尾辅助码的那个 ;，由 Server 决定恢复
// 原串还是只删这一个字符。
template <typename Char> constexpr bool IsEndTriggerBackspaceAt(const Char *text, std::size_t size, std::size_t caret)
{
    return caret == size && size >= 1 && text[size - 1] == Char(FanyImeMidSentenceHelpcode::kEndMarker);
}
} // namespace FanyImeDirectHelpcode
