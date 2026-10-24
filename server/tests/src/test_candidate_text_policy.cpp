#include "ipc/candidate_text_policy.h"
#include "tests/includes/test_framework.h"

TEST_CASE(candidate_text_policy_extracts_first_and_last_han_character)
{
    const std::string candidate =
        "\xE4\xB8\xAD\xE5\x8D\x8E\xE4\xBA\xBA\xE6\xB0\x91\xE5\x85\xB1\xE5\x92\x8C\xE5\x9B\xBD";
    const auto first = FanyImeIpc::ExtractHanCharacter(candidate, FanyImeIpc::HanCharacterEdge::First);
    const auto last = FanyImeIpc::ExtractHanCharacter(candidate, FanyImeIpc::HanCharacterEdge::Last);
    REQUIRE(first.has_value());
    REQUIRE(last.has_value());
    REQUIRE_EQ(*first, std::string("\xE4\xB8\xAD"));
    REQUIRE_EQ(*last, std::string("\xE5\x9B\xBD"));
}

TEST_CASE(candidate_text_policy_skips_non_han_characters)
{
    const std::string candidate = "C\xE8\xAF\xAD\xE8\xA8\x80 2";
    const auto first = FanyImeIpc::ExtractHanCharacter(candidate, FanyImeIpc::HanCharacterEdge::First);
    const auto last = FanyImeIpc::ExtractHanCharacter(candidate, FanyImeIpc::HanCharacterEdge::Last);
    REQUIRE(first.has_value());
    REQUIRE(last.has_value());
    REQUIRE_EQ(*first, std::string("\xE8\xAF\xAD"));
    REQUIRE_EQ(*last, std::string("\xE8\xA8\x80"));
    REQUIRE(!FanyImeIpc::ExtractHanCharacter("GitHub", FanyImeIpc::HanCharacterEdge::First).has_value());
}

TEST_CASE(candidate_text_policy_supports_non_bmp_han_characters)
{
    const std::string candidate = "\xF0\xA0\x80\x80\xE6\x96\xB9\xE6\xA1\x88\xF0\xA0\xAE\xB7";
    const auto first = FanyImeIpc::ExtractHanCharacter(candidate, FanyImeIpc::HanCharacterEdge::First);
    const auto last = FanyImeIpc::ExtractHanCharacter(candidate, FanyImeIpc::HanCharacterEdge::Last);
    REQUIRE(first.has_value());
    REQUIRE(last.has_value());
    REQUIRE_EQ(*first, std::string("\xF0\xA0\x80\x80"));
    REQUIRE_EQ(*last, std::string("\xF0\xA0\xAE\xB7"));
}

TEST_CASE(candidate_text_policy_uses_the_highlighted_candidate)
{
    const std::vector<std::wstring> page_words = {L"first", L"highlighted", L"third"};
    REQUIRE_EQ(FanyImeIpc::HighlightedCandidateText(page_words, 1), std::wstring(L"highlighted"));
    REQUIRE(FanyImeIpc::HighlightedCandidateText(page_words, -1).empty());
    REQUIRE(FanyImeIpc::HighlightedCandidateText(page_words, 3).empty());
}

namespace
{
constexpr bool kTraditional = true;
constexpr bool kSimplified = false;
constexpr bool kJapaneseScheme = true;
constexpr bool kChineseScheme = false;
constexpr bool kRadicalCandidateList = true;
constexpr bool kOtherCandidateList = false;
} // namespace

TEST_CASE(candidate_text_policy_keeps_radical_candidates_in_traditional_mode)
{
    REQUIRE(!FanyImeIpc::ShouldConvertCandidateTextToTraditional(kTraditional, kChineseScheme, kRadicalCandidateList));
}

TEST_CASE(candidate_text_policy_converts_other_candidates_in_traditional_mode)
{
    REQUIRE(FanyImeIpc::ShouldConvertCandidateTextToTraditional(kTraditional, kChineseScheme, kOtherCandidateList));
}

TEST_CASE(candidate_text_policy_never_converts_in_simplified_mode)
{
    REQUIRE(!FanyImeIpc::ShouldConvertCandidateTextToTraditional(kSimplified, kChineseScheme, kOtherCandidateList));
    REQUIRE(!FanyImeIpc::ShouldConvertCandidateTextToTraditional(kSimplified, kChineseScheme, kRadicalCandidateList));
}

TEST_CASE(candidate_text_policy_never_converts_japanese_candidates)
{
    for (const bool traditional : {kTraditional, kSimplified})
    {
        for (const bool radical : {kRadicalCandidateList, kOtherCandidateList})
        {
            REQUIRE(!FanyImeIpc::ShouldConvertCandidateTextToTraditional(traditional, kJapaneseScheme, radical));
        }
    }
}

namespace
{
constexpr bool kRadicalModeTriggered = true;
constexpr bool kTranslationPageActive = true;

const std::string kGan = "\xE5\xB9\xB2";                    // 干
const std::string kZao = "\xE7\x87\xA5";                    // 燥
const std::string kQianPhrase = "\xE4\xB9\xBE\xE7\x87\xA5"; // 乾燥
const std::string kGanTraditional = "\xE5\xB9\xB9";         // 幹
const std::string kFa = "\xE5\x8F\x91";                     // 发
const std::string kFaTraditional = "\xE7\x99\xBC";          // 發

// 假转换器：「干燥」按词组整体转成「乾燥」，其余逐字把「干」转成「幹」、「发」转成「發」。
// 整串转与分段转得到的结果不同，测试才分得出拼接规则走的是哪一种。
std::string FakeToTraditional(const std::string &text)
{
    if (text == kGan + kZao)
        return kQianPhrase;
    std::string result;
    for (size_t offset = 0; offset < text.size();)
    {
        if (text.compare(offset, kGan.size(), kGan) == 0)
        {
            result += kGanTraditional;
            offset += kGan.size();
        }
        else if (text.compare(offset, kFa.size(), kFa) == 0)
        {
            result += kFaTraditional;
            offset += kFa.size();
        }
        else
        {
            result += text[offset++];
        }
    }
    return result;
}
} // namespace

TEST_CASE(candidate_text_policy_recognizes_radical_candidate_list)
{
    REQUIRE(FanyImeIpc::IsRadicalCandidateList(kRadicalModeTriggered, !kTranslationPageActive, "Ukou"));
}

TEST_CASE(candidate_text_policy_radical_candidate_list_needs_every_condition)
{
    REQUIRE(!FanyImeIpc::IsRadicalCandidateList(!kRadicalModeTriggered, !kTranslationPageActive, "Ukou"));
    REQUIRE(!FanyImeIpc::IsRadicalCandidateList(kRadicalModeTriggered, kTranslationPageActive, "Ukou"));
    REQUIRE(!FanyImeIpc::IsRadicalCandidateList(kRadicalModeTriggered, !kTranslationPageActive, "Kkou"));
}

TEST_CASE(candidate_text_policy_radical_candidate_list_rejects_lowercase_and_empty_input)
{
    REQUIRE(!FanyImeIpc::IsRadicalCandidateList(kRadicalModeTriggered, !kTranslationPageActive, "ukou"));
    REQUIRE(!FanyImeIpc::IsRadicalCandidateList(kRadicalModeTriggered, !kTranslationPageActive, ""));
}

TEST_CASE(candidate_text_policy_radical_composition_needs_u_at_the_start)
{
    // 打 Ukou 后把光标移到开头再按 a，串里仍有 U，但已经不在开头。
    REQUIRE(!FanyImeIpc::IsRadicalComposition(kRadicalModeTriggered, "aUkou"));
}

TEST_CASE(candidate_text_policy_converts_prefix_and_candidate_together_outside_radical_list)
{
    const std::string committed =
        FanyImeIpc::CommitTextAfterCreatingWordPrefix(kGan, kZao, kOtherCandidateList, FakeToTraditional);
    REQUIRE_EQ(committed, kQianPhrase);
}

TEST_CASE(candidate_text_policy_converts_only_prefix_in_radical_list)
{
    const std::string committed =
        FanyImeIpc::CommitTextAfterCreatingWordPrefix(kFa, kGan, kRadicalCandidateList, FakeToTraditional);
    REQUIRE_EQ(committed, kFaTraditional + kGan);
}

TEST_CASE(candidate_text_policy_commit_text_without_prefix)
{
    REQUIRE_EQ(FanyImeIpc::CommitTextAfterCreatingWordPrefix("", kGan, kRadicalCandidateList, FakeToTraditional), kGan);
    REQUIRE_EQ(FanyImeIpc::CommitTextAfterCreatingWordPrefix("", kGan, kOtherCandidateList, FakeToTraditional),
               kGanTraditional);
}
