#include "engine/user_dictionary/user_dictionary_journal.h"
#include "ipc/candidate_selection_policy.h"
#include "ipc/traditional_spelling_index_with_choices.h"
#include "ipc/traditional_variant_choice_journal.h"
#include "ipc/traditional_variant_choice_policy.h"
#include "ipc/traditional_variant_expansion_policy.h"
#include "tests/includes/table_traditional_spelling_index.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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

// ---- 造词完成、提前返回分支的「前缀 + 候选」 ----

// 上屏的整串等于整词的地区默认第一写法：等于改回默认，删掉这个（键, 底词）的记录。分段选了非第一写法、
// 拼起来恰好是整词默认写法（「干」选「乾」、「燥」选「燥」，整词默认是「乾燥」）时也一样。
TEST_CASE(variant_choice_composed_word_forgets_when_the_committed_text_is_the_regional_default)
{
    RequireChange(VariantChoiceForComposedWord(kTraditional, false, "gan'zao", "干燥", "乾燥", "乾燥"),
                  Forget("gan'zao", "干燥"));
    RequireChange(VariantChoiceForComposedWord(kTraditional, true, "tai'wan'ren", "台湾人", "臺灣人", "臺灣人"),
                  Forget("tai'wan'ren", "台湾人"));
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
// 删拼音（全拼、双拼）候选才连带删记录。
TEST_CASE(variant_choice_deletion_forgets_only_for_pinyin_candidates)
{
    for (const auto &scope : {kTraditional, kSimplified})
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

// ---- 带写法记录的索引：记录从注入的查询函数来 ----

// 展开时列表里每个要展开、键不为空的候选各查一次记录，只有一种写法的也查；没有全拼键的、原样保留的不查。
TEST_CASE(variant_choice_expansion_looks_up_each_expanded_candidate_that_has_a_key_once)
{
    std::vector<std::pair<std::string, std::string>> asked;
    const test::TableTraditionalSpellingIndex spellings({{"台湾", {"臺灣", "臺灣", "台灣"}}});
    const FanyImeIpc::TraditionalSpellingIndexWithChoices index(
        spellings, [&asked](const std::string &key, const std::string &word) -> std::optional<std::string> {
            asked.emplace_back(key, word);
            return std::nullopt;
        });
    const WordItem taiwan("tai'wan", "台湾", 100, CandidateSource::Database, "tai'wan");
    const WordItem tai("tai", "太", 90, CandidateSource::UserDatabase, "tai");
    const WordItem sentence("tai'wan'ren", "台湾人", 85, CandidateSource::Generated, "tai'wan'ren");
    const WordItem keyless("tw", "台湾", 80, CandidateSource::Database, "");
    const WordItem emoji("tai", "台", 70, CandidateSource::Emoji, "tai");

    (void)FanyImeIpc::ExpandTraditionalVariants({taiwan, tai, sentence, keyless, emoji}, index,
                                                FanyImeIpc::TraditionalRegion::Taiwan);
    REQUIRE_EQ(asked, (std::vector<std::pair<std::string, std::string>>{
                          {"tai'wan", "台湾"}, {"tai", "太"}, {"tai'wan'ren", "台湾人"}}));
}

// ---- 接日志库的用例共用的临时库与索引 ----

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

// 产品的组合：spellings 之上加写法记录，记录每次都从日志库 user 查。spellings 要活得比返回的索引久。
FanyImeIpc::TraditionalSpellingIndexWithChoices JournalIndex(
    const ChineseConverter::TraditionalSpellingIndex &spellings, const std::string &user)
{
    return FanyImeIpc::TraditionalSpellingIndexWithChoices(
        spellings, FanyImeIpc::JournalRememberedSpellingLookup([user] { return user; }));
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

// 「台湾」的三种转换结果照 PR-1〈实跑数据〉表：s2t 臺灣、t2tw 臺灣、t2hk 台灣。没有字表写法，没有写法记录。
test::TableTraditionalSpellingIndex TaiwanIndex()
{
    return test::TableTraditionalSpellingIndex({{"台湾", {"臺灣", "臺灣", "台灣"}}});
}

// 台湾地区按 index 展开。这里的底词都是简体写法。
std::vector<WordItem> ExpandWith(const ChineseConverter::TraditionalSpellingIndex &index, std::vector<WordItem> items)
{
    return FanyImeIpc::ExpandTraditionalVariants(std::move(items), index, FanyImeIpc::TraditionalRegion::Taiwan);
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

// ---- 端到端：判定 → 写进日志库 → 同一个索引下次展开从日志库查 ----

// 台湾地区：「台湾」默认是 臺灣、台灣。选了「台灣」之后再打同样的音，第一项是「台灣」；
// 再选回「臺灣」，记录删除，顺序回到地区默认。
TEST_CASE(variant_choice_chain_selected_spelling_leads_next_time_until_the_default_is_chosen_again)
{
    const TemporaryJournal journal("msime-variant-choice-chain-selection");
    const std::string user = journal.path();
    const auto spellings = TaiwanIndex();
    const auto index = JournalIndex(spellings, user);
    const WordItem taiwan("tai'wan", "台湾", 100, CandidateSource::Database, "tai'wan");

    auto expanded = ExpandWith(index, {taiwan});
    REQUIRE_EQ(SpellingsIn(expanded), (Spellings{"臺灣", "台灣"}));
    Select(user, kTraditional, expanded[1]);
    REQUIRE_EQ(RecordOf(user, "tai'wan", "台湾"), std::optional<std::string>("台灣"));

    expanded = ExpandWith(index, {taiwan});
    REQUIRE_EQ(SpellingsIn(expanded), (Spellings{"台灣", "臺灣"}));
    REQUIRE_EQ(expanded[0].variant_rank, 0);

    // 选当前第一项（记住的那个）不改记录。
    Select(user, kTraditional, expanded[0]);
    REQUIRE_EQ(RecordOf(user, "tai'wan", "台湾"), std::optional<std::string>("台灣"));

    Select(user, kTraditional, expanded[1]);
    REQUIRE(!RecordOf(user, "tai'wan", "台湾").has_value());
    REQUIRE_EQ(SpellingsIn(ExpandWith(index, {taiwan})), (Spellings{"臺灣", "台灣"}));
}

// 记录挂在（键, 底词）上：同一个底词在别的键下照旧按地区默认排。
TEST_CASE(variant_choice_chain_record_applies_only_to_its_own_key)
{
    const TemporaryJournal journal("msime-variant-choice-chain-own-key");
    const std::string user = journal.path();
    Write(user, Remember("tai'wan", "台湾", "台灣"));
    const auto spellings = TaiwanIndex();
    const auto index = JournalIndex(spellings, user);
    const WordItem own_key("tai'wan", "台湾", 100, CandidateSource::Database, "tai'wan");
    const WordItem other_key("tw", "台湾", 100, CandidateSource::Database, "tai'wa'n");
    REQUIRE_EQ(SpellingsIn(ExpandWith(index, {own_key})), (Spellings{"台灣", "臺灣"}));
    REQUIRE_EQ(SpellingsIn(ExpandWith(index, {other_key})), (Spellings{"臺灣", "台灣"}));
}

// ---- 端到端：造词逐段选出的写法 → 写进日志库 → 同一个索引下次展开从日志库查 ----

namespace
{
// 「干早」整词经 s2t、t2tw、t2hk 只得「幹早」；「干」的单字转换是「幹」，字表写法是 幹、乾、干、榦（与
// OpenCC STCharacters 一致）；「早」转换成自己、不在字表里。没有写法记录。
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
    const auto index = JournalIndex(spellings, user);
    const WordItem ganzao("gan'zao", "干早", 100, CandidateSource::UserDatabase, "gan'zao");
    REQUIRE_EQ(SpellingsIn(ExpandWith(index, {ganzao})), (Spellings{"幹早"}));

    const auto composed = VariantChoiceForComposedWord(kTraditional, /*all_first_spellings=*/false, "gan'zao", "干早",
                                                       "乾早", GanzaoRegionalFirstSpelling("干早"));
    RequireChange(composed, Remember("gan'zao", "干早", "乾早"));
    Write(user, composed);

    auto expanded = ExpandWith(index, {ganzao});
    REQUIRE_EQ(SpellingsIn(expanded), (Spellings{"乾早", "幹早"}));
    REQUIRE_EQ(expanded[0].variant_rank, 0);
    REQUIRE_EQ(expanded[1].variant_rank, 1);

    const auto selected =
        VariantChoiceForSelection(kTraditional, expanded[1], FanyImeIpc::VariantChoiceKeyOf(expanded[1]),
                                  GanzaoRegionalFirstSpelling(expanded[1].word));
    RequireChange(selected, Forget("gan'zao", "干早"));
    Write(user, selected);
    REQUIRE(!RecordOf(user, "gan'zao", "干早").has_value());
    REQUIRE_EQ(SpellingsIn(ExpandWith(index, {ganzao})), (Spellings{"幹早"}));
}
