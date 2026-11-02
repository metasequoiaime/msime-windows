#include "shuangpin_latin_words.h"

#include "../quanpin/quanpin_query.h"
#include "shuangpin_utils.h"

#include <utf8.h>

#include <algorithm>
#include <iterator>
#include <string_view>
#include <unordered_map>

namespace
{
// 一个词的敲法按音节的不同敲法相乘，个别多音节词会乘出一大串，截到这么多。
constexpr std::size_t kCodesPerWordLimit = 8;
} // namespace

void ShuangpinLatinWords::build(std::shared_ptr<const quanpin::LatinWords> source, const ShuangpinProfile &profile)
{
    codes_.clear();
    max_code_length_ = 0;
    words_ = std::move(source);
    if (!words_ || words_->empty())
        return;
    const quanpin::LatinWords &words = *words_;

    // 音节 -> 本方案里能敲出它的两键。逐个试所有两键组合，和切分时的解码走同一个函数，
    // 自定义方案也不用单独处理。
    static constexpr char kKeys[] = "abcdefghijklmnopqrstuvwxyz;";
    std::unordered_map<std::string, std::vector<std::string>> syllable_codes;
    for (const char first : std::string_view(kKeys, sizeof(kKeys) - 1))
    {
        for (const char second : std::string_view(kKeys, sizeof(kKeys) - 1))
        {
            const std::string code{first, second};
            const std::string syllable = ShuangpinUtil::cvt_single_sp_to_pinyin(code, profile);
            if (!syllable.empty())
                syllable_codes[syllable].push_back(code);
        }
    }

    for (const auto &word : words.all())
    {
        const auto segments = quanpin::split_segments(word.key);
        // 键里的 a 既可能是字母 A，也可能是音节「啊」：按词里对应位置的字是不是拉丁字母来分。
        std::vector<char32_t> chars;
        utf8::unchecked::utf8to32(word.value.begin(), word.value.end(), std::back_inserter(chars));
        if (chars.size() != segments.size())
            continue;
        std::vector<std::string> codes{std::string{}};
        bool encodable = true;
        for (std::size_t i = 0; i < segments.size() && encodable; ++i)
        {
            const bool letter = chars[i] < 0x80;
            const auto found = letter ? syllable_codes.end() : syllable_codes.find(segments[i]);
            if (!letter && found == syllable_codes.end())
            {
                encodable = false;
                break;
            }
            std::vector<std::string> next;
            for (const auto &prefix : codes)
            {
                if (letter)
                {
                    next.push_back(prefix + segments[i]);
                    continue;
                }
                for (const auto &code : found->second)
                {
                    if (next.size() < kCodesPerWordLimit)
                        next.push_back(prefix + code);
                }
            }
            codes = std::move(next);
        }
        if (!encodable)
            continue;
        for (auto &code : codes)
        {
            max_code_length_ = (std::max)(max_code_length_, code.size());
            codes_.emplace_back(std::move(code), &word);
        }
    }
    std::sort(codes_.begin(), codes_.end(), [](const auto &left, const auto &right) {
        return left.first != right.first ? left.first < right.first : left.second->weight > right.second->weight;
    });
    codes_.erase(std::unique(codes_.begin(), codes_.end()), codes_.end());
}

std::vector<const quanpin::LatinWord *> ShuangpinLatinWords::exact(const std::string &keys) const
{
    std::vector<const quanpin::LatinWord *> result;
    if (keys.empty())
        return result;
    auto it = std::lower_bound(codes_.begin(), codes_.end(), keys,
                               [](const auto &entry, const std::string &target) { return entry.first < target; });
    for (; it != codes_.end() && it->first == keys; ++it)
        result.push_back(it->second);
    return result;
}

std::vector<const quanpin::LatinWord *> ShuangpinLatinWords::completions(const std::string &keys,
                                                                         std::size_t limit) const
{
    std::vector<const quanpin::LatinWord *> result;
    if (keys.empty() || limit == 0)
        return result;
    auto it = std::lower_bound(codes_.begin(), codes_.end(), keys,
                               [](const auto &entry, const std::string &target) { return entry.first < target; });
    for (; it != codes_.end() && it->first.compare(0, keys.size(), keys) == 0; ++it)
    {
        if (it->first.size() > keys.size() && !it->second->exact_only &&
            std::find(result.begin(), result.end(), it->second) == result.end())
            result.push_back(it->second);
    }
    std::stable_sort(result.begin(), result.end(), [](const quanpin::LatinWord *left, const quanpin::LatinWord *right) {
        return left->weight > right->weight;
    });
    if (result.size() > limit)
        result.resize(limit);
    return result;
}
