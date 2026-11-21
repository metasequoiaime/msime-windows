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

TEST_CASE(mixed_english_preservation_matches_lowercase_key_not_display)
{
    // 英文库的行：pinyin 是小写查询键，word 是显示形式。
    WordItem uefi = EnglishWord("UEFI");
    uefi.pinyin = "uefi";
    WordItem python = EnglishWord("Python");
    python.pinyin = "python";
    const std::vector<WordItem> current{ChineseWord("额"), uefi, python};

    const std::vector<std::string> ue{"UEFI"};
    REQUIRE(Words(FanyImeIpc::PreservedMixedEnglish(current, "ue")) == ue);
    REQUIRE(Words(FanyImeIpc::PreservedMixedEnglish(current, "uefi")) == ue);
    const std::vector<std::string> py{"Python"};
    REQUIRE(Words(FanyImeIpc::PreservedMixedEnglish(current, "py")) == py);
    REQUIRE(FanyImeIpc::PreservedMixedEnglish(current, "uefix").empty());
}

TEST_CASE(mixed_rows_preservation_keeps_rows_merged_for_same_input)
{
    WordItem degree = ChineseWord("°");
    degree.source = CandidateSource::Symbol;
    WordItem emoji = ChineseWord("🙂");
    emoji.source = CandidateSource::Emoji;
    const std::vector<WordItem> current{ChineseWord("度"), EnglishWord("du"), degree, emoji};
    const auto keep_all = [](CandidateSource) { return true; };
    const auto symbols_only = [](CandidateSource source) { return source == CandidateSource::Symbol; };

    const std::vector<std::string> both{"°", "🙂"};
    const std::vector<std::string> symbol{"°"};
    REQUIRE(Words(FanyImeIpc::PreservedMixedRows(current, "du", "du", keep_all)) == both);
    REQUIRE(Words(FanyImeIpc::PreservedMixedRows(current, "du", "du", symbols_only)) == symbol);
    // 行是为别的输入查出来的：一行都不带。
    REQUIRE(FanyImeIpc::PreservedMixedRows(current, "d", "du", keep_all).empty());
    REQUIRE(FanyImeIpc::PreservedMixedRows(current, "", "", keep_all).empty());
}
