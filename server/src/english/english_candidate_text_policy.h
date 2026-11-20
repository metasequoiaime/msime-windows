#pragma once

// 英文候选模式（Ctrl+Shift+E）的大小写、上屏补空格与纠错合并规则。纯函数，不碰全局状态。
// 规则参照 hallelujahIM（https://github.com/dongyuwei/hallelujahIM，GPL-3.0，作者 Dong Yuwei）
// 的 ConversionEngine / InputController，按本仓的候选模型改写。

#include "engine/core/word_item.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

namespace EnglishIme
{
namespace detail
{
inline bool IsAsciiUpper(unsigned char ch)
{
    return ch >= 'A' && ch <= 'Z';
}

inline bool IsAsciiLower(unsigned char ch)
{
    return ch >= 'a' && ch <= 'z';
}

inline std::string AsciiLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return IsAsciiUpper(ch) ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch);
    });
    return text;
}

inline std::string AsciiUpper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return IsAsciiLower(ch) ? static_cast<char>(ch - ('a' - 'A')) : static_cast<char>(ch);
    });
    return text;
}
} // namespace detail

/**
 * 按用户敲的大小写调整候选的写法：
 * - 敲的全是小写：照词库写法（Wikipedia、iPhone 这类专有写法不被压成小写）。
 * - 敲了两个以上字母且全是大写：整个词转大写（NASA、HTTP）。
 * - 其余情况，候选以敲的字母开头时，敲过的部分照原样、后半截接词库写法（Tes → Test，McD → McDonald's）；
 *   候选不以敲的字母开头（纠错结果）时，只在首字母大写时把候选首字母也大写。
 */
inline std::string WithTypedCase(const std::string &typed, const std::string &display)
{
    std::size_t letters = 0;
    std::size_t uppercase = 0;
    for (const unsigned char ch : typed)
    {
        if (detail::IsAsciiUpper(ch))
        {
            ++letters;
            ++uppercase;
        }
        else if (detail::IsAsciiLower(ch))
        {
            ++letters;
        }
    }
    if (uppercase == 0 || display.empty())
        return display;
    if (letters >= 2 && uppercase == letters)
        return detail::AsciiUpper(display);
    if (typed.size() <= display.size() &&
        detail::AsciiLower(display.substr(0, typed.size())) == detail::AsciiLower(typed))
        return typed + display.substr(typed.size());
    // 短语的键去掉了空格和标点（Accordingt → According to）：敲的字母逐个对到写法里的字母上，
    // 中间的空格、连字符、撇号照原样留着。
    std::string result = display;
    std::size_t matched = 0;
    for (std::size_t at = 0; at < result.size() && matched < typed.size(); ++at)
    {
        const unsigned char shown = static_cast<unsigned char>(result[at]);
        if (!detail::IsAsciiUpper(shown) && !detail::IsAsciiLower(shown))
            continue;
        const unsigned char letter = static_cast<unsigned char>(typed[matched]);
        const auto lower = [](unsigned char ch) { return detail::IsAsciiUpper(ch) ? ch + ('a' - 'A') : ch; };
        if (lower(letter) != lower(shown))
            break;
        result[at] = static_cast<char>(letter);
        ++matched;
    }
    if (matched == typed.size())
        return result;
    result = display;
    if (detail::IsAsciiUpper(static_cast<unsigned char>(typed.front())) &&
        detail::IsAsciiLower(static_cast<unsigned char>(result.front())))
        result.front() = static_cast<char>(result.front() - ('a' - 'A'));
    return result;
}

/**
 * 上屏的英文后面是否补空格。数字开头（版本号、序号）和以撇号结尾（rock 'n' roll 里的
 * 'n'、复数所有格）的不补，已经以空白结尾的也不补。
 */
inline bool ShouldAppendCommitSpace(const std::string &committed)
{
    if (committed.empty())
        return false;
    const unsigned char first = static_cast<unsigned char>(committed.front());
    const unsigned char last = static_cast<unsigned char>(committed.back());
    return !(first >= '0' && first <= '9') && last != '\'' && !std::isspace(last);
}

/**
 * 高亮英文候选下方那一行的查询键：词库行用它的小写键；纠错和原文行（Generated）只有在整串是
 * ASCII 字母、撇号或连字符时才按小写查。别的候选没有这一行，返回空。
 */
inline std::string DetailKey(bool dictionary_row, const std::string &key, const std::string &text)
{
    if (dictionary_row)
        return key;
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char ch) {
            return detail::IsAsciiUpper(ch) || detail::IsAsciiLower(ch) || ch == '\'' || ch == '-';
        }))
        return {};
    return detail::AsciiLower(text);
}

/**
 * 短语候选那一行的查询键：小写的短语写法（according to）。短语释义在 en_zh_glosses 里就按它存。
 */
inline std::string PhraseDetailKey(const std::string &display)
{
    return detail::AsciiLower(display);
}

/**
 * 英文候选模式的单词与短语合并成一张列表：键正好等于输入的单词、键正好等于输入的短语、其余单词、
 * 其余短语。两边各自已经按「键相等的在前、再按权重」排好，这里只按键是否相等切开拼接。
 */
inline std::vector<WordItem> MergeEnglishPhrases(const std::string &prefix, std::vector<WordItem> words,
                                                 std::vector<WordItem> phrases)
{
    if (phrases.empty())
        return words;
    const auto exact = [&](const WordItem &item) { return item.pinyin == prefix; };
    const auto words_split = std::stable_partition(words.begin(), words.end(), exact);
    const auto phrases_split = std::stable_partition(phrases.begin(), phrases.end(), exact);
    std::vector<WordItem> merged;
    merged.reserve(words.size() + phrases.size());
    merged.insert(merged.end(), std::make_move_iterator(words.begin()), std::make_move_iterator(words_split));
    merged.insert(merged.end(), std::make_move_iterator(phrases.begin()), std::make_move_iterator(phrases_split));
    merged.insert(merged.end(), std::make_move_iterator(words_split), std::make_move_iterator(words.end()));
    merged.insert(merged.end(), std::make_move_iterator(phrases_split), std::make_move_iterator(phrases.end()));
    return merged;
}

/**
 * 一个小写英文词或短语的音标，lookup 查 english.db 的 en_ipa（由 CMUdict 转写，按单词存）。
 * 整体查得到就用它；短语逐词查，每个词都有音标时用空格连起来，缺一个就不标，免得半截音标误导人。
 * 词典里的音标与释义不进候选窗，由停靠在候选窗旁的 DR Dict 弹窗显示（见 utils/dr_dict_dock.h）。
 */
template <class Lookup> std::string PhoneticOf(const std::string &term, Lookup &&lookup)
{
    if (term.empty())
        return {};
    std::string whole = lookup(term);
    if (!whole.empty() || term.find(' ') == std::string::npos)
        return whole;
    std::string joined;
    size_t start = 0;
    while (start < term.size())
    {
        size_t end = term.find(' ', start);
        if (end == std::string::npos)
            end = term.size();
        if (end > start)
        {
            const std::string ipa = lookup(term.substr(start, end - start));
            if (ipa.empty())
                return {};
            if (!joined.empty())
                joined += ' ';
            joined += ipa;
        }
        start = end + 1;
    }
    return joined;
}

struct SpellingCandidate
{
    std::string word;    // 小写键
    std::string display; // 写法
};

/**
 * 纠错候选的合并顺序：先取同读音的词库词（最多 per_source 个），再用系统拼写检查补到 limit 个。
 * 两边按小写去重，和输入本身相同的不要。
 */
inline std::vector<SpellingCandidate> MergeSpellingCandidates(const std::string &input,
                                                              const std::vector<SpellingCandidate> &phonetic,
                                                              const std::vector<std::string> &system,
                                                              std::size_t per_source, std::size_t limit)
{
    std::vector<SpellingCandidate> merged;
    std::vector<std::string> seen{detail::AsciiLower(input)};
    const auto take = [&](std::string word, const std::string &display) {
        word = detail::AsciiLower(std::move(word));
        if (word.empty() || std::find(seen.begin(), seen.end(), word) != seen.end())
            return false;
        seen.push_back(word);
        merged.push_back({std::move(word), display});
        return true;
    };
    std::size_t taken = 0;
    for (const auto &candidate : phonetic)
    {
        if (taken >= per_source || merged.size() >= limit)
            break;
        if (take(candidate.word, candidate.display))
            ++taken;
    }
    for (const auto &suggestion : system)
    {
        if (merged.size() >= limit)
            break;
        take(suggestion, suggestion);
    }
    return merged;
}
} // namespace EnglishIme
