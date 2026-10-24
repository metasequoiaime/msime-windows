#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FanyImeIpc
{
enum class HanCharacterEdge
{
    First,
    Last,
};

inline bool IsHanCodePoint(char32_t code_point)
{
    return code_point == 0x3007 || (code_point >= 0x3400 && code_point <= 0x4DBF) ||
           (code_point >= 0x4E00 && code_point <= 0x9FFF) || (code_point >= 0xF900 && code_point <= 0xFAFF) ||
           (code_point >= 0x20000 && code_point <= 0x2FA1F) || (code_point >= 0x30000 && code_point <= 0x323AF);
}

inline std::optional<std::string> ExtractHanCharacter(std::string_view text, HanCharacterEdge edge)
{
    std::optional<std::string> result;
    for (size_t offset = 0; offset < text.size();)
    {
        const size_t start = offset;
        const auto lead = static_cast<unsigned char>(text[offset++]);
        char32_t code_point = 0;
        size_t continuation_count = 0;
        if (lead < 0x80)
        {
            code_point = lead;
        }
        else if ((lead & 0xE0) == 0xC0)
        {
            code_point = lead & 0x1F;
            continuation_count = 1;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            code_point = lead & 0x0F;
            continuation_count = 2;
        }
        else if ((lead & 0xF8) == 0xF0)
        {
            code_point = lead & 0x07;
            continuation_count = 3;
        }
        else
        {
            return std::nullopt;
        }

        if (offset + continuation_count > text.size())
        {
            return std::nullopt;
        }
        for (size_t index = 0; index < continuation_count; ++index)
        {
            const auto continuation = static_cast<unsigned char>(text[offset++]);
            if ((continuation & 0xC0) != 0x80)
            {
                return std::nullopt;
            }
            code_point = (code_point << 6) | (continuation & 0x3F);
        }

        if (IsHanCodePoint(code_point))
        {
            result = std::string(text.substr(start, offset - start));
            if (edge == HanCharacterEdge::First)
            {
                return result;
            }
        }
    }
    return result;
}

inline std::wstring HighlightedCandidateText(const std::vector<std::wstring> &page_words, int selected_index_in_page)
{
    if (selected_index_in_page < 0 || static_cast<size_t>(selected_index_in_page) >= page_words.size())
    {
        return {};
    }
    return page_words[static_cast<size_t>(selected_index_in_page)];
}

/**
 * 输入串是不是 U 模式（部件拆字）组合：Shift+U 的唤醒标记仍在，且输入串以大写 U 开头。
 * 只看这两样，U 之后混进撇号、大写字母的串也算，只是查不出候选。
 */
inline bool IsRadicalComposition(bool radical_mode_triggered, std::string_view raw)
{
    return radical_mode_triggered && !raw.empty() && raw.front() == 'U';
}

/**
 * 当前候选列表是不是 U 模式的拆字结果：U 模式组合中，且候选框没有被译文页占着。
 * 译文页上列的是译义，不算拆字结果。
 */
inline bool IsRadicalCandidateList(bool radical_mode_triggered, bool translation_page_active, std::string_view raw)
{
    return !translation_page_active && IsRadicalComposition(radical_mode_triggered, raw);
}

/**
 * 候选文字显示与上屏前要不要做简转繁。
 * 日语方案的候选是假名与日文汉字，原样输出；U 模式拆字结果（见 IsRadicalCandidateList）是字表里
 * 确定的字形，同样原样输出，否则「干」「台」这类简繁同形字会被换成用户没有选的字。
 */
inline bool ShouldConvertCandidateTextToTraditional(bool traditional_character_set, bool japanese_scheme,
                                                    bool radical_candidate_list)
{
    return traditional_character_set && !japanese_scheme && !radical_candidate_list;
}

/**
 * 造词前缀接上本次选中的候选后要上屏的文字。to_output 把一段文字转成输出形式（简转繁等）。
 * 拆字结果不转，只转前缀；其余情形整串一起转：OpenCC 的词组规则会跨过前缀与候选的交界，
 * 「干」「燥」分开转得「幹燥」，整串转才得「乾燥」。
 */
template <typename ToOutput>
std::string CommitTextAfterCreatingWordPrefix(const std::string &prefix, const std::string &candidate,
                                              bool radical_candidate_list, ToOutput &&to_output)
{
    if (radical_candidate_list)
        return to_output(prefix) + candidate;
    return to_output(prefix + candidate);
}
} // namespace FanyImeIpc
