#pragma once

#include "conversion/traditional_spelling_index.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace test
{
/**
 * 按词查写法表的假索引。spellings 给每个词按台湾地区排好序的全部写法：查不到的词只有它自己一种写法，
 * 空列表表示没有写法。三种转换结果取表里的前三种写法（台湾取第一种、香港取第二种、标准取第三种，
 * 不够三种时取最后一种），单字的字表写法是整张列表，所以 TraditionalSpellingsOf 按台湾地区排出来正好是
 * 表里的顺序；多字词只用得到前三种。香港地区排出来的顺序与真数据不一定一致，要比香港顺序的用例用真索引。
 * 词里含 traditional_characters 中任一个字时不是简体写法，其余都是。首选底词查 preferred_words，
 * 查不到的单字写法是它自己，多字写法为空串。
 */
class FakeTraditionalSpellingIndex final : public ChineseConverter::TraditionalSpellingIndex
{
  public:
    FakeTraditionalSpellingIndex(std::map<std::string, std::vector<std::string>> spellings,
                                 std::vector<std::string> traditional_characters = {},
                                 std::map<std::string, std::string> preferred_words = {})
        : spellings_(std::move(spellings)), traditional_characters_(std::move(traditional_characters)),
          preferred_words_(std::move(preferred_words))
    {
    }

    ChineseConverter::TraditionalForms FormsOf(const std::string &simplified) const override
    {
        const auto listed = SpellingsOf(simplified);
        if (listed.empty())
            return {};
        const auto at = [&](size_t index) { return listed[(std::min)(index, listed.size() - 1)]; };
        ChineseConverter::TraditionalForms forms;
        forms.taiwan = at(0);
        forms.hong_kong = at(1);
        forms.standard = at(2);
        return forms;
    }

    std::vector<std::string> CharacterTableSpellingsOf(const std::string &character) const override
    {
        return SpellingsOf(character);
    }

    bool IsSimplifiedSpelling(const std::string &text) const override
    {
        return std::none_of(traditional_characters_.begin(), traditional_characters_.end(),
                            [&](const std::string &character) { return text.find(character) != std::string::npos; });
    }

    std::string PreferredWordOf(const std::string &spelling) const override
    {
        const auto found = preferred_words_.find(spelling);
        if (found != preferred_words_.end())
            return found->second;
        return ChineseConverter::IsSingleUtf8CodePoint(spelling) ? spelling : std::string();
    }

  private:
    std::vector<std::string> SpellingsOf(const std::string &text) const
    {
        const auto found = spellings_.find(text);
        return found != spellings_.end() ? found->second : std::vector<std::string>{text};
    }

    std::map<std::string, std::vector<std::string>> spellings_;
    std::vector<std::string> traditional_characters_;
    std::map<std::string, std::string> preferred_words_;
};
} // namespace test
