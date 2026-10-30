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

// 切换字形后要不要换写法：只有中文方案下、不是 U 模式拆字列表、也不在英文状态时，列表才随字形变。
TEST_CASE(candidate_text_policy_rebuilds_only_lists_that_follow_the_character_set)
{
    constexpr bool kEnglishInputMode = true;
    constexpr bool kChineseInputMode = false;
    REQUIRE(FanyImeIpc::CandidateListFollowsCharacterSet(kChineseScheme, kOtherCandidateList, kChineseInputMode));
    for (const bool japanese : {kJapaneseScheme, kChineseScheme})
    {
        for (const bool radical : {kRadicalCandidateList, kOtherCandidateList})
        {
            for (const bool english : {kEnglishInputMode, kChineseInputMode})
            {
                if (!japanese && !radical && !english)
                    continue;
                REQUIRE(!FanyImeIpc::CandidateListFollowsCharacterSet(japanese, radical, english));
            }
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

namespace
{
const std::string kQianTraditional = "\xE4\xB9\xBE"; // 乾，「干」的另一种写法
const std::string kFaHair = "\xE9\xAB\xAE";          // 髮，「发」的另一种写法
constexpr bool kFirstSpellings = true;

FanyImeIpc::ChosenText Chosen(const std::string &word, const std::string &output_text, bool first_spellings)
{
    return {word, output_text, first_spellings};
}

std::string Commit(const FanyImeIpc::ChosenText &prefix, const FanyImeIpc::ChosenText &candidate, bool radical)
{
    return FanyImeIpc::CommitTextAfterCreatingWordPrefix(prefix, candidate, radical, FakeToTraditional);
}
} // namespace

// 两段都选了第一写法：按整串取第一写法，词组规则跨过段的交界（「干」「燥」→「乾燥」，不是「幹燥」）。
TEST_CASE(candidate_text_policy_commits_the_whole_word_spelling_when_every_segment_took_its_first_spelling)
{
    REQUIRE_EQ(Commit(Chosen(kGan, kGanTraditional, kFirstSpellings), Chosen(kZao, kZao, kFirstSpellings),
                      kOtherCandidateList),
               kQianPhrase);
}

// 前缀里有一段选了非第一写法：上屏屏幕上显示的拼接。
TEST_CASE(candidate_text_policy_commits_the_shown_text_when_a_prefix_segment_took_another_spelling)
{
    REQUIRE_EQ(Commit(Chosen(kGan, kQianTraditional, !kFirstSpellings), Chosen(kFa, kFaTraditional, kFirstSpellings),
                      kOtherCandidateList),
               kQianTraditional + kFaTraditional);
}

// U 模式拆字结果：前缀取前缀文字，候选原样。
TEST_CASE(candidate_text_policy_converts_only_prefix_in_radical_list)
{
    REQUIRE_EQ(Commit(Chosen(kFa, kFaTraditional, kFirstSpellings), Chosen(kGan, kGanTraditional, kFirstSpellings),
                      kRadicalCandidateList),
               kFaTraditional + kGan);
    // 前缀整串取第一写法：「干燥」作前缀时是「乾燥」。
    REQUIRE_EQ(Commit(Chosen(kGan + kZao, kGanTraditional + kZao, kFirstSpellings),
                      Chosen(kFa, kFaTraditional, kFirstSpellings), kRadicalCandidateList),
               kQianPhrase + kFa);
    REQUIRE_EQ(Commit(Chosen(kFa, kFaHair, !kFirstSpellings), Chosen(kGan, kGanTraditional, kFirstSpellings),
                      kRadicalCandidateList),
               kFaHair + kGan);
}

TEST_CASE(candidate_text_policy_commit_text_without_prefix)
{
    const auto empty = Chosen("", "", kFirstSpellings);
    REQUIRE_EQ(Commit(empty, Chosen(kGan, kGanTraditional, kFirstSpellings), kRadicalCandidateList), kGan);
    REQUIRE_EQ(Commit(empty, Chosen(kGan, kGanTraditional, kFirstSpellings), kOtherCandidateList), kGanTraditional);
    REQUIRE_EQ(Commit(empty, Chosen(kGan, kQianTraditional, !kFirstSpellings), kOtherCandidateList), kQianTraditional);
    REQUIRE_EQ(Commit(empty, Chosen(kGan + kZao, kGanTraditional + kZao, kFirstSpellings), kOtherCandidateList),
               kQianPhrase);
}

// 简体模式：取第一写法就是原文，各段显示文字也是原文，结果与逐段拼接简体底词逐字相同。
TEST_CASE(candidate_text_policy_simplified_mode_commits_the_simplified_words)
{
    const auto unchanged = [](const std::string &text) { return text; };
    for (const bool radical : {kRadicalCandidateList, kOtherCandidateList})
        for (const bool prefix_first : {true, false})
            for (const bool candidate_first : {true, false})
                for (const std::string &prefix : {std::string(), kGan, kGan + kZao})
                {
                    const auto committed = FanyImeIpc::CommitTextAfterCreatingWordPrefix(
                        Chosen(prefix, prefix, prefix_first), Chosen(kFa, kFa, candidate_first), radical, unchanged);
                    REQUIRE_EQ(committed, prefix + kFa);
                    REQUIRE_EQ(FanyImeIpc::ChosenTextForOutput(Chosen(prefix, prefix, prefix_first), unchanged),
                               prefix);
                }
}

namespace
{
const std::string kTaiwanSimplified = "台湾";
const std::string kTaiwanStandard = "臺灣"; // s2t，未展开的项（如快捷短语）显示的就是它
const std::string kTaiwanHongKong = "台灣"; // 香港用户的第一写法
const std::string kZhe = "着";
const std::string kZheTaiwan = "著";

// 假的「整词第一写法」：「台湾」取香港写法「台灣」；其余照 FakeToTraditional，再把「着」换成「著」。
std::string FakeFirstSpelling(const std::string &text)
{
    if (text == kTaiwanSimplified)
        return kTaiwanHongKong;
    std::string result = FakeToTraditional(text);
    for (size_t offset = result.find(kZhe); offset != std::string::npos; offset = result.find(kZhe, offset))
        result.replace(offset, kZhe.size(), kZheTaiwan);
    return result;
}

FanyImeIpc::ChosenText Shown(const std::string &word, const std::string &output_text)
{
    return FanyImeIpc::ChosenTextOf(word, output_text, FakeFirstSpelling);
}
} // namespace

// R1：没有展开的项显示 s2t 结果，与地区第一写法不同时不算第一写法，上屏照显示。
TEST_CASE(candidate_text_policy_an_unexpanded_candidate_commits_what_it_shows)
{
    const auto phrase = Shown(kTaiwanSimplified, kTaiwanStandard);
    REQUIRE(!phrase.first_spellings);
    REQUIRE_EQ(
        FanyImeIpc::CommitTextAfterCreatingWordPrefix(Shown("", ""), phrase, kOtherCandidateList, FakeFirstSpelling),
        kTaiwanStandard);
    // 显示与第一写法相同时算第一写法，上屏结果也相同。
    const auto same = Shown(kTaiwanSimplified, kTaiwanHongKong);
    REQUIRE(same.first_spellings);
    REQUIRE_EQ(
        FanyImeIpc::CommitTextAfterCreatingWordPrefix(Shown("", ""), same, kOtherCandidateList, FakeFirstSpelling),
        kTaiwanHongKong);
}

// R2：去重后编号为 0、但显示的不是底词第一写法的项（「着」组只剩「着」）不算第一写法，走显示拼接。
TEST_CASE(candidate_text_policy_a_renumbered_spelling_is_not_the_first_spelling)
{
    const auto zhe = Shown(kZhe, kZhe);
    REQUIRE(!zhe.first_spellings);
    REQUIRE_EQ(FanyImeIpc::CommitTextAfterCreatingWordPrefix(Shown(kGan, kGanTraditional), zhe, kOtherCandidateList,
                                                             FakeFirstSpelling),
               kGanTraditional + kZhe);
    REQUIRE_EQ(
        FanyImeIpc::CommitTextAfterCreatingWordPrefix(Shown("", ""), zhe, kOtherCandidateList, FakeFirstSpelling),
        kZhe);
}
