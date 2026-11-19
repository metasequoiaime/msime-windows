// 召回策略移植自 hallelujahIM（GPL-3.0，作者 Dong Yuwei），phonex 移植自 talisman
// （MIT，Copyright (c) 2016 Guillaume Plique）。出处与许可见 english_spelling.h。
#include "english_spelling.h"

#include <algorithm>
#include <tuple>

namespace metasequoia::english
{
namespace
{
bool IsVowel(char ch)
{
    return ch == 'A' || ch == 'E' || ch == 'I' || ch == 'O' || ch == 'U' || ch == 'Y';
}

char EncodeInitial(char ch)
{
    if (IsVowel(ch))
        return 'A';
    switch (ch)
    {
    case 'B':
    case 'P':
        return 'B';
    case 'V':
    case 'F':
        return 'F';
    case 'K':
    case 'Q':
    case 'C':
        return 'C';
    case 'J':
    case 'G':
        return 'G';
    case 'Z':
    case 'S':
        return 'S';
    default:
        return ch;
    }
}
} // namespace

std::string phonex(std::string_view word)
{
    std::string name;
    name.reserve(word.size());
    for (const unsigned char ch : word)
    {
        if (ch >= 'a' && ch <= 'z')
            name.push_back(static_cast<char>(ch - ('a' - 'A')));
        else if (ch >= 'A' && ch <= 'Z')
            name.push_back(static_cast<char>(ch));
    }

    // 去掉末尾的 S。
    while (!name.empty() && name.back() == 'S')
        name.pop_back();

    // 几个词首组合先换掉。
    if (name.size() >= 2)
    {
        const std::string_view first_two(name.data(), 2);
        if (first_two == "KN")
            name.replace(0, 2, "N");
        else if (first_two == "PH")
            name.replace(0, 2, "F");
        else if (first_two == "WR")
            name.replace(0, 2, "R");
    }

    // 词首的 H 不发音。
    if (!name.empty() && name.front() == 'H')
        name.erase(name.begin());
    if (name.empty())
        return {};

    name.front() = EncodeInitial(name.front());

    std::string code(1, name.front());
    char last = code.back();
    const std::size_t length = name.size();
    for (std::size_t i = 1; i < length; ++i)
    {
        const char letter = name[i];
        const char next = i + 1 < length ? name[i + 1] : '\0';
        char encoding = '0';
        switch (letter)
        {
        case 'B':
        case 'P':
        case 'F':
        case 'V':
            encoding = '1';
            break;
        case 'C':
        case 'S':
        case 'K':
        case 'G':
        case 'J':
        case 'Q':
        case 'X':
        case 'Z':
            encoding = '2';
            break;
        case 'D':
        case 'T':
            if (next != 'C')
                encoding = '3';
            break;
        case 'L':
            if (IsVowel(next) || i + 1 == length)
                encoding = '4';
            break;
        case 'M':
        case 'N':
            // ND / NG / MD / MG：后一个字母并进鼻音，按同一个音处理。
            if (next == 'D' || next == 'G')
                name[i + 1] = letter;
            encoding = '5';
            break;
        case 'R':
            if (IsVowel(next) || i + 1 == length)
                encoding = '6';
            break;
        default:
            break;
        }

        if (encoding != last && encoding != '0')
            code.push_back(encoding);
        last = code.back();
    }
    return code;
}

std::size_t levenshtein(std::string_view left, std::string_view right, std::size_t limit)
{
    const std::size_t difference = left.size() > right.size() ? left.size() - right.size() : right.size() - left.size();
    if (difference > limit)
        return limit + 1;
    if (left.empty())
        return right.size();
    if (right.empty())
        return left.size();

    std::vector<std::size_t> previous(right.size() + 1);
    std::vector<std::size_t> current(right.size() + 1);
    for (std::size_t j = 0; j <= right.size(); ++j)
        previous[j] = j;
    for (std::size_t i = 1; i <= left.size(); ++i)
    {
        current[0] = i;
        std::size_t row_minimum = current[0];
        for (std::size_t j = 1; j <= right.size(); ++j)
        {
            const std::size_t substitution = previous[j - 1] + (left[i - 1] == right[j - 1] ? 0 : 1);
            current[j] = (std::min)({previous[j] + 1, current[j - 1] + 1, substitution});
            row_minimum = (std::min)(row_minimum, current[j]);
        }
        if (row_minimum > limit)
            return limit + 1;
        previous.swap(current);
    }
    return (std::min)(previous[right.size()], limit + 1);
}

void PhonexIndex::add(std::string_view word, std::string_view display, std::int64_t weight)
{
    if (word.size() < kMinimumLength)
        return;
    std::string code = phonex(word);
    if (code.empty())
        return;
    buckets_[std::move(code)].push_back({std::string(word), std::string(display), weight});
    ++size_;
}

std::vector<SpellingSuggestion> PhonexIndex::suggest(std::string_view input, std::size_t limit) const
{
    if (input.size() < kMinimumLength || limit == 0)
        return {};
    const auto bucket = buckets_.find(phonex(input));
    if (bucket == buckets_.end())
        return {};

    std::vector<SpellingSuggestion> suggestions;
    for (const auto &entry : bucket->second)
    {
        if (entry.word == input)
            continue;
        const std::size_t distance = levenshtein(input, entry.word, kMaximumDistance);
        if (distance <= kMaximumDistance)
            suggestions.push_back({entry.word, entry.display, entry.weight, distance});
    }
    std::sort(suggestions.begin(), suggestions.end(), [](const SpellingSuggestion &a, const SpellingSuggestion &b) {
        return std::tie(a.distance, b.weight, a.word) < std::tie(b.distance, a.weight, b.word);
    });
    if (suggestions.size() > limit)
        suggestions.resize(limit);
    return suggestions;
}
} // namespace metasequoia::english
