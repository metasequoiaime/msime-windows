#pragma once

#include <cstddef>

// 双拼句中辅助码的输入形状（规则见 engine/core/syllable_helpcode.h）。
//
// TSF 在同步吃键阶段要先判断反引号是不是编码键，Server 随后按同一条规则决定收不收、引擎按它
// 解析输入串。三处必须一致：TSF 吃了而 Server 不收，两边的输入串从此分叉。TSF 不链接引擎、
// 没有音节表，所以这里只看形状，不判断前面的键是不是合法音节。
//
// 一段是反引号加至多两个字母：第一码（大小写都算）和紧跟其后的大写第二码。
//
// 触发键可以是反引号，也可以是分号（设置里多选）。无论按的是哪个，输入串里记的都是反引号：TSF 的
// 按键缓冲、Server 的 raw 和引擎解析的都是同一种串，分号在微软/搜狗/紫光双拼里还是 ing 韵母，
// 不能让它在串里有两种意思。分号只在 AcceptsMarkerAt 成立时才是触发键，那时这一节是偶数键，
// 而 ing 韵母只跟在奇数键之后，两者不会抢同一个位置。
//
// 双拼末尾辅助码关掉「直接」后改由引导键触发（间接辅助码），它和句中辅助码共用这里的扫描规则：
// 触发键（反引号、分号，设置里与句中辅助码互斥地分配）在输入串里一律记成 kEndMarker，后面跟至多
// 两个辅码（大小写都算）。大写触发同理：完整音节后的大写字母开一段末尾段，紧跟的一个字母（大小写
// 都算）是第二码。末尾段只在位于输入串末尾时按词的首末字筛候选，后面又接了拼音就只当分隔符。
namespace FanyImeMidSentenceHelpcode
{
inline constexpr char kMarker = '`';
inline constexpr char kSemicolonTrigger = ';';
// 末尾辅助码（间接）一段的开头。不是任何会进组合的按键字符，与反引号段、; 韵母都不会混淆。
inline constexpr char kEndMarker = '|';
// 编码按双拼音节读的本地模式的唤醒键：U 拆字、S 符号、T 日期时间。组合串以它开头时它不是双拼键，
// 见 AcceptsSemicolonFinalAt。快捷短语、超级简拼等只收字母的模式不在内：那里的 ; 仍是标点。
template <typename Char> constexpr bool IsSyllableModeKey(Char ch)
{
    return ch == Char('U') || ch == Char('S') || ch == Char('T');
}

// 哪种段由大写字母开头：句中辅助码的大写触发，或末尾辅助码（间接）的大写触发。两者互斥，都给时
// 按句中辅助码。由 bool 隐式构造时只表示句中辅助码的大写触发，与这一项出现之前的调用一致。
struct UppercaseTriggers
{
    bool mid = false;
    bool end = false;

    constexpr UppercaseTriggers(bool mid_trigger = false, bool end_trigger = false)
        : mid(mid_trigger), end(end_trigger && !mid_trigger)
    {
    }
};

enum class BlockKind
{
    // 句中辅助码：约束它前面那个音节上的字。
    Mid,
    // 末尾辅助码（间接）：按词的首末字筛整个候选。
    End,
};

template <typename Char> constexpr bool IsAsciiLetter(Char ch)
{
    return (ch >= Char('a') && ch <= Char('z')) || (ch >= Char('A') && ch <= Char('Z'));
}

template <typename Char> constexpr bool IsAsciiUpper(Char ch)
{
    return ch >= Char('A') && ch <= Char('Z');
}

// text[marker] 是反引号，返回这一段结束的位置。
template <typename Char> constexpr std::size_t BlockEnd(const Char *text, std::size_t size, std::size_t marker)
{
    std::size_t end = marker + 1;
    if (end < size && IsAsciiLetter(text[end]))
    {
        ++end;
        if (end < size && IsAsciiUpper(text[end]))
        {
            ++end;
        }
    }
    return end;
}

// text[first] 是一段末尾段的第一个字符（kEndMarker，或大写触发时的第一码），codes 是这个位置之后
// 还能收几个辅码。辅码大小写都算。
template <typename Char>
constexpr std::size_t EndBlockEnd(const Char *text, std::size_t size, std::size_t first, std::size_t codes)
{
    std::size_t end = first + 1;
    while (codes > 0 && end < size && IsAsciiLetter(text[end]))
    {
        ++end;
        --codes;
    }
    return end;
}

// 大写字母触发（设置里与反引号、分号并列多选）：完整双拼音节后面的大写字母自己开一段，相当于
// 「反引号 + 这个字母」，紧跟的大写字母仍是第二码。原串不改写——一个键换成两个字符会打乱 TSF 的
// 光标与退格——所以三处都按这里的规则解析带大写段的原串：
//
//   ulpbXihfa   ≡ ulpb`xihfa
//   ulpbXYih    ≡ ulpb`xYih
//
// 当前这一节已有的键数 chunk_length 是偶数且至少两个时，这个位置上的大写字母开一段。
template <typename Char> constexpr bool StartsUppercaseBlock(Char ch, std::size_t chunk_length)
{
    return IsAsciiUpper(ch) && chunk_length >= 2 && chunk_length % 2 == 0;
}

// text[first] 是大写段的第一码，返回这一段结束的位置。
template <typename Char> constexpr std::size_t UppercaseBlockEnd(const Char *text, std::size_t size, std::size_t first)
{
    std::size_t end = first + 1;
    if (end < size && IsAsciiUpper(text[end]))
    {
        ++end;
    }
    return end;
}

// 扫描 text[0, size)，每碰到一段（反引号段、末尾段或大写段）调用
// on_block(begin, end, codes_begin, kind)，codes_begin 是第一码的位置；返回最后一节（最后一个 ' 或段
// 之后）的起点。
template <typename Char, typename OnBlock>
constexpr std::size_t ScanBlocks(const Char *text, std::size_t size, UppercaseTriggers triggers, OnBlock &&on_block)
{
    std::size_t chunk_start = 0;
    std::size_t index = 0;
    while (index < size)
    {
        if (text[index] == Char('\''))
        {
            chunk_start = ++index;
        }
        else if (text[index] == Char(kMarker))
        {
            const std::size_t end = BlockEnd(text, size, index);
            on_block(index, end, index + 1, BlockKind::Mid);
            chunk_start = index = end;
        }
        else if (text[index] == Char(kEndMarker))
        {
            const std::size_t end = EndBlockEnd(text, size, index, 2);
            on_block(index, end, index + 1, BlockKind::End);
            chunk_start = index = end;
        }
        else if (triggers.mid && StartsUppercaseBlock(text[index], index - chunk_start))
        {
            const std::size_t end = UppercaseBlockEnd(text, size, index);
            on_block(index, end, index, BlockKind::Mid);
            chunk_start = index = end;
        }
        else if (triggers.end && StartsUppercaseBlock(text[index], index - chunk_start))
        {
            const std::size_t end = EndBlockEnd(text, size, index, 1);
            on_block(index, end, index, BlockKind::End);
            chunk_start = index = end;
        }
        else
        {
            ++index;
        }
    }
    return chunk_start;
}

template <typename Char>
constexpr std::size_t ChunkStart(const Char *text, std::size_t size, UppercaseTriggers triggers = {})
{
    return ScanBlocks(text, size, triggers, [](std::size_t, std::size_t, std::size_t, BlockKind) {});
}

// text[0, size) 末尾能否接一个反引号：当前这一节（最后一个 ' 或段之后）是偶数个、
// 至少两个键，也就是光标前恰好是一个个完整的双拼两键音节。
template <typename Char>
constexpr bool AcceptsMarker(const Char *text, std::size_t size, UppercaseTriggers triggers = {})
{
    const std::size_t chunk_length = size - ChunkStart(text, size, triggers);
    return chunk_length >= 2 && chunk_length % 2 == 0;
}

// 光标停在 text[caret] 时能否插入一个反引号：光标前按 AcceptsMarker 判断，后面的部分不看——
// 光标移回句中补辅助码时，它后面还有没敲完的音节。光标后紧跟的已经是这个音节的段时不收，
// 一个音节上叠两段只会让后一段把前一段盖掉；大写触发开着时，光标后的大写字母正好开这个音节的段。
template <typename Char>
constexpr bool AcceptsMarkerAt(const Char *text, std::size_t size, std::size_t caret, UppercaseTriggers triggers = {})
{
    return caret <= size && AcceptsMarker(text, caret, triggers) &&
           (caret == size ||
            (text[caret] != Char(kMarker) && !((triggers.mid || triggers.end) && IsAsciiUpper(text[caret]))));
}

// 光标停在 text[caret] 时能否插入一段末尾段（kEndMarker）：只在输入串末尾，且前面恰好是一个个完整
// 的双拼两键音节。末尾段只筛整串的候选，插在句中没有意义。
template <typename Char>
constexpr bool AcceptsEndMarkerAt(const Char *text, std::size_t size, std::size_t caret,
                                  UppercaseTriggers triggers = {})
{
    return caret == size && AcceptsMarker(text, size, triggers);
}

// 光标停在 text[caret] 时大写字母能否作为第二码：光标前恰好是一段只有第一码的句中段（反引号 + 一码，
// 或大写触发的一码）。末尾段的辅码大小写都算，不经过这里。
template <typename Char>
constexpr bool AcceptsSecondCodeAt(const Char *text, std::size_t size, std::size_t caret,
                                   UppercaseTriggers triggers = {})
{
    if (caret > size)
    {
        return false;
    }
    bool open = false;
    ScanBlocks(text, caret, triggers, [&](std::size_t, std::size_t end, std::size_t codes_begin, BlockKind kind) {
        open = kind == BlockKind::Mid && end == caret && end - codes_begin == 1;
    });
    return open && IsAsciiLetter(text[caret - 1]);
}

// 微软/搜狗/紫光双拼的 ; 是 ing 韵母，光标前这一节是奇数键时它是编码键。大写触发开着、或串里有末尾段时，
// 段不算这一节的键（ulpbXi; 里 i; 才是一个音节，ulpb|xy 后面的 ; 不是韵母）；都没有时保持原来只看最后
// 一个 ' 之后的规则。按音节打码的本地模式（见 IsSyllableModeKey）开头的唤醒键不是双拼键，不算进第一节：
// Ud; 里 d; 才是一个音节，Tx;qi 是 xingqi。TSF 看不到这些模式开没开，和 FanyImeDateTimeInput 认开头的 T
// 一样只按开头的字符判断，三处才不会分叉；开头的大写字母前面没有音节，本来也不会是辅助码段。
template <typename Char>
constexpr bool AcceptsSemicolonFinalAt(const Char *text, std::size_t size, std::size_t caret,
                                       UppercaseTriggers triggers)
{
    if (caret > size)
    {
        return false;
    }
    bool scan_blocks = triggers.mid || triggers.end;
    for (std::size_t index = 0; index < caret && !scan_blocks; ++index)
    {
        scan_blocks = text[index] == Char(kEndMarker);
    }
    std::size_t chunk_start = 0;
    if (scan_blocks)
    {
        chunk_start = ChunkStart(text, caret, triggers);
    }
    else
    {
        for (std::size_t index = caret; index > 0; --index)
        {
            if (text[index - 1] == Char('\''))
            {
                chunk_start = index;
                break;
            }
        }
    }
    if (chunk_start == 0 && caret > 0 && IsSyllableModeKey(text[0]))
    {
        chunk_start = 1;
    }
    return (caret - chunk_start) % 2 == 1;
}
} // namespace FanyImeMidSentenceHelpcode

// 末尾辅助码（间接）的触发键在 TSF 与 Server 之间的载荷（FanyImeWorkerReplyType::EndHelpcodeTriggersChanged）。
// 一个字符 '0'..'7'：位 1 反引号、位 2 分号、位 4 大写字母。Server 只发此刻真正生效的那几个——双拼、
// 开着双拼辅助码、关着「直接」、没开万象式直接辅助码，并去掉句中辅助码正占着的键。
namespace FanyImeEndHelpcode
{
inline constexpr unsigned kBacktick = 1u;
inline constexpr unsigned kSemicolon = 2u;
inline constexpr unsigned kUppercase = 4u;

template <typename Char> constexpr Char PayloadFor(bool backtick, bool semicolon, bool uppercase)
{
    return static_cast<Char>('0' + (backtick ? kBacktick : 0u) + (semicolon ? kSemicolon : 0u) +
                             (uppercase ? kUppercase : 0u));
}

template <typename Char> constexpr bool IsValidPayload(Char ch)
{
    return ch >= Char('0') && ch <= Char('7');
}

template <typename Char> constexpr unsigned FlagsFromPayload(Char ch)
{
    return IsValidPayload(ch) ? static_cast<unsigned>(ch - Char('0')) : 0u;
}
} // namespace FanyImeEndHelpcode
