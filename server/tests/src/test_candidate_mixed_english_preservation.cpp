#include "ipc/candidate_mixed_english_preservation.h"
#include "tests/includes/test_framework.h"

#include <string>
#include <vector>

namespace
{
WordItem EnglishWord(std::string word)
{
    WordItem item;
    item.word = word;
    item.pinyin = word;
    item.source = CandidateSource::EnglishDictionary;
    return item;
}

WordItem ChineseWord(std::string word)
{
    WordItem item;
    item.word = std::move(word);
    item.source = CandidateSource::Database;
    return item;
}

std::vector<std::string> Words(const std::vector<WordItem> &items)
{
    std::vector<std::string> words;
    for (const auto &item : items)
        words.push_back(item.word);
    return words;
}
} // namespace

TEST_CASE(mixed_english_preservation_keeps_exact_and_prefix_rows)
{
    const std::vector<WordItem> current{
        EnglishWord("photon"),
        EnglishWord("photons"),
        EnglishWord("photo"),
        ChineseWord("跑通"),
    };

    const std::vector<std::string> exact{"photon", "photons"};
    const std::vector<std::string> prefix{"photon", "photons", "photo"};
    const std::vector<std::string> none;
    REQUIRE(Words(FanyImeIpc::PreservedMixedEnglish(current, "photon")) == exact);
    REQUIRE(Words(FanyImeIpc::PreservedMixedEnglish(current, "pho")) == prefix);
    REQUIRE(FanyImeIpc::PreservedMixedEnglish(current, "photonz").empty());
    REQUIRE(FanyImeIpc::PreservedMixedEnglish(current, "").empty());
}
