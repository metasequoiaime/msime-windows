#pragma once

// 成对标点表：左半边 → 右半边。TSF 用它决定自动补全补出哪个右半边、按到哪个右半边时跨过已补出的那一个；
// 设置页用它把一对合成一项显示（例【】）。两边都只读这一份。
namespace FanyImePairedPunctuation
{
struct Pair
{
    wchar_t opening;
    wchar_t closing;
};

inline constexpr Pair kPairs[] = {
    {L'{', L'}'},   {L'「', L'」'}, {L'『', L'』'}, {L'“', L'”'},   {L'‘', L'’'},   {L'（', L'）'},
    {L'《', L'》'}, {L'〈', L'〉'}, {L'【', L'】'}, {L'〖', L'〗'}, {L'〔', L'〕'}, {L'［', L'］'},
    {L'｛', L'｝'}, {L'«', L'»'},   {L'‹', L'›'},   {L'⟨', L'⟩'},   {L'〘', L'〙'}, {L'〚', L'〛'},
    {L'〝', L'〞'}, {L'﹁', L'﹂'}, {L'﹃', L'﹄'}, {L'︻', L'︼'}, {L'︗', L'︘'}, {L'︵', L'︶'},
    {L'︷', L'︸'}, {L'︹', L'︺'}, {L'︿', L'﹀'}, {L'︽', L'︾'}, {L'﹇', L'﹈'},
};

// 左半边对应的右半边；不是左半边时为 0。
inline constexpr wchar_t ClosingFor(wchar_t opening)
{
    for (const Pair &pair : kPairs)
    {
        if (pair.opening == opening)
            return pair.closing;
    }
    return 0;
}

inline constexpr bool IsClosing(wchar_t ch)
{
    for (const Pair &pair : kPairs)
    {
        if (pair.closing == ch)
            return true;
    }
    return false;
}
} // namespace FanyImePairedPunctuation
