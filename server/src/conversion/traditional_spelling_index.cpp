#include "traditional_spelling_index.h"

#include "chinese_converter.h"
#include "opencc_character_tables.h"

namespace ChineseConverter
{
namespace
{
class OpenCcIndex final : public TraditionalSpellingIndex
{
  public:
    TraditionalForms FormsOf(const std::string &simplified) const override
    {
        return ToTraditionalForms(simplified);
    }

    std::vector<std::string> CharacterTableSpellingsOf(const std::string &character) const override
    {
        return WithoutOtherIndependentSimplifiedCharacters(character, CharacterTableSpellings(character),
                                                           IsIndependentSimplifiedCharacter);
    }

    bool IsSimplifiedSpelling(const std::string &text) const override
    {
        return ChineseConverter::IsSimplifiedSpelling(text);
    }

    std::string PreferredWordOf(const std::string &spelling) const override
    {
        if (!IsSingleUtf8CodePoint(spelling))
            return {};
        const std::string simplified = TraditionalToSimplifiedCharacter(spelling);
        return simplified.empty() ? spelling : simplified;
    }
};
} // namespace

const TraditionalSpellingIndex &OpenCcTraditionalSpellingIndex()
{
    static const OpenCcIndex index;
    return index;
}
} // namespace ChineseConverter
