#pragma once

#include "conversion/traditional_spelling_index.h"
#include "ipc/persistent_traditional_variant_choices.h"

#include <optional>
#include <string>
#include <vector>

namespace FanyImeIpc
{
/**
 * 在 spellings 之上加用户的写法记录：转换结果、字表写法、简体判定与首选底词照 spellings，
 * RememberedSpellingOf 查 choices。两者都要活得比这个对象久；可以从多个线程同时调用。
 */
class TraditionalSpellingIndexWithChoices final : public ChineseConverter::TraditionalSpellingIndex
{
  public:
    TraditionalSpellingIndexWithChoices(const ChineseConverter::TraditionalSpellingIndex &spellings,
                                        PersistentTraditionalVariantChoices &choices)
        : spellings_(spellings), choices_(choices)
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
        return choices_.RememberedSpelling(key, word);
    }

  private:
    const ChineseConverter::TraditionalSpellingIndex &spellings_;
    PersistentTraditionalVariantChoices &choices_;
};
} // namespace FanyImeIpc
