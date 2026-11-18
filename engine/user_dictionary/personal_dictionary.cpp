#include <metasequoia/personal_dictionary.h>
#include "../quanpin/quanpin_utils.h"
#include <utf8.h>
#include <algorithm>

namespace metasequoia
{
PersonalDictionaryValidation validate_personal_dictionary_entry(PersonalDictionaryEntry entry)
{
    const auto invalid = [](const char *message) { return PersonalDictionaryValidation{std::nullopt, message}; };
    if (entry.weight < 1 || entry.weight > 100000000)
        return invalid("Weight must be between 1 and 100000000");
    if (entry.key.empty() || entry.key.size() > 512 || entry.value.empty() || entry.value.size() > 4096 ||
        !utf8::is_valid(entry.value.begin(), entry.value.end()))
        return invalid("A valid, bounded UTF-8 word and input code are required");
    const bool quick = entry.kind == PersonalDictionaryKind::QuickPhrase;
    for (unsigned char ch : entry.value)
        if ((ch < 32 && !(quick && (ch == '\n' || ch == '\t'))) || ch == 127)
            return invalid("The word contains an unsupported control character");
    for (char &ch : entry.key)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    auto letters = [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; };
    switch (entry.kind)
    {
    case PersonalDictionaryKind::Pinyin: {
        // Require explicit syllables; guessing a split here could store a different pronunciation.
        std::replace(entry.key.begin(), entry.key.end(), ' ', '\'');
        std::size_t start = 0, count = 0;
        while (start <= entry.key.size())
        {
            const auto end = entry.key.find('\'', start);
            const auto syllable = entry.key.substr(start, end == std::string::npos ? end : end - start);
            if (!quanpin::intact_pinyin_set().count(syllable))
                return invalid("Use complete pinyin syllables separated by apostrophes or spaces");
            ++count;
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        if (count > 64 || count != static_cast<std::size_t>(utf8::distance(entry.value.begin(), entry.value.end())))
            return invalid("Each character must have one pinyin syllable (maximum 64)");
        break;
    }
    case PersonalDictionaryKind::Wubi:
    case PersonalDictionaryKind::Wubi98:
    case PersonalDictionaryKind::WubiXsj:
        if (entry.key.size() > 4 || !std::all_of(entry.key.begin(), entry.key.end(), letters))
            return invalid("Wubi codes contain one to four letters");
        break;
    case PersonalDictionaryKind::QuickPhrase:
        if (entry.key.size() > 32 || !std::all_of(entry.key.begin(), entry.key.end(), [&](unsigned char ch) {
                return letters(ch) || (ch >= '0' && ch <= '9');
            }))
            return invalid("Quick-phrase codes contain one to 32 letters or digits");
        break;
    case PersonalDictionaryKind::English: {
        std::string normalized = entry.value;
        for (char &ch : normalized)
            if (ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>(ch - 'A' + 'a');
        if (entry.key.size() > 64 || !std::all_of(entry.key.begin(), entry.key.end(), letters) ||
            normalized != entry.key)
            return invalid("English code must match the word's letters, ignoring case");
        break;
    }
    default:
        return invalid("Unsupported dictionary kind");
    }
    return {std::move(entry), {}};
}
} // namespace metasequoia
