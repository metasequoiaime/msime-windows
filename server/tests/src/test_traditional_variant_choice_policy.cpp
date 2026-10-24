#include "ipc/candidate_selection_policy.h"
#include "ipc/traditional_variant_choice_policy.h"
#include "ipc/traditional_variant_choice_store.h"
#include "ipc/traditional_variant_expansion_policy.h"
#include "tests/includes/table_traditional_spelling_index.h"
#include "tests/includes/test_framework.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

using FanyImeIpc::TraditionalVariantChoiceStore;
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

user_dictionary::TraditionalVariantChoice Row(const std::string &key, const std::string &word,
                                              const std::string &spelling)
{
    return {key, word, spelling};
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

// ---- 内存副本 ----

// 产品里的真实顺序：第一次展开时 ClaimLoad，读表排在写线程上；读完之前用户又改了几次，这些改动的
// 写库排在读表后面，读到的是改动之前的旧行。载入时按发生顺序重放：同一个（键, 底词）先改写、再删、
// 再写，留下最后一次写的；先写后删的不留；按底词删的也照样删。
TEST_CASE(variant_choice_store_replays_edits_made_while_the_table_is_loading_in_order)
{
    TraditionalVariantChoiceStore store;
    REQUIRE(store.ClaimLoad());
    store.Apply(Remember("tai'wan", "台湾", "臺灣"));
    store.Apply(Forget("tai'wan", "台湾"));
    store.Apply(Remember("tai'wan", "台湾", "台灣"));
    store.Apply(Remember("hou'tai", "后台", "後台"));
    store.Apply(Forget("hou'tai", "后台"));
    store.ForgetWord("群众");
    store.Load({Row("tai'wan", "台湾", "臺灣"), Row("hou'tai", "后台", "後臺"), Row("qun'zhong", "群众", "羣衆"),
                Row("wei", "为", "爲")});
    REQUIRE_EQ(store.Find("tai'wan", "台湾").value_or(""), std::string("台灣"));
    REQUIRE(!store.Find("hou'tai", "后台").has_value());
    REQUIRE(!store.Find("qun'zhong", "群众").has_value());
    REQUIRE_EQ(store.Find("wei", "为").value_or(""), std::string("爲"));
}

// ClaimLoad 之前做的改动，调用方已经把它的写库排在之后那次读表前面，读到的表里就有它的结果，所以它
// 只改内存、不进重放队列。简体模式从不展开、从不 ClaimLoad，删词再多也不在内存里攒下待重放的改动。
TEST_CASE(variant_choice_store_does_not_replay_edits_made_before_the_load_is_claimed)
{
    TraditionalVariantChoiceStore store;
    store.ForgetWord("台湾");
    store.Apply(Remember("hou'tai", "后台", "後台"));
    REQUIRE_EQ(store.Find("hou'tai", "后台").value_or(""), std::string("後台"));
    REQUIRE(store.ClaimLoad());
    store.Load({Row("tai'wan", "台湾", "台灣")});
    REQUIRE_EQ(store.Find("tai'wan", "台湾").value_or(""), std::string("台灣"));
    REQUIRE(!store.Find("hou'tai", "后台").has_value());
}

// 只载入一次：没有 ClaimLoad 时的 Load、载入之后再来的 Load 都不起作用，载入之后的改动不会被冲掉。
TEST_CASE(variant_choice_store_loads_only_once)
{
    TraditionalVariantChoiceStore unclaimed;
    unclaimed.Apply(Remember("tai'wan", "台湾", "台灣"));
    unclaimed.Load({Row("wei", "为", "爲")});
    REQUIRE_EQ(unclaimed.Find("tai'wan", "台湾").value_or(""), std::string("台灣"));
    REQUIRE(!unclaimed.Find("wei", "为").has_value());

    TraditionalVariantChoiceStore store;
    REQUIRE(store.ClaimLoad());
    store.Load({Row("wei", "为", "爲")});
    store.Apply(Remember("tai'wan", "台湾", "台灣"));
    store.Load({Row("hou'tai", "后台", "後臺")});
    REQUIRE_EQ(store.Find("tai'wan", "台湾").value_or(""), std::string("台灣"));
    REQUIRE_EQ(store.Find("wei", "为").value_or(""), std::string("爲"));
    REQUIRE(!store.Find("hou'tai", "后台").has_value());
}

// ---- 端到端：判定 → 记录 → 下次展开 ----

namespace
{
// 「台湾」的三种转换结果照 PR-1〈实跑数据〉表：s2t 臺灣、t2tw 臺灣、t2hk 台灣。没有字表写法。
// 写法记录从 store 查。
test::TableTraditionalSpellingIndex TaiwanIndex(const TraditionalVariantChoiceStore &store)
{
    return test::TableTraditionalSpellingIndex(
        {{"台湾", {"臺灣", "臺灣", "台灣"}}}, {}, {},
        [&store](const std::string &key, const std::string &word) { return store.Find(key, word); });
}

// 产品展开时的组合：每个候选的写法按它的全拼键与底词查记录。这里的底词都是简体写法。
std::vector<WordItem> ExpandWith(const TraditionalVariantChoiceStore &store, std::vector<WordItem> items)
{
    return FanyImeIpc::ExpandTraditionalVariants(std::move(items), TaiwanIndex(store),
                                                 FanyImeIpc::TraditionalRegion::Taiwan);
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
    const TraditionalVariantChoiceStore no_records;
    return FanyImeIpc::TraditionalSpellingsOf(word, TaiwanIndex(no_records), FanyImeIpc::TraditionalRegion::Taiwan)
        .front();
}
void Select(TraditionalVariantChoiceStore &store, const VariantChoiceScope &scope, const WordItem &item)
{
    const auto change =
        VariantChoiceForSelection(scope, item, FanyImeIpc::VariantChoiceKeyOf(item), RegionalFirstSpelling(item.word));
    if (change)
        store.Apply(*change);
}
} // namespace

// 台湾地区：「台湾」默认是 臺灣、台灣。选了「台灣」之后再打同样的音，第一项是「台灣」；
// 再选回「臺灣」，记录删除，顺序回到地区默认。
TEST_CASE(variant_choice_chain_selected_spelling_leads_next_time_until_the_default_is_chosen_again)
{
    TraditionalVariantChoiceStore store;
    const WordItem taiwan("tai'wan", "台湾", 100, CandidateSource::Database, "tai'wan");

    auto expanded = ExpandWith(store, {taiwan});
    REQUIRE_EQ(SpellingsIn(expanded), (Spellings{"臺灣", "台灣"}));
    Select(store, kTraditional, expanded[1]);
    REQUIRE_EQ(store.Find("tai'wan", "台湾").value_or(""), std::string("台灣"));

    expanded = ExpandWith(store, {taiwan});
    REQUIRE_EQ(SpellingsIn(expanded), (Spellings{"台灣", "臺灣"}));
    REQUIRE_EQ(expanded[0].variant_rank, 0);

    // 选当前第一项（记住的那个）不改记录。
    Select(store, kTraditional, expanded[0]);
    REQUIRE_EQ(store.Find("tai'wan", "台湾").value_or(""), std::string("台灣"));

    Select(store, kTraditional, expanded[1]);
    REQUIRE(!store.Find("tai'wan", "台湾").has_value());
    REQUIRE_EQ(SpellingsIn(ExpandWith(store, {taiwan})), (Spellings{"臺灣", "台灣"}));
}

// 记录挂在（键, 底词）上：同一个底词在别的键下照旧按地区默认排。
TEST_CASE(variant_choice_chain_record_applies_only_to_its_own_key)
{
    TraditionalVariantChoiceStore store;
    store.Apply(Remember("tai'wan", "台湾", "台灣"));
    const WordItem other_key("tw", "台湾", 100, CandidateSource::Database, "tai'wa'n");
    REQUIRE_EQ(SpellingsIn(ExpandWith(store, {other_key})), (Spellings{"臺灣", "台灣"}));
}

// ---- 端到端：造词逐段选出的写法 → 记录 → 下次展开 ----

namespace
{
// 「干早」整词经 s2t、t2tw、t2hk 只得「幹早」；「干」的单字转换是「幹」，字表写法是 幹、乾、干、榦（与
// OpenCC STCharacters 一致）；「早」转换成自己、不在字表里。写法记录从 store 查。
test::TableTraditionalSpellingIndex GanzaoIndex(const TraditionalVariantChoiceStore &store)
{
    return test::TableTraditionalSpellingIndex(
        {{"干早", {"幹早", "幹早", "幹早"}}, {"干", {"幹", "幹", "幹"}}}, {{"干", {"幹", "乾", "干", "榦"}}}, {},
        [&store](const std::string &key, const std::string &word) { return store.Find(key, word); });
}

std::vector<WordItem> ExpandGanzaoWith(const TraditionalVariantChoiceStore &store, std::vector<WordItem> items)
{
    return FanyImeIpc::ExpandTraditionalVariants(std::move(items), GanzaoIndex(store),
                                                 FanyImeIpc::TraditionalRegion::Taiwan);
}

std::string GanzaoRegionalFirstSpelling(const std::string &word)
{
    const TraditionalVariantChoiceStore no_records;
    return FanyImeIpc::TraditionalSpellingsOf(word, GanzaoIndex(no_records), FanyImeIpc::TraditionalRegion::Taiwan)
        .front();
}
} // namespace

// 台湾地区造词：第一段选「干」的第二写法「乾」，第二段选「早」，上屏「乾早」。记下之后再打同样的音，
// 「干早」这组第一项是「乾早」、第二项「幹早」；再选「幹早」（地区默认写法），记录删除，只剩「幹早」。
TEST_CASE(variant_choice_chain_composed_spelling_leads_next_time_until_the_default_is_chosen_again)
{
    TraditionalVariantChoiceStore store;
    const WordItem ganzao("gan'zao", "干早", 100, CandidateSource::UserDatabase, "gan'zao");
    REQUIRE_EQ(SpellingsIn(ExpandGanzaoWith(store, {ganzao})), (Spellings{"幹早"}));

    const auto composed = VariantChoiceForComposedWord(kTraditional, /*all_first_spellings=*/false, "gan'zao", "干早",
                                                       "乾早", GanzaoRegionalFirstSpelling("干早"));
    RequireChange(composed, Remember("gan'zao", "干早", "乾早"));
    store.Apply(*composed);

    auto expanded = ExpandGanzaoWith(store, {ganzao});
    REQUIRE_EQ(SpellingsIn(expanded), (Spellings{"乾早", "幹早"}));
    REQUIRE_EQ(expanded[0].variant_rank, 0);
    REQUIRE_EQ(expanded[1].variant_rank, 1);

    const auto selected =
        VariantChoiceForSelection(kTraditional, expanded[1], FanyImeIpc::VariantChoiceKeyOf(expanded[1]),
                                  GanzaoRegionalFirstSpelling(expanded[1].word));
    RequireChange(selected, Forget("gan'zao", "干早"));
    store.Apply(*selected);
    REQUIRE(!store.Find("gan'zao", "干早").has_value());
    REQUIRE_EQ(SpellingsIn(ExpandGanzaoWith(store, {ganzao})), (Spellings{"幹早"}));
}
