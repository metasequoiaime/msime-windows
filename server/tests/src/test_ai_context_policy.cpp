#include "ipc/ai_context_policy.h"
#include "tests/includes/test_framework.h"

#include <cstddef>
#include <map>
#include <string>
#include <utility>

TEST_CASE(ai_context_drops_credential_like_ascii_runs)
{
    using FanyImeIpc::StripCredentialLikeText;
    REQUIRE_EQ(StripCredentialLikeText("密码是Abc12345然后"), std::string("密码是然后"));
    REQUIRE_EQ(StripCredentialLikeText("key sk-proj-abcdEFGH1234 已填"), std::string("key  已填"));
    REQUIRE_EQ(StripCredentialLikeText("p@ssword"), std::string());
    REQUIRE_EQ(StripCredentialLikeText("2026-10-11"), std::string());
}

TEST_CASE(ai_context_keeps_plain_words_numbers_and_chinese)
{
    using FanyImeIpc::StripCredentialLikeText;
    REQUIRE_EQ(StripCredentialLikeText("明天3点开会"), std::string("明天3点开会"));
    REQUIRE_EQ(StripCredentialLikeText("用Python写个脚本"), std::string("用Python写个脚本"));
    REQUIRE_EQ(StripCredentialLikeText("international"), std::string("international"));
    REQUIRE_EQ(StripCredentialLikeText("13800138000"), std::string("13800138000"));
    // Short mixed runs are below the length threshold.
    REQUIRE_EQ(StripCredentialLikeText("A4纸和e.g."), std::string("A4纸和e.g."));
    REQUIRE_EQ(StripCredentialLikeText(""), std::string());
}

TEST_CASE(ai_context_credential_threshold_needs_length_and_two_classes)
{
    using FanyImeIpc::IsCredentialLikeAsciiRun;
    REQUIRE(!IsCredentialLikeAsciiRun("abc1234"));
    REQUIRE(IsCredentialLikeAsciiRun("abcd1234"));
    REQUIRE(!IsCredentialLikeAsciiRun("abcdefgh"));
    REQUIRE(!IsCredentialLikeAsciiRun("12345678"));
    REQUIRE(IsCredentialLikeAsciiRun("abcdefg!"));
    REQUIRE(IsCredentialLikeAsciiRun("1234-567"));
}

namespace
{
std::string Repeat(const std::string &unit, std::size_t count)
{
    std::string result;
    for (std::size_t i = 0; i < count; ++i)
        result += unit;
    return result;
}

// 按查表转换的替身：表里有的整串替换，没有的原样返回。
struct TableConverter
{
    std::map<std::string, std::string> table;

    std::string operator()(const std::string &text) const
    {
        const auto found = table.find(text);
        return found == table.end() ? text : found->second;
    }
};

// 下面几条用例共用的一组转换结果，取自 OpenCC t2s、s2t 对这些词的实际输出。
const TableConverter kToSimplified{{{"臺灣", "台湾"}, {"台灣", "台湾"}, {"因為", "因为"}, {"萧乾", "萧干"}}};
const TableConverter kToTraditional{
    {{"台湾", "臺灣"}, {"台灣", "臺灣"}, {"因为", "因爲"}, {"萧乾", "蕭乾"}, {"萧干", "蕭幹"}}};
} // namespace

TEST_CASE(ai_context_trim_leaves_contexts_up_to_1024_bytes_alone)
{
    using FanyImeIpc::TrimAiContextFront;
    REQUIRE_EQ(FanyImeIpc::kAiContextMaxBytes, std::size_t{1024});
    REQUIRE_EQ(TrimAiContextFront(""), std::string());
    const std::string exactly_full = Repeat("中", 341) + "x"; // 1024 字节
    REQUIRE_EQ(TrimAiContextFront(exactly_full), exactly_full);
}

TEST_CASE(ai_context_trim_cuts_front_and_moves_cut_to_next_utf8_char)
{
    using FanyImeIpc::TrimAiContextFront;
    // 1025 字节：裁点落在第一个「中」的续字节上，往后挪到下一个字符开头。
    REQUIRE_EQ(TrimAiContextFront(Repeat("中", 341) + "xy"), Repeat("中", 340) + "xy");
    // 四字节字符同理。
    REQUIRE_EQ(TrimAiContextFront(Repeat("😀", 256) + "a"), Repeat("😀", 255) + "a");
    // 裁点正好在字符开头时只裁那么多。
    REQUIRE_EQ(TrimAiContextFront(std::string(1024, 'a') + "中"), std::string(1021, 'a') + "中");
    REQUIRE_EQ(TrimAiContextFront(std::string(1029, 'a')), std::string(1024, 'a'));
}

TEST_CASE(ai_candidate_text_is_unchanged_in_simplified_mode)
{
    using FanyImeIpc::SimplifiedAiCandidateText;
    // 同一组输入在繁体模式下会转成简体（见下面几条），简体模式下原样返回。
    REQUIRE_EQ(SimplifiedAiCandidateText("臺灣", "simplified", kToSimplified, kToTraditional), std::string("臺灣"));
    REQUIRE_EQ(SimplifiedAiCandidateText("台灣", "simplified", kToSimplified, kToTraditional), std::string("台灣"));
    REQUIRE_EQ(SimplifiedAiCandidateText("因為", "simplified", kToSimplified, kToTraditional), std::string("因為"));
}

TEST_CASE(ai_candidate_text_simplifies_traditional_text_even_when_display_changes)
{
    using FanyImeIpc::SimplifiedAiCandidateText;
    // s2t(因為) == 因為：原文本来就是繁体，转回简体；s2t(因为) == 因爲 != 因為，之后的繁体显示会变。
    REQUIRE_EQ(SimplifiedAiCandidateText("因為", "traditional", kToSimplified, kToTraditional), std::string("因为"));
}

TEST_CASE(ai_candidate_text_simplifies_when_round_trip_display_matches)
{
    using FanyImeIpc::SimplifiedAiCandidateText;
    // 「台灣」繁简混写：s2t(台灣) == 臺灣 != 台灣，但 s2t(t2s(台灣)) == s2t(台湾) == 臺灣，显示不变。
    REQUIRE_EQ(SimplifiedAiCandidateText("台灣", "traditional", kToSimplified, kToTraditional), std::string("台湾"));
}

TEST_CASE(ai_candidate_text_keeps_simplified_names_that_t2s_would_rewrite)
{
    using FanyImeIpc::SimplifiedAiCandidateText;
    // t2s(萧乾) == 萧干，s2t(萧干) == 蕭幹 != s2t(萧乾) == 蕭乾：转了会改变繁体显示，存原文。
    REQUIRE_EQ(SimplifiedAiCandidateText("萧乾", "traditional", kToSimplified, kToTraditional), std::string("萧乾"));
}
