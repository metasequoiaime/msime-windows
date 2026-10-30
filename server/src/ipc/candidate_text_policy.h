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
 * 当前候选列表的内容会不会随简繁字形变：英文状态（english_input_mode）、日语方案、U 模式拆字列表下不变，
 * 切换字形后不必换写法。
 */
inline bool CandidateListFollowsCharacterSet(bool japanese_scheme, bool radical_candidate_list, bool english_input_mode)
{
    return !english_input_mode && ShouldConvertCandidateTextToTraditional(/*traditional_character_set=*/true,
                                                                          japanese_scheme, radical_candidate_list);
}

/**
 * 造词已选的部分或本次选中的候选：简体底词、屏幕上显示的文字，以及其中每一段是不是都选了第一写法
 * （判定见 ChosenTextOf）。
 */
struct ChosenText
{
    std::string word;
    std::string output_text;
    bool first_spellings = true;
};

/**
 * 一段选中的文字：word 是简体底词，output_text 是它在屏幕上显示（也就是上屏）的文字。
 * 显示的文字等于 first_spelling_of(word)（整词第一写法）时，这一段算选了第一写法。
 */
template <typename FirstSpellingOf>
ChosenText ChosenTextOf(const std::string &word, const std::string &output_text, FirstSpellingOf &&first_spelling_of)
{
    const bool first_spelling = output_text == first_spelling_of(word);
    return {word, output_text, first_spelling};
}

/**
 * 造词前缀或造好的整词的输出文字。first_spelling_of 给出一段简体文字整串的第一写法（不做简转繁时
 * 原样返回）。每段都选了第一写法时取整串的第一写法，OpenCC 的词组规则会跨过段与段的交界：
 * 「干」「燥」分开取是「幹燥」，整串取是「乾燥」。否则取屏幕上显示的拼接。
 */
template <typename FirstSpellingOf>
std::string ChosenTextForOutput(const ChosenText &chosen, FirstSpellingOf &&first_spelling_of)
{
    return chosen.first_spellings ? first_spelling_of(chosen.word) : chosen.output_text;
}

/**
 * 造词前缀接上本次选中的候选后要上屏的文字。拆字结果原样接在前缀的输出文字后面；
 * 前缀与候选都是第一写法时取两者简体拼接的整串第一写法；否则取屏幕上显示的拼接。
 */
template <typename FirstSpellingOf>
std::string CommitTextAfterCreatingWordPrefix(const ChosenText &prefix, const ChosenText &candidate,
                                              bool radical_candidate_list, FirstSpellingOf &&first_spelling_of)
{
    if (radical_candidate_list)
        return ChosenTextForOutput(prefix, first_spelling_of) + candidate.word;
    if (prefix.first_spellings && candidate.first_spellings)
        return first_spelling_of(prefix.word + candidate.word);
    return prefix.output_text + candidate.output_text;
}
} // namespace FanyImeIpc
