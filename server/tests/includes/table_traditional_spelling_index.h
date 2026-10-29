#pragma once

#include "conversion/traditional_spelling_index.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace test
{
/**
 * 逐词给出三种转换结果、逐字给出字表写法的假索引，转换结果与字表写法互不牵连。forms 里没有的文字三种结果都是
 * 它自己；tables 里没有的字没有字表写法。traditional_words 里的文字不是简体写法，其余都是。单字写法的首选底词
 * 是它自己，多字写法为空串。写法记录交给 remembered(key, word)，不给时没有记录。
 */
class TableTraditionalSpellingIndex final : public ChineseConverter::TraditionalSpellingIndex
{
  public:
    using RememberedOf = std::function<std::optional<std::string>(const std::string &, const std::string &)>;

    explicit TableTraditionalSpellingIndex(std::map<std::string, ChineseConverter::TraditionalForms> forms,
                                           std::map<std::string, std::vector<std::string>> tables = {},
                                           std::set<std::string> traditional_words = {}, RememberedOf remembered = {})
        : forms_(std::move(forms)), tables_(std::move(tables)), traditional_words_(std::move(traditional_words)),
          remembered_(std::move(remembered))
    {
    }

    ChineseConverter::TraditionalForms FormsOf(const std::string &simplified) const override
    {
        const auto found = forms_.find(simplified);
        if (found != forms_.end())
            return found->second;
        return {simplified, simplified, simplified};
    }

    std::vector<std::string> CharacterTableSpellingsOf(const std::string &character) const override
    {
        const auto found = tables_.find(character);
        return found != tables_.end() ? found->second : std::vector<std::string>{};
    }

    bool IsSimplifiedSpelling(const std::string &text) const override
    {
        return traditional_words_.count(text) == 0;
    }

    std::string PreferredWordOf(const std::string &spelling) const override
    {
        return ChineseConverter::IsSingleUtf8CodePoint(spelling) ? spelling : std::string();
    }

    std::optional<std::string> RememberedSpellingOf(const std::string &key, const std::string &word) const override
    {
        return remembered_ ? remembered_(key, word) : std::nullopt;
    }

  private:
    std::map<std::string, ChineseConverter::TraditionalForms> forms_;
    std::map<std::string, std::vector<std::string>> tables_;
    std::set<std::string> traditional_words_;
    RememberedOf remembered_;
};
} // namespace test
