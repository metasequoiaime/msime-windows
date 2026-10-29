#pragma once

#include "conversion/traditional_spelling_index.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace FanyImeIpc
{
/**
 * 在 spellings 之上加用户的写法记录：转换结果、字表写法、简体判定与首选底词照 spellings，
 * RememberedSpellingOf(key, word) 每次都调 remembered(key, word)。spellings 要活得比这个对象久；
 * spellings 与 remembered 都能从多个线程同时调用时，这个对象也能。
 */
class TraditionalSpellingIndexWithChoices final : public ChineseConverter::TraditionalSpellingIndex
{
  public:
    /** （全拼键, 简体底词）记住的写法，没有记录时返回空。 */
    using RememberedSpellingLookup =
        std::function<std::optional<std::string>(const std::string &key, const std::string &word)>;

    TraditionalSpellingIndexWithChoices(const ChineseConverter::TraditionalSpellingIndex &spellings,
                                        RememberedSpellingLookup remembered)
        : spellings_(spellings), remembered_(std::move(remembered))
    {
    }

    ChineseConverter::TraditionalForms FormsOf(const std::string &simplified) const override
    {
        return spellings_.FormsOf(simplified);
    }

    std::vector<std::string> CharacterTableSpellingsOf(const std::string &character) const override
    {
        return spellings_.CharacterTableSpellingsOf(character);
    }

    bool IsSimplifiedSpelling(const std::string &text) const override
    {
        return spellings_.IsSimplifiedSpelling(text);
    }

    std::string PreferredWordOf(const std::string &spelling) const override
    {
        return spellings_.PreferredWordOf(spelling);
    }

    std::optional<std::string> RememberedSpellingOf(const std::string &key, const std::string &word) const override
    {
        return remembered_(key, word);
    }

  private:
    const ChineseConverter::TraditionalSpellingIndex &spellings_;
    RememberedSpellingLookup remembered_;
};
} // namespace FanyImeIpc
