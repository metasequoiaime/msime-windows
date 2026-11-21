#include "engine/user_dictionary/user_dictionary_journal.h"
#include "ipc/candidate_selection_policy.h"
#include "ipc/traditional_variant_choice_journal.h"
#include "ipc/traditional_variant_choice_policy.h"
#include "ipc/traditional_variant_expansion_policy.h"
#include "tests/includes/table_traditional_spelling_index.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using FanyImeIpc::ComposedWordSegments;
using FanyImeIpc::SameCharacterSetAfterSegment;
using FanyImeIpc::VariantChoiceAction;
using FanyImeIpc::VariantChoiceChange;
using FanyImeIpc::VariantChoiceForComposedWord;
using FanyImeIpc::VariantChoiceForSelection;
using FanyImeIpc::VariantChoiceScope;
using Spellings = std::vector<std::string>;

namespace
{
constexpr VariantChoiceScope kTraditional{/*traditional_character_set=*/true, /*japanese_scheme=*/false,
                                          /*radical_candidate_list=*/false};
constexpr VariantChoiceScope kSimplified{false, false, false};
constexpr VariantChoiceScope kJapanese{true, true, false};
constexpr VariantChoiceScope kRadical{true, false, true};

constexpr ComposedWordSegments kSameSetSomeNotFirst{/*same_character_set=*/true, /*all_first_spellings=*/false};
constexpr ComposedWordSegments kSameSetAllFirst{/*same_character_set=*/true, /*all_first_spellings=*/true};
constexpr ComposedWordSegments kMixedSetSomeNotFirst{/*same_character_set=*/false, /*all_first_spellings=*/false};
constexpr ComposedWordSegments kMixedSetAllFirst{/*same_character_set=*/false, /*all_first_spellings=*/true};

const std::string kTraditionalSet = "traditional";
const std::string kSimplifiedSet = "simplified";

// 展开后的一项：底词 word，这一项的写法 spelling，组内序号 rank。
WordItem Variant(const std::string &word, const std::string &spelling, int rank,
                 const std::string &canonical = "tai'wan")
{
    WordItem item("tai'wan", word, 100, CandidateSource::Database, canonical);
    item.output_text = spelling;
    item.variant_rank = rank;
    return item;
}

VariantChoiceChange Remember(const std::string &key, const std::string &word, const std::string &spelling)
{
    return {VariantChoiceAction::Remember, key, word, spelling};
}

VariantChoiceChange Forget(const std::string &key, const std::string &word)
{
    return {VariantChoiceAction::Forget, key, word, {}};
}

void RequireChange(const std::optional<VariantChoiceChange> &actual, const VariantChoiceChange &expected)
{
    REQUIRE(actual.has_value());
    REQUIRE(actual->action == expected.action);
    REQUIRE_EQ(actual->key, expected.key);
    REQUIRE_EQ(actual->word, expected.word);
    REQUIRE_EQ(actual->spelling, expected.spelling);
}
} // namespace

// ---- 普通选词、置顶、固定：选中项的写法要不要记 ----

// 简体模式、日语方案、U 模式拆字列表里，同样的选择一律不碰记录（含改回默认时的删除）。
TEST_CASE(variant_choice_selection_does_nothing_outside_traditional_candidates)
{
    for (const auto &scope : {kSimplified, kJapanese, kRadical})
    {
        REQUIRE(!VariantChoiceForSelection(scope, Variant("台湾", "台灣", 1), "tai'wan", "臺灣").has_value());
        REQUIRE(!VariantChoiceForSelection(scope, Variant("台湾", "臺灣", 1), "tai'wan", "臺灣").has_value());
    }
    WordItem japanese = Variant("台湾", "台灣", 1);
    japanese.scheme = SchemeType::JapaneseRomaji;
    REQUIRE(!VariantChoiceForSelection(kTraditional, japanese, "tai'wan", "臺灣").has_value());
}

// 选中的是这组排第一的写法（序号 0）时不改记录：它不论是记住的写法还是地区默认写法，都已经排在第一。
TEST_CASE(variant_choice_selection_ignores_the_item_that_already_leads_its_group)
{
    REQUIRE(!VariantChoiceForSelection(kTraditional, Variant("台湾", "台灣", 0), "tai'wan", "臺灣").has_value());
    REQUIRE(!VariantChoiceForSelection(kTraditional, Variant("台湾", "臺灣", 0), "tai'wan", "臺灣").has_value());
}

// 组内序号大于 0 的项：不是地区默认写法时记下，是地区默认写法时删记录。
TEST_CASE(variant_choice_selection_remembers_or_forgets_an_item_behind_the_first)
{
    RequireChange(VariantChoiceForSelection(kTraditional, Variant("台湾", "台灣", 1), "tai'wan", "臺灣"),
                  Remember("tai'wan", "台湾", "台灣"));
    RequireChange(VariantChoiceForSelection(kTraditional, Variant("台湾", "臺灣", 1), "tai'wan", "臺灣"),
                  Forget("tai'wan", "台湾"));
}

// 键、底词、上屏串缺一个就不改记录。
TEST_CASE(variant_choice_selection_ignores_a_missing_key_word_or_spelling)
{
    REQUIRE(!VariantChoiceForSelection(kTraditional, Variant("台湾", "台灣", 1), "", "臺灣").has_value());
    REQUIRE(!VariantChoiceForSelection(kTraditional, Variant("", "台灣", 1), "tai'wan", "臺灣").has_value());
    REQUIRE(!VariantChoiceForSelection(kTraditional, Variant("台湾", "", 1), "tai'wan", "臺灣").has_value());
}

// 调用方靠 SelectionCanChangeVariantChoice 先跳过地区默认写法的计算，所以它为假的项，
// 不论地区默认写法是什么，VariantChoiceForSelection 都必须不改记录；为真的只有组内排在后面的拼音项。
TEST_CASE(variant_choice_selection_precheck_rules_out_every_item_the_policy_ignores)
{
    WordItem japanese = Variant("台湾", "台灣", 1);
    japanese.scheme = SchemeType::JapaneseRomaji;
    const std::vector<WordItem> ruled_out{Variant("台湾", "台灣", 0), Variant("台湾", "臺灣", 0), japanese,
                                          Variant("", "台灣", 1), Variant("台湾", "", 1)};
    for (const auto &item : ruled_out)
    {
        REQUIRE(!FanyImeIpc::SelectionCanChangeVariantChoice(item));
        for (const std::string default_spelling : {"臺灣", "台灣", ""})
            REQUIRE(!VariantChoiceForSelection(kTraditional, item, "tai'wan", default_spelling).has_value());
    }
    REQUIRE(FanyImeIpc::SelectionCanChangeVariantChoice(Variant("台湾", "台灣", 1)));
    REQUIRE(FanyImeIpc::SelectionCanChangeVariantChoice(Variant("台湾", "臺灣", 2)));
}

// ---- 改动的字段齐不齐 ----

TEST_CASE(variant_choice_change_is_complete_only_with_every_field_it_needs)
{
    REQUIRE(FanyImeIpc::IsCompleteVariantChoiceChange(Remember("tai'wan", "台湾", "台灣")));
    REQUIRE(FanyImeIpc::IsCompleteVariantChoiceChange(Forget("tai'wan", "台湾")));
    REQUIRE(!FanyImeIpc::IsCompleteVariantChoiceChange(Remember("", "台湾", "台灣")));
    REQUIRE(!FanyImeIpc::IsCompleteVariantChoiceChange(Remember("tai'wan", "", "台灣")));
    REQUIRE(!FanyImeIpc::IsCompleteVariantChoiceChange(Remember("tai'wan", "台湾", "")));
    REQUIRE(!FanyImeIpc::IsCompleteVariantChoiceChange(Forget("", "台湾")));
    REQUIRE(!FanyImeIpc::IsCompleteVariantChoiceChange(Forget("tai'wan", "")));
}

// ---- 写入失败时重试 ----

namespace
{
// 前 failures 次调用返回假、之后返回真的写操作，calls 记下调用次数。
auto WriteFailingFirst(int failures, int &calls)
{
    return [failures, &calls] { return ++calls > failures; };
}
} // namespace

TEST_CASE(variant_choice_write_stops_after_the_first_success)
{
    int calls = 0;
    REQUIRE(FanyImeIpc::RetryVariantChoiceWrite(WriteFailingFirst(0, calls)));
    REQUIRE_EQ(calls, 1);
}

TEST_CASE(variant_choice_write_succeeds_on_the_last_attempt)
{
    int calls = 0;
    REQUIRE(FanyImeIpc::RetryVariantChoiceWrite(WriteFailingFirst(2, calls)));
    REQUIRE_EQ(calls, 3);
}

TEST_CASE(variant_choice_write_gives_up_after_three_failures)
{
    int calls = 0;
    REQUIRE(!FanyImeIpc::RetryVariantChoiceWrite(WriteFailingFirst(100, calls)));
    REQUIRE_EQ(calls, 3);
}

// ---- 查记录的累计用时上限 ----

namespace
{
// 手动拨动的时钟，单位毫秒。
struct FakeClock
{
    std::int64_t now_ms = 1000;
    FanyImeIpc::MillisecondClock Reader()
    {
        return [this] { return now_ms; };
    }
};
} // namespace

// 累计用时没到上限时每次都查，结果原样返回。计时从第一次调用开始，创建之后过了多久不算。
TEST_CASE(lookup_budget_calls_the_lookup_within_the_budget)
{
    FakeClock clock;
    int calls = 0;
    const auto limited = FanyImeIpc::WithLookupBudget(
        [&calls](const std::string &, const std::string &) -> std::optional<std::string> {
            ++calls;
            return "記錄";
        },
        50, clock.Reader());
    clock.now_ms += 500;
    REQUIRE_EQ(limited("tai'wan", "台湾"), std::optional<std::string>("記錄"));
    clock.now_ms += 49;
    REQUIRE_EQ(limited("tai'wan", "台湾"), std::optional<std::string>("記錄"));
    REQUIRE_EQ(calls, 2);
}

// 累计用时一达到上限就不再查，一律返回空。
TEST_CASE(lookup_budget_stops_calling_the_lookup_once_the_budget_is_spent)
{
    FakeClock clock;
    int calls = 0;
    const auto limited = FanyImeIpc::WithLookupBudget(
        [&calls](const std::string &, const std::string &) -> std::optional<std::string> {
            ++calls;
            return "記錄";
        },
        50, clock.Reader());
    REQUIRE(limited("tai'wan", "台湾").has_value());
    clock.now_ms += 50;
    REQUIRE(!limited("tai'wan", "台湾").has_value());
    REQUIRE(!limited("tai", "台").has_value());
    REQUIRE_EQ(calls, 1);
}

// 一次查询本身就超过上限时，这次查到的结果照样返回，之后不再查。
TEST_CASE(lookup_budget_keeps_the_result_of_a_lookup_that_overruns_the_budget)
{
    FakeClock clock;
    int calls = 0;
    const auto limited = FanyImeIpc::WithLookupBudget(
        [&](const std::string &, const std::string &) -> std::optional<std::string> {
            ++calls;
            clock.now_ms += 200;
            return "記錄";
        },
        50, clock.Reader());
    REQUIRE_EQ(limited("tai'wan", "台湾"), std::optional<std::string>("記錄"));
    REQUIRE(!limited("tai", "台").has_value());
    REQUIRE_EQ(calls, 1);
}

TEST_CASE(lookup_budget_over_an_empty_lookup_answers_empty)
{
    FakeClock clock;
    const auto limited = FanyImeIpc::WithLookupBudget(FanyImeIpc::RememberedSpellingLookup{}, 50, clock.Reader());
    REQUIRE(!limited("tai'wan", "台湾").has_value());
}

// 同一次展开里靠前的候选先查：查到上限之前的候选排记住的写法，之后的候选排地区默认写法。
TEST_CASE(lookup_budget_leaves_later_candidates_in_regional_order)
{
    FakeClock clock;
    const test::TableTraditionalSpellingIndex spellings(
        {{"台湾", {"臺灣", "臺灣", "台灣"}}, {"台北", {"臺北", "臺北", "台北"}}});
    const auto limited = FanyImeIpc::WithLookupBudget(
        [&](const std::string &, const std::string &word) -> std::optional<std::string> {
            clock.now_ms += 60;
            return word == "台湾" ? std::optional<std::string>("台灣") : std::optional<std::string>("台北");
        },
        50, clock.Reader());
    const auto expanded =
        FanyImeIpc::ExpandTraditionalVariants({WordItem("tai'wan", "台湾", 100, CandidateSource::Database, "tai'wan"),
                                               WordItem("tai'bei", "台北", 90, CandidateSource::Database, "tai'bei")},
                                              spellings, FanyImeIpc::TraditionalRegion::Taiwan, limited);
    Spellings shown;
    for (const auto &item : expanded)
        shown.push_back(item.output_text);
    REQUIRE_EQ(shown, (Spellings{"台灣", "臺灣", "臺北", "台北"}));
}

// ---- 造词完成、提前返回分支的「前缀 + 候选」 ----

// 上屏的整串等于整词的地区默认第一写法：等于改回默认，删掉这个（键, 底词）的记录。分段选了非第一写法、
// 拼起来恰好是整词默认写法（「干」选「乾」、「燥」选「燥」，整词默认是「乾燥」）时也一样。
TEST_CASE(variant_choice_composed_word_forgets_when_the_committed_text_is_the_regional_default)
{
    RequireChange(VariantChoiceForComposedWord(kTraditional, kSameSetSomeNotFirst, "gan'zao", "干燥", "乾燥", "乾燥"),
                  Forget("gan'zao", "干燥"));
    RequireChange(
        VariantChoiceForComposedWord(kTraditional, kSameSetAllFirst, "tai'wan'ren", "台湾人", "臺灣人", "臺灣人"),
        Forget("tai'wan'ren", "台湾人"));
}

// 各段字形相同、有一段没选第一写法、上屏串不是地区默认写法：记下上屏串。
TEST_CASE(variant_choice_composed_word_remembers_a_same_character_set_text_with_a_non_first_segment)
{
    RequireChange(VariantChoiceForComposedWord(kTraditional, kSameSetSomeNotFirst, "gan'zao", "干早", "乾早", "幹早"),
                  Remember("gan'zao", "干早", "乾早"));
}

// 各段都选第一写法时，产品上屏的是整词的地区默认写法（「台」「风」上屏「颱風」），走上面删记录的分支。
// 上屏串却是各段第一写法的拼接（「臺風」）这种输入，产品走不到；遇到时不记也不删。
TEST_CASE(variant_choice_composed_word_ignores_a_text_made_of_first_spellings_only)
{
    REQUIRE(
        !VariantChoiceForComposedWord(kTraditional, kSameSetAllFirst, "tai'feng", "台风", "臺風", "颱風").has_value());
}

// 造词途中切过字形，上屏串是简繁混排（「开发區」）：不是一种写法，不记也不删。
TEST_CASE(variant_choice_composed_word_ignores_a_mixed_character_set_text)
{
    for (const auto &segments : {kMixedSetSomeNotFirst, kMixedSetAllFirst})
        REQUIRE(!VariantChoiceForComposedWord(kTraditional, segments, "kai'fa'qu", "开发区", "开发區", "開發區")
                     .has_value());
}

// 切过字形的各段拼起来恰好等于地区默认写法（「太」在简体下选、「阳」在繁体下选）时也不删：混排串不算改回默认。
TEST_CASE(variant_choice_composed_word_keeps_the_record_when_a_mixed_text_equals_the_regional_default)
{
    for (const auto &segments : {kMixedSetSomeNotFirst, kMixedSetAllFirst})
        REQUIRE(!VariantChoiceForComposedWord(kTraditional, segments, "tai'yang", "太阳", "太陽", "太陽").has_value());
}

// 不读写记录的环境（简体、日语、U 模式）和缺字段（键、底词或上屏串为空）都不改记录，不论字形一致与否。
TEST_CASE(variant_choice_composed_word_does_nothing_out_of_scope_whatever_the_character_sets)
{
    for (const auto &segments : {kSameSetSomeNotFirst, kMixedSetSomeNotFirst})
    {
        for (const auto &scope : {kSimplified, kJapanese, kRadical})
        {
            REQUIRE(!VariantChoiceForComposedWord(scope, segments, "gan'zao", "干早", "乾早", "幹早").has_value());
            REQUIRE(!VariantChoiceForComposedWord(scope, segments, "gan'zao", "干早", "幹早", "幹早").has_value());
        }
        REQUIRE(!VariantChoiceForComposedWord(kTraditional, segments, "", "干早", "乾早", "幹早").has_value());
        REQUIRE(!VariantChoiceForComposedWord(kTraditional, segments, "", "干早", "幹早", "幹早").has_value());
        REQUIRE(!VariantChoiceForComposedWord(kTraditional, segments, "gan'zao", "", "乾早", "幹早").has_value());
        REQUIRE(!VariantChoiceForComposedWord(kTraditional, segments, "gan'zao", "", "幹早", "幹早").has_value());
        REQUIRE(!VariantChoiceForComposedWord(kTraditional, segments, "gan'zao", "干早", "", "幹早").has_value());
        REQUIRE(!VariantChoiceForComposedWord(kTraditional, segments, "gan'zao", "干早", "", "").has_value());
    }
}

// ---- 造词各段的字形是否相同 ----

TEST_CASE(same_character_set_holds_across_three_traditional_segments)
{
    bool same = SameCharacterSetAfterSegment(true, kTraditionalSet, kTraditionalSet);
    REQUIRE(same);
    same = SameCharacterSetAfterSegment(same, kTraditionalSet, kTraditionalSet);
    REQUIRE(same);
    same = SameCharacterSetAfterSegment(same, kTraditionalSet, kTraditionalSet);
    REQUIRE(same);
}

TEST_CASE(same_character_set_breaks_when_a_later_segment_differs_from_the_first)
{
    bool same = SameCharacterSetAfterSegment(true, kSimplifiedSet, kSimplifiedSet);
    REQUIRE(same);
    same = SameCharacterSetAfterSegment(same, kSimplifiedSet, kTraditionalSet);
    REQUIRE(!same);
}

// 繁、简、繁：第三段回到第一段的字形，第二段已经不同，结果仍为假。
TEST_CASE(same_character_set_stays_false_after_returning_to_the_first_character_set)
{
    bool same = SameCharacterSetAfterSegment(true, kTraditionalSet, kTraditionalSet);
    REQUIRE(same);
    same = SameCharacterSetAfterSegment(same, kTraditionalSet, kSimplifiedSet);
    REQUIRE(!same);
    same = SameCharacterSetAfterSegment(same, kTraditionalSet, kTraditionalSet);
    REQUIRE(!same);
}

// 第二段已经不同之后，第三段仍与第一段不同：结果为假，第三段的字形相同与否不会把它翻回真。
TEST_CASE(same_character_set_stays_false_when_a_later_segment_also_differs_from_the_first)
{
    REQUIRE(!SameCharacterSetAfterSegment(false, kSimplifiedSet, kTraditionalSet));
    REQUIRE(!SameCharacterSetAfterSegment(false, kTraditionalSet, kSimplifiedSet));
}

// ---- 删除底词 ----

// 日语候选删的不是拼音词库里的底词，同形的中文底词的记录留着；没有底词时什么都不删。
TEST_CASE(variant_choice_deletion_keeps_records_for_japanese_and_empty_words)
{
    REQUIRE(!FanyImeIpc::VariantChoiceWordToForgetOnDelete(kJapanese, Variant("台湾", "", 0)).has_value());
    WordItem japanese = Variant("台湾", "", 0);
    japanese.scheme = SchemeType::JapaneseRomaji;
    REQUIRE(!FanyImeIpc::VariantChoiceWordToForgetOnDelete(kTraditional, japanese).has_value());
    REQUIRE(!FanyImeIpc::VariantChoiceWordToForgetOnDelete(kTraditional, Variant("", "", 0)).has_value());
}

// 删五笔候选删的是五笔码表里的那一行，拼音词库里同形的底词还在，它的写法记录留着；
// 删拼音（全拼、双拼）候选才连带删记录，简体、繁体、U 模式都一样。
TEST_CASE(variant_choice_deletion_forgets_only_for_pinyin_candidates)
{
    for (const auto &scope : {kTraditional, kSimplified, kRadical})
    {
        WordItem wubi = Variant("台湾", "", 0, "");
        wubi.scheme = SchemeType::Wubi;
        REQUIRE(!FanyImeIpc::VariantChoiceWordToForgetOnDelete(scope, wubi).has_value());
        WordItem shuangpin = Variant("台湾", "", 0);
        shuangpin.scheme = SchemeType::Shuangpin;
        REQUIRE_EQ(FanyImeIpc::VariantChoiceWordToForgetOnDelete(scope, shuangpin).value_or(""), std::string("台湾"));
        WordItem quanpin = Variant("台湾", "", 0);
        quanpin.scheme = SchemeType::Quanpin;
        REQUIRE_EQ(FanyImeIpc::VariantChoiceWordToForgetOnDelete(scope, quanpin).value_or(""), std::string("台湾"));
    }
}

// ---- 提前返回分支落库的词 ----

// 前缀接上整句候选落库时，读音是两段 canonical 全拼的拼接，词是两段的简体底词：屏幕上显示的繁体写法
// 不进用户词库。没有前缀时只取候选自己的读音，前面不带分隔符。
TEST_CASE(early_return_phrase_stores_the_simplified_words)
{
    const FanyImeIpc::ChosenText prefix{"台湾", "台灣", false};
    const FanyImeIpc::ChosenText candidate{"人民", "人民", true};
    const auto phrase = FanyImeIpc::EarlyReturnPhraseToStore("tai'wan", prefix, "ren'min", candidate);
    REQUIRE_EQ(phrase.pinyin, std::string("tai'wan'ren'min"));
    REQUIRE_EQ(phrase.word, std::string("台湾人民"));

    const auto standalone = FanyImeIpc::EarlyReturnPhraseToStore("", FanyImeIpc::ChosenText{}, "tai'wan'ren",
                                                                 FanyImeIpc::ChosenText{"台湾人", "臺灣人", true});
    REQUIRE_EQ(standalone.pinyin, std::string("tai'wan'ren"));
    REQUIRE_EQ(standalone.word, std::string("台湾人"));
}

// ---- 展开时查记录：记录从注入的查询函数来 ----

// 展开时列表里每个要展开、键不为空的候选各查一次记录，只有一种写法的也查；没有全拼键的、原样保留的不查。
TEST_CASE(variant_choice_expansion_looks_up_each_expanded_candidate_that_has_a_key_once)
{
    std::vector<std::pair<std::string, std::string>> asked;
    const test::TableTraditionalSpellingIndex spellings({{"台湾", {"臺灣", "臺灣", "台灣"}}});
    const auto remembered = [&asked](const std::string &key, const std::string &word) -> std::optional<std::string> {
        asked.emplace_back(key, word);
        return std::nullopt;
    };
    const WordItem taiwan("tai'wan", "台湾", 100, CandidateSource::Database, "tai'wan");
    const WordItem tai("tai", "太", 90, CandidateSource::UserDatabase, "tai");
    const WordItem sentence("tai'wan'ren", "台湾人", 85, CandidateSource::Generated, "tai'wan'ren");
    const WordItem keyless("tw", "台湾", 80, CandidateSource::Database, "");
    const WordItem emoji("tai", "台", 70, CandidateSource::Emoji, "tai");

    (void)FanyImeIpc::ExpandTraditionalVariants({taiwan, tai, sentence, keyless, emoji}, spellings,
                                                FanyImeIpc::TraditionalRegion::Taiwan, remembered);
    REQUIRE_EQ(asked, (std::vector<std::pair<std::string, std::string>>{
                          {"tai'wan", "台湾"}, {"tai", "太"}, {"tai'wan'ren", "台湾人"}}));
}

// 云、AI 候选带全拼键也不查记录，同样带键的词库候选照查。
TEST_CASE(variant_choice_expansion_does_not_look_up_cloud_and_ai_candidates)
{
    const test::TableTraditionalSpellingIndex spellings({{"台湾", {"臺灣", "臺灣", "台灣"}}});
    const auto expand_counting_lookups = [&](CandidateSource source) {
        int lookups = 0;
        const auto expanded = FanyImeIpc::ExpandTraditionalVariants(
            {WordItem("tai'wan", "台湾", 100, source, "tai'wan")}, spellings, FanyImeIpc::TraditionalRegion::Taiwan,
            [&lookups](const std::string &, const std::string &) -> std::optional<std::string> {
                ++lookups;
                return std::string("台灣");
            });
        REQUIRE(!expanded.empty());
        return std::make_pair(lookups, expanded.front().output_text);
    };
    for (const auto source : {CandidateSource::CloudSuggestion, CandidateSource::AiSuggestion})
        REQUIRE(expand_counting_lookups(source) == std::make_pair(0, std::string("臺灣")));
    REQUIRE(expand_counting_lookups(CandidateSource::Database) == std::make_pair(1, std::string("台灣")));
}

// 没有持有函数的查询函数（{}、nullptr）按没有记录处理：结果与 NoRememberedSpellings() 相同，不抛异常。
// 候选带全拼键，展开确实会走到查记录那一步。
TEST_CASE(variant_choice_expansion_treats_an_empty_lookup_as_no_records)
{
    const test::TableTraditionalSpellingIndex spellings({{"台湾", {"臺灣", "臺灣", "台灣"}}});
    const std::vector<WordItem> items = {WordItem("tai'wan", "台湾", 100, CandidateSource::Database, "tai'wan")};
    const auto expand = [&](const FanyImeIpc::RememberedSpellingLookup &remembered) {
        std::vector<std::pair<std::string, int>> shown;
        for (const auto &item :
             FanyImeIpc::ExpandTraditionalVariants(items, spellings, FanyImeIpc::TraditionalRegion::Taiwan, remembered))
            shown.emplace_back(item.output_text, item.variant_rank);
        return shown;
    };
    const std::vector<std::pair<std::string, int>> regional_order = {{"臺灣", 0}, {"台灣", 1}};
    REQUIRE(expand(FanyImeIpc::NoRememberedSpellings()) == regional_order);
    REQUIRE(expand(FanyImeIpc::RememberedSpellingLookup{}) == regional_order);
    REQUIRE(expand(nullptr) == regional_order);
}

// NoRememberedSpellings() 持有函数，不论键与底词都给空。
TEST_CASE(no_remembered_spellings_answers_empty_for_any_key_and_word)
{
    const auto remembered = FanyImeIpc::NoRememberedSpellings();
    REQUIRE(static_cast<bool>(remembered));
    REQUIRE(!remembered("tai'wan", "台湾").has_value());
    REQUIRE(!remembered("", "").has_value());
}

// ---- 接日志库的用例共用的临时库与查询函数 ----

namespace
{
// 临时目录里的一个日志库 msime_user.db，析构时连目录一起删掉。
class TemporaryJournal
{
  public:
    explicit TemporaryJournal(const std::string &name) : directory_(test::TempRoot() / name)
    {
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
    }
    ~TemporaryJournal()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
    TemporaryJournal(const TemporaryJournal &) = delete;
    TemporaryJournal &operator=(const TemporaryJournal &) = delete;

    std::string path() const
    {
        return test::Utf8(directory_ / "msime_user.db");
    }

  private:
    std::filesystem::path directory_;
};

// 产品的查询函数：记录每次都从日志库 user 查。
FanyImeIpc::RememberedSpellingLookup JournalLookup(const std::string &user)
{
    return FanyImeIpc::JournalRememberedSpellingLookup([user] { return user; });
}

std::optional<std::string> RecordOf(const std::string &user, const std::string &key, const std::string &word)
{
    return user_dictionary::traditional_variant_choice(user, key, word);
}

void Write(const std::string &user, const std::optional<VariantChoiceChange> &change)
{
    REQUIRE(change.has_value());
    REQUIRE(FanyImeIpc::WriteVariantChoiceChange(user, *change));
}

// 「台湾」的三种转换结果：s2t 臺灣、t2tw 臺灣、t2hk 台灣。没有字表写法。
test::TableTraditionalSpellingIndex TaiwanIndex()
{
    return test::TableTraditionalSpellingIndex({{"台湾", {"臺灣", "臺灣", "台灣"}}});
}

// 台湾地区按 index 展开，记录查 remembered。这里的底词都是简体写法。
std::vector<WordItem> ExpandWith(const ChineseConverter::TraditionalSpellingIndex &index,
                                 const FanyImeIpc::RememberedSpellingLookup &remembered, std::vector<WordItem> items)
{
    return FanyImeIpc::ExpandTraditionalVariants(std::move(items), index, FanyImeIpc::TraditionalRegion::Taiwan,
                                                 remembered);
}

Spellings SpellingsIn(const std::vector<WordItem> &items)
{
    Spellings spellings;
    for (const auto &item : items)
        spellings.push_back(item.output_text);
    return spellings;
}

std::string RegionalFirstSpelling(const std::string &word)
{
    return FanyImeIpc::TraditionalSpellingsOf(word, TaiwanIndex(), FanyImeIpc::TraditionalRegion::Taiwan).front();
}

// 选中 item 之后要做的改动写进日志库 user；不用改时什么都不写。
void Select(const std::string &user, const VariantChoiceScope &scope, const WordItem &item)
{
    const auto change =
        VariantChoiceForSelection(scope, item, FanyImeIpc::VariantChoiceKeyOf(item), RegionalFirstSpelling(item.word));
    if (change)
        Write(user, change);
}
} // namespace

// ---- 端到端：判定 → 写进日志库 → 同一个查询函数下次展开从日志库查 ----

// 台湾地区：「台湾」默认是 臺灣、台灣。选了「台灣」之后再打同样的音，第一项是「台灣」；
// 再选回「臺灣」，记录删除，顺序回到地区默认。
TEST_CASE(variant_choice_chain_selected_spelling_leads_next_time_until_the_default_is_chosen_again)
{
    const TemporaryJournal journal("msime-variant-choice-chain-selection");
    const std::string user = journal.path();
    const auto spellings = TaiwanIndex();
    const auto remembered = JournalLookup(user);
    const WordItem taiwan("tai'wan", "台湾", 100, CandidateSource::Database, "tai'wan");

    auto expanded = ExpandWith(spellings, remembered, {taiwan});
    REQUIRE_EQ(SpellingsIn(expanded), (Spellings{"臺灣", "台灣"}));
    Select(user, kTraditional, expanded[1]);
    REQUIRE_EQ(RecordOf(user, "tai'wan", "台湾"), std::optional<std::string>("台灣"));

    expanded = ExpandWith(spellings, remembered, {taiwan});
    REQUIRE_EQ(SpellingsIn(expanded), (Spellings{"台灣", "臺灣"}));
    REQUIRE_EQ(expanded[0].variant_rank, 0);

    // 选当前第一项（记住的那个）不改记录。
    Select(user, kTraditional, expanded[0]);
    REQUIRE_EQ(RecordOf(user, "tai'wan", "台湾"), std::optional<std::string>("台灣"));

    Select(user, kTraditional, expanded[1]);
    REQUIRE(!RecordOf(user, "tai'wan", "台湾").has_value());
    REQUIRE_EQ(SpellingsIn(ExpandWith(spellings, remembered, {taiwan})), (Spellings{"臺灣", "台灣"}));
}

// 字段不齐的改动不写：原有的记录留着，返回假。
TEST_CASE(variant_choice_write_skips_an_incomplete_change)
{
    const TemporaryJournal journal("msime-variant-choice-incomplete-write");
    const std::string user = journal.path();
    Write(user, Remember("tai'wan", "台湾", "台灣"));
    REQUIRE(!FanyImeIpc::WriteVariantChoiceChange(user, Remember("tai'wan", "台湾", "")));
    REQUIRE(!FanyImeIpc::WriteVariantChoiceChange(user, Forget("", "台湾")));
    REQUIRE(!FanyImeIpc::WriteVariantChoiceChange(user, Forget("tai'wan", "")));
    REQUIRE_EQ(RecordOf(user, "tai'wan", "台湾"), std::optional<std::string>("台灣"));
}

// 记录挂在（键, 底词）上：同一个底词在别的键下照旧按地区默认排。
TEST_CASE(variant_choice_chain_record_applies_only_to_its_own_key)
{
    const TemporaryJournal journal("msime-variant-choice-chain-own-key");
    const std::string user = journal.path();
    Write(user, Remember("tai'wan", "台湾", "台灣"));
    const auto spellings = TaiwanIndex();
    const auto remembered = JournalLookup(user);
    const WordItem own_key("tai'wan", "台湾", 100, CandidateSource::Database, "tai'wan");
    const WordItem other_key("tw", "台湾", 100, CandidateSource::Database, "tai'wa'n");
    REQUIRE_EQ(SpellingsIn(ExpandWith(spellings, remembered, {own_key})), (Spellings{"台灣", "臺灣"}));
    REQUIRE_EQ(SpellingsIn(ExpandWith(spellings, remembered, {other_key})), (Spellings{"臺灣", "台灣"}));
}

// ---- 端到端：造词逐段选出的写法 → 写进日志库 → 同一个查询函数下次展开从日志库查 ----

namespace
{
// 「干早」整词经 s2t、t2tw、t2hk 只得「幹早」；「干」的单字转换是「幹」，字表写法是 幹、乾、干、榦（与
// OpenCC STCharacters 一致）；「早」转换成自己、不在字表里。
test::TableTraditionalSpellingIndex GanzaoIndex()
{
    return test::TableTraditionalSpellingIndex({{"干早", {"幹早", "幹早", "幹早"}}, {"干", {"幹", "幹", "幹"}}},
                                               {{"干", {"幹", "乾", "干", "榦"}}});
}

std::string GanzaoRegionalFirstSpelling(const std::string &word)
{
    return FanyImeIpc::TraditionalSpellingsOf(word, GanzaoIndex(), FanyImeIpc::TraditionalRegion::Taiwan).front();
}
} // namespace

// 台湾地区造词：第一段选「干」的第二写法「乾」，第二段选「早」，上屏「乾早」。记下之后再打同样的音，
// 「干早」这组第一项是「乾早」、第二项「幹早」；再选「幹早」（地区默认写法），记录删除，只剩「幹早」。
TEST_CASE(variant_choice_chain_composed_spelling_leads_next_time_until_the_default_is_chosen_again)
{
    const TemporaryJournal journal("msime-variant-choice-chain-composed");
    const std::string user = journal.path();
    const auto spellings = GanzaoIndex();
    const auto remembered = JournalLookup(user);
    const WordItem ganzao("gan'zao", "干早", 100, CandidateSource::UserDatabase, "gan'zao");
    REQUIRE_EQ(SpellingsIn(ExpandWith(spellings, remembered, {ganzao})), (Spellings{"幹早"}));

    const auto composed = VariantChoiceForComposedWord(kTraditional, kSameSetSomeNotFirst, "gan'zao", "干早", "乾早",
                                                       GanzaoRegionalFirstSpelling("干早"));
    RequireChange(composed, Remember("gan'zao", "干早", "乾早"));
    Write(user, composed);

    auto expanded = ExpandWith(spellings, remembered, {ganzao});
    REQUIRE_EQ(SpellingsIn(expanded), (Spellings{"乾早", "幹早"}));
    REQUIRE_EQ(expanded[0].variant_rank, 0);
    REQUIRE_EQ(expanded[1].variant_rank, 1);

    const auto selected =
        VariantChoiceForSelection(kTraditional, expanded[1], FanyImeIpc::VariantChoiceKeyOf(expanded[1]),
                                  GanzaoRegionalFirstSpelling(expanded[1].word));
    RequireChange(selected, Forget("gan'zao", "干早"));
    Write(user, selected);
    REQUIRE(!RecordOf(user, "gan'zao", "干早").has_value());
    REQUIRE_EQ(SpellingsIn(ExpandWith(spellings, remembered, {ganzao})), (Spellings{"幹早"}));
}
