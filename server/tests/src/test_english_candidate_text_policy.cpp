#include "english/english_candidate_text_policy.h"
#include "tests/includes/test_framework.h"

using EnglishIme::MergeSpellingCandidates;
using EnglishIme::ShouldAppendCommitSpace;
using EnglishIme::SpellingCandidate;
using EnglishIme::WithTypedCase;

TEST_CASE(english_typed_case_keeps_dictionary_spelling_for_lowercase_input)
{
    REQUIRE(WithTypedCase("tes", "test") == "test");
    REQUIRE(WithTypedCase("wiki", "Wikipedia") == "Wikipedia");
    REQUIRE(WithTypedCase("iph", "iPhone") == "iPhone");
}

TEST_CASE(english_typed_case_keeps_the_typed_prefix)
{
    REQUIRE(WithTypedCase("Tes", "test") == "Test");
    REQUIRE(WithTypedCase("McD", "mcdonald's") == "McDonald's");
    REQUIRE(WithTypedCase("I", "i") == "I");
    // 敲了一个大写字母就只是首字母大写，不是全大写。
    REQUIRE(WithTypedCase("A", "apple") == "Apple");
}

TEST_CASE(english_typed_case_uppercases_words_typed_in_capitals)
{
    REQUIRE(WithTypedCase("NA", "nasa") == "NASA");
    REQUIRE(WithTypedCase("HTT", "http") == "HTTP");
    REQUIRE(WithTypedCase("NASA", "nasa") == "NASA");
}

TEST_CASE(english_typed_case_capitalizes_corrections_that_do_not_share_the_prefix)
{
    REQUIRE(WithTypedCase("Kerrage", "courage") == "Courage");
    REQUIRE(WithTypedCase("kerrage", "courage") == "courage");
    REQUIRE(WithTypedCase("Teh", "the") == "The");
    REQUIRE(WithTypedCase("KERRAGE", "courage") == "COURAGE");
}

TEST_CASE(english_typed_case_maps_letters_onto_phrases)
{
    REQUIRE(WithTypedCase("accordingt", "according to") == "according to");
    REQUIRE(WithTypedCase("Accordingt", "according to") == "According to");
    REQUIRE(WithTypedCase("AccordingTo", "according to") == "According To");
    REQUIRE(WithTypedCase("ALOT", "a lot") == "A LOT");
    REQUIRE(WithTypedCase("WellK", "well-known") == "Well-Known");
    REQUIRE(WithTypedCase("Babys", "a baby's dozen") == "A baby's dozen");
}

TEST_CASE(english_phrases_follow_exact_words_and_precede_the_other_phrases)
{
    using EnglishIme::MergeEnglishPhrases;
    const auto word = [](const char *key) { return WordItem(key, key, 0, CandidateSource::EnglishDictionary); };
    const auto phrase = [](const char *key, const char *display) {
        WordItem item(key, display, 0, CandidateSource::Generated);
        item.english_phrase = true;
        return item;
    };
    const auto merged = MergeEnglishPhrases("alot", {word("alot"), word("alots")},
                                            {phrase("alotof", "a lot of"), phrase("alot", "a lot")});
    REQUIRE(merged.size() == 4);
    REQUIRE(merged[0].word == "alot");
    REQUIRE(merged[1].word == "a lot");
    REQUIRE(merged[2].word == "alots");
    REQUIRE(merged[3].word == "a lot of");

    const auto only_phrases = MergeEnglishPhrases("accordingt", {}, {phrase("accordingto", "according to")});
    REQUIRE(only_phrases.size() == 1);
    REQUIRE(only_phrases[0].english_phrase);
    REQUIRE(MergeEnglishPhrases("air", {word("air")}, {}).size() == 1);
    REQUIRE(EnglishIme::PhraseDetailKey("A Level") == "a level");
}

TEST_CASE(english_commit_space_skips_digits_apostrophes_and_whitespace)
{
    REQUIRE(ShouldAppendCommitSpace("hello"));
    REQUIRE(ShouldAppendCommitSpace("McDonald's"));
    REQUIRE(!ShouldAppendCommitSpace(""));
    REQUIRE(!ShouldAppendCommitSpace("3rd"));
    REQUIRE(!ShouldAppendCommitSpace("rock'"));
    REQUIRE(!ShouldAppendCommitSpace("hello "));
}

TEST_CASE(english_spelling_merge_puts_phonetic_matches_first_and_deduplicates)
{
    const std::vector<SpellingCandidate> phonetic{
        {"courage", "courage"}, {"carriage", "carriage"}, {"kerrage", "kerrage"}};
    const std::vector<std::string> system{"Courage", "garage", "kerrage", "carnage"};
    const auto merged = MergeSpellingCandidates("kerrage", phonetic, system, 4, 8);
    REQUIRE(merged.size() == 4);
    REQUIRE(merged[0].display == "courage");
    REQUIRE(merged[1].display == "carriage");
    REQUIRE(merged[2].display == "garage");
    REQUIRE(merged[3].display == "carnage");
}

TEST_CASE(english_detail_key_uses_the_dictionary_key_or_a_plain_lowercased_word)
{
    REQUIRE(EnglishIme::DetailKey(true, "wikipedia", "Wikipedia") == "wikipedia");
    REQUIRE(EnglishIme::DetailKey(false, "", "Courage") == "courage");
    REQUIRE(EnglishIme::DetailKey(false, "", "rock'n'roll") == "rock'n'roll");
    REQUIRE(EnglishIme::DetailKey(false, "", "").empty());
    REQUIRE(EnglishIme::DetailKey(false, "", "3rd").empty());
    REQUIRE(EnglishIme::DetailKey(false, "", "two words").empty());
}

TEST_CASE(english_detail_prefers_the_dr_dict_brief_over_the_local_gloss)
{
    REQUIRE(EnglishIme::FormatDetail("ˈælgərɪðəm", "n. 算法", "") == "/ˈælgərɪðəm/  n. 算法");
    REQUIRE(EnglishIme::FormatDetail("ˈælgərɪðəm", "n. 算法", "n. 算法；运算法则") ==
            "/ˈælgərɪðəm/  n. 算法；运算法则");
    REQUIRE(EnglishIme::FormatDetail("", "n. 算法", "") == "n. 算法");
    REQUIRE(EnglishIme::FormatDetail("ˈælgərɪðəm", "", "") == "/ˈælgərɪðəm/");
    REQUIRE(EnglishIme::FormatDetail("", "", "").empty());
}

TEST_CASE(english_spelling_merge_caps_each_source_and_the_total)
{
    const std::vector<SpellingCandidate> phonetic{{"a1", "a1"}, {"a2", "a2"}, {"a3", "a3"}};
    const std::vector<std::string> system{"b1", "b2", "b3"};
    const auto merged = MergeSpellingCandidates("x", phonetic, system, 2, 3);
    REQUIRE(merged.size() == 3);
    REQUIRE(merged[0].word == "a1");
    REQUIRE(merged[1].word == "a2");
    REQUIRE(merged[2].word == "b1");
}
