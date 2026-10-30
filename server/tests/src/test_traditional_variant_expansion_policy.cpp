#include "engine/local_modes/quick_phrase_query.h"
#include "ipc/candidate_selection_policy.h"
#include "ipc/traditional_variant_expansion_policy.h"
#include "tests/includes/fake_traditional_spelling_index.h"
#include "tests/includes/test_framework.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace
{
using Spellings = std::vector<std::string>;

// 测试用的写法索引：按 word 查写法表，查不到的词只有它自己一种写法。所有来源都查同一张表，
// 不展开的来源若被误展开，测试会看到多出来的项。单字的写法与 OpenCC 字表经索引过滤后一致：
// 「复」「借」「里」的字表写法里另有独立的简体字「覆」「藉」「哩」，索引不把它们列在「复」「借」「里」下。
// 含「臺」「檯」「颱」「裏」「裡」「鍾」的词不是简体写法，与 OpenCC 繁转简字表一致（它们转成「台」「里」「钟」）。
// 「钟」「锺」的写法都有「鍾」、「余」「馀」的写法都有「餘」，首选底词照繁转简字表的第一个值：鍾→钟、餘→余。
const test::FakeTraditionalSpellingIndex &FakeIndex()
{
    static const test::FakeTraditionalSpellingIndex index(
        {
            {"台", {"臺", "台", "檯", "颱"}},
            {"为", {"為", "爲"}},
            {"里", {"裡", "裏", "里"}},
            {"着", {"著", "着"}},
            {"台湾", {"臺灣", "台灣"}},
            {"因为", {"因為", "因爲"}},
            {"群众", {"群眾", "羣眾", "羣衆"}},
            {"空", {}},
            {"复", {"復", "複"}},
            {"借", {"借"}},
            // 撞写法的两个底词都不等于这个写法时的假数据。
            {"甲", {"丙"}},
            {"乙", {"丙", "丁"}},
            {"戊", {"丙", "己"}},
            {"钟", {"鍾", "鐘", "鈡"}},
            {"锺", {"鍾"}},
            {"余", {"餘", "余"}},
            {"馀", {"餘"}},
        },
        {"臺", "檯", "颱", "裏", "裡", "鍾"}, {{"鍾", "钟"}, {"餘", "余"}});
    return index;
}

std::vector<WordItem> Expand(std::vector<WordItem> items)
{
    return FanyImeIpc::ExpandTraditionalVariants(std::move(items), FakeIndex(), FanyImeIpc::TraditionalRegion::Taiwan);
}

WordItem Rich(const std::string &word, CandidateSource source = CandidateSource::Database)
{
    WordItem item("tai", word, 4321, source, "tai");
    item.scheme = SchemeType::Shuangpin;
    item.fixed_position = 3;
    item.fuzzy = true;
    item.corrected_from = "tia";
    item.sentence_association = true;
    return item;
}

void RequireSameExceptVariant(const WordItem &actual, const WordItem &original)
{
    REQUIRE_EQ(actual.pinyin, original.pinyin);
    REQUIRE_EQ(actual.canonical_pinyin, original.canonical_pinyin);
    REQUIRE_EQ(actual.word, original.word);
    REQUIRE_EQ(actual.weight, original.weight);
    REQUIRE(actual.source == original.source);
    REQUIRE(actual.scheme == original.scheme);
    REQUIRE_EQ(actual.fixed_position, original.fixed_position);
    REQUIRE_EQ(actual.fuzzy, original.fuzzy);
    REQUIRE_EQ(actual.corrected_from, original.corrected_from);
    REQUIRE_EQ(actual.sentence_association, original.sentence_association);
}

void RequireUnchanged(const WordItem &actual, const WordItem &original)
{
    RequireSameExceptVariant(actual, original);
    REQUIRE(actual.output_text.empty());
    REQUIRE_EQ(actual.variant_rank, 0);
}

Spellings OutputTexts(const std::vector<WordItem> &items)
{
    Spellings texts;
    for (const auto &item : items)
        texts.push_back(item.output_text);
    return texts;
}

std::vector<int> Ranks(const std::vector<WordItem> &items)
{
    std::vector<int> ranks;
    for (const auto &item : items)
        ranks.push_back(item.variant_rank);
    return ranks;
}

Spellings Words(const std::vector<WordItem> &items)
{
    Spellings words;
    for (const auto &item : items)
        words.push_back(item.word);
    return words;
}

// 列表里显示为 spelling 的那些项的底词。
Spellings WordsShownAs(const std::vector<WordItem> &items, const std::string &spelling)
{
    Spellings words;
    for (const auto &item : items)
        if (item.output_text == spelling)
            words.push_back(item.word);
    return words;
}

// 列表里底词为 word 的那些项显示的写法，按列表顺序。
Spellings SpellingsShownFor(const std::vector<WordItem> &items, const std::string &word)
{
    Spellings spellings;
    for (const auto &item : items)
        if (item.word == word)
            spellings.push_back(item.output_text);
    return spellings;
}
} // namespace

TEST_CASE(variant_expansion_lists_every_spelling_of_a_dictionary_word_next_to_each_other)
{
    const WordItem tai = Rich("台");
    const auto expanded = Expand({WordItem("ni", "你", 9), tai, WordItem("ta", "他", 1)});
    REQUIRE_EQ(expanded.size(), size_t{6});
    REQUIRE_EQ(Words(expanded), (Spellings{"你", "台", "台", "台", "台", "他"}));
    REQUIRE_EQ(OutputTexts(expanded), (Spellings{"你", "臺", "台", "檯", "颱", "他"}));
    REQUIRE_EQ(Ranks(expanded), (std::vector<int>{0, 0, 1, 2, 3, 0}));
    for (size_t index = 1; index <= 4; ++index)
        RequireSameExceptVariant(expanded[index], tai);
}

TEST_CASE(variant_expansion_expands_user_dictionary_words)
{
    const auto expanded = Expand({WordItem("tai'wan", "台湾", 5, CandidateSource::UserDatabase, "tai'wan")});
    REQUIRE_EQ(OutputTexts(expanded), (Spellings{"臺灣", "台灣"}));
    REQUIRE_EQ(Ranks(expanded), (std::vector<int>{0, 1}));
    REQUIRE(expanded[1].source == CandidateSource::UserDatabase);
    REQUIRE_EQ(expanded[1].canonical_pinyin, std::string("tai'wan"));
}

TEST_CASE(variant_expansion_gives_guessed_sentences_cloud_and_ai_only_their_first_spelling)
{
    for (const auto source : {CandidateSource::Generated, CandidateSource::Fallback, CandidateSource::NeuralDesktop,
                              CandidateSource::NeuralKeyboard, CandidateSource::Collocation,
                              CandidateSource::CloudSuggestion, CandidateSource::AiSuggestion})
    {
        const WordItem sentence = Rich("群众", source);
        const auto expanded = Expand({sentence, WordItem("ta", "他", 1)});
        REQUIRE_EQ(expanded.size(), size_t{2});
        RequireSameExceptVariant(expanded[0], sentence);
        REQUIRE_EQ(expanded[0].output_text, std::string("群眾"));
        REQUIRE_EQ(expanded[0].variant_rank, 0);
        REQUIRE_EQ(expanded[1].output_text, std::string("他"));
    }
}

TEST_CASE(variant_expansion_keeps_other_sources_exactly_as_they_were)
{
    std::vector<WordItem> items;
    for (const auto source : {CandidateSource::EnglishDictionary, CandidateSource::Emoji, CandidateSource::Kaomoji,
                              CandidateSource::Symbol, CandidateSource::DateTime, CandidateSource::QuickPhrase})
        items.push_back(Rich("台", source));
    const auto expanded = Expand(items);
    REQUIRE_EQ(expanded.size(), items.size());
    for (size_t index = 0; index < items.size(); ++index)
        RequireUnchanged(expanded[index], items[index]);
}

// 两个不同底词撞同一种写法、两者的写法都不等于自己的底词时，留前面那项。后面那个底词的这种写法删掉，
// 写法删光了的候选整项不出现；整句只出第一写法，同样参与比较。
TEST_CASE(variant_expansion_drops_a_later_item_whose_display_text_is_already_listed)
{
    const auto expanded = Expand({WordItem("jia", "甲", 9), WordItem("yi", "乙", 8), WordItem("li", "理", 7),
                                  WordItem("jia", "戊", 6, CandidateSource::Generated)});
    REQUIRE_EQ(OutputTexts(expanded), (Spellings{"丙", "丁", "理"}));
    REQUIRE_EQ(Words(expanded), (Spellings{"甲", "乙", "理"}));
    REQUIRE_EQ(Ranks(expanded), (std::vector<int>{0, 0, 0}));
    REQUIRE(std::all_of(expanded.begin(), expanded.end(),
                        [](const WordItem &item) { return item.source == CandidateSource::Database; }));
}

// 繁体字词条「鍾」排在最前面：有两个简体单字底词共用「鍾」时它归首选底词「钟」，繁体字词条整项不出现；
// 只有一个简体底词时照繁体字词条的规则，留给先列出的那项。
TEST_CASE(variant_expansion_lets_the_preferred_word_take_a_shared_spelling_ahead_of_a_traditional_entry)
{
    const auto shared = Expand({WordItem("zhong", "鍾", 9), WordItem("zhong", "锺", 8), WordItem("zhong", "钟", 7)});
    REQUIRE_EQ(WordsShownAs(shared, "鍾"), Spellings{"钟"});
    REQUIRE(SpellingsShownFor(shared, "鍾").empty());
    REQUIRE(SpellingsShownFor(shared, "锺").empty());

    const auto single = Expand({WordItem("zhong", "鍾", 9), WordItem("zhong", "钟", 8)});
    REQUIRE_EQ(WordsShownAs(single, "鍾"), Spellings{"鍾"});
    REQUIRE_EQ(SpellingsShownFor(single, "钟"), (Spellings{"鐘", "鈡"}));
}

// 去重只在底词不同的项之间做：同一个底词以不同拼音出现多次时，各自展开后照原样拼接。
TEST_CASE(variant_expansion_keeps_every_spelling_of_the_same_word_under_another_pinyin)
{
    const std::vector<WordItem> zhe = {WordItem("zhe", "着", 9), WordItem("zhuo", "着", 8)};
    const auto expanded = Expand(zhe);
    REQUIRE_EQ(OutputTexts(expanded), (Spellings{"著", "着", "著", "着"}));
    REQUIRE_EQ(Ranks(expanded), (std::vector<int>{0, 1, 0, 1}));
    REQUIRE_EQ(expanded[0].pinyin, std::string("zhe"));
    REQUIRE_EQ(expanded[2].pinyin, std::string("zhuo"));

    // 与「各自展开后直接拼接」相同。
    auto separately = Expand({zhe[0]});
    const auto second = Expand({zhe[1]});
    separately.insert(separately.end(), second.begin(), second.end());
    REQUIRE_EQ(OutputTexts(expanded), OutputTexts(separately));
    REQUIRE_EQ(Words(expanded), Words(separately));
    REQUIRE_EQ(Ranks(expanded), Ranks(separately));
    for (size_t index = 0; index < expanded.size(); ++index)
        REQUIRE_EQ(expanded[index].pinyin, separately[index].pinyin);

    // 同一底词的整句与词库词也各自保留。
    const auto mixed =
        Expand({WordItem("tai'wan", "台湾", 6), WordItem("tai'wan", "台湾", 5, CandidateSource::Generated)});
    REQUIRE_EQ(OutputTexts(mixed), (Spellings{"臺灣", "台灣", "臺灣"}));
    REQUIRE(mixed[2].source == CandidateSource::Generated);
}

// 日语方案的候选不论来源一律原样保留。
TEST_CASE(variant_expansion_leaves_every_japanese_candidate_alone)
{
    for (const auto source :
         {CandidateSource::Database, CandidateSource::UserDatabase, CandidateSource::Generated,
          CandidateSource::Fallback, CandidateSource::NeuralDesktop, CandidateSource::NeuralKeyboard,
          CandidateSource::Collocation, CandidateSource::CloudSuggestion, CandidateSource::AiSuggestion})
    {
        WordItem japanese("tai", "台", 7, source);
        japanese.scheme = SchemeType::JapaneseRomaji;
        REQUIRE(FanyImeIpc::VariantExpansionFor(japanese) == FanyImeIpc::VariantExpansion::Unchanged);
        const auto expanded = Expand({japanese});
        REQUIRE_EQ(expanded.size(), size_t{1});
        RequireUnchanged(expanded[0], japanese);
    }
}

// 去重只在填了 output_text 的项之间比；原样保留的来源不参与，也不会被删。
TEST_CASE(variant_expansion_does_not_deduplicate_against_sources_it_keeps_unchanged)
{
    const WordItem phrase("tai'wan", "台灣", 0, CandidateSource::QuickPhrase);
    const WordItem english("tai", "臺", 0, CandidateSource::EnglishDictionary);
    const auto expanded = Expand({WordItem("tai'wan", "台湾", 6), phrase, english, WordItem("tai", "台", 3)});
    REQUIRE_EQ(Words(expanded), (Spellings{"台湾", "台湾", "台灣", "臺", "台", "台", "台", "台"}));
    REQUIRE_EQ(OutputTexts(expanded), (Spellings{"臺灣", "台灣", "", "", "臺", "台", "檯", "颱"}));
    RequireUnchanged(expanded[2], phrase);
    RequireUnchanged(expanded[3], english);
}

TEST_CASE(variant_expansion_keeps_a_candidate_unchanged_when_it_has_no_spelling)
{
    const WordItem empty("kong", "空", 3);
    const WordItem cloud("kong", "空", 1, CandidateSource::CloudSuggestion);
    const auto expanded = Expand({empty, cloud});
    REQUIRE_EQ(expanded.size(), size_t{2});
    RequireUnchanged(expanded[0], empty);
    RequireUnchanged(expanded[1], cloud);
}

TEST_CASE(first_variants_only_keeps_one_item_per_word_in_list_order)
{
    const auto expanded =
        Expand({WordItem("tai", "台", 9), WordItem("ni", "Ni", 0, CandidateSource::EnglishDictionary),
                WordItem("wei", "为", 8), WordItem("qun'zhong", "群众", 7, CandidateSource::Generated)});
    const auto first = FanyImeIpc::FirstVariantsOnly(expanded);
    REQUIRE_EQ(Words(first), (Spellings{"台", "Ni", "为", "群众"}));
    REQUIRE_EQ(OutputTexts(first), (Spellings{"臺", "", "為", "群眾"}));
    REQUIRE(std::all_of(first.begin(), first.end(), [](const WordItem &item) { return item.variant_rank == 0; }));
}

// ---- 展开后的列表经过下游策略 ----

TEST_CASE(index_after_local_candidates_steps_over_the_group_and_then_the_quick_phrases)
{
    auto items = Expand({WordItem("tai", "台", 9), WordItem("ta", "他", 8)});
    REQUIRE_EQ(items.size(), size_t{5});
    items.insert(items.begin() + 4, WordItem("tai", "短语一", 0, CandidateSource::QuickPhrase));
    items.insert(items.begin() + 5, WordItem("tai", "短语二", 0, CandidateSource::QuickPhrase));
    // 臺 台 檯 颱 | 短语一 短语二 | 他
    REQUIRE_EQ(FanyImeIpc::IndexAfterLocalCandidates(items, 1), size_t{6});
}

TEST_CASE(mixed_candidate_order_keeps_async_candidates_out_of_a_spelling_group)
{
    auto items = Expand({WordItem("tai", "台", 9), WordItem("ta", "他", 8)});
    items.emplace_back("tai", "cloud", 0, CandidateSource::CloudSuggestion);
    items.emplace_back("tai", "ai", 0, CandidateSource::AiSuggestion);
    items.push_back(metasequoia::local_modes::date_time_menu_item("rq"));
    items.emplace_back("date:ymd_dash", "2026-10-23", 0, CandidateSource::DateTime);
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items.size(), size_t{9});
    REQUIRE_EQ(Words(items), (Spellings{"台", "台", "台", "台", "2026-10-23", items[5].word, "cloud", "ai", "他"}));
    REQUIRE(metasequoia::local_modes::is_date_time_menu_item(items[5]));
}

TEST_CASE(quick_phrase_slot_counts_one_item_per_spelling_group)
{
    WordItem first("tai", "台", 9);
    WordItem follower = first;
    follower.variant_rank = 1;
    REQUIRE(metasequoia::local_modes::counts_toward_quick_phrase_slot(first));
    REQUIRE(!metasequoia::local_modes::counts_toward_quick_phrase_slot(follower));

    // 同一份列表展开前后数出的普通候选数相同。
    const std::vector<WordItem> plain = {WordItem("tai", "台", 9), WordItem("ta", "他", 8), WordItem("wei", "为", 7)};
    const auto expanded = Expand(plain);
    const auto ordinary = [](const std::vector<WordItem> &items) {
        return std::count_if(items.begin(), items.end(), metasequoia::local_modes::counts_toward_quick_phrase_slot);
    };
    REQUIRE_EQ(expanded.size(), size_t{7});
    REQUIRE_EQ(ordinary(expanded), ordinary(plain));
    REQUIRE_EQ(ordinary(expanded), std::ptrdiff_t{3});
}

// ---- NormalizeMixedCandidateOrder 按绝对位置插入时不拆开写法组 ----

namespace
{
// 首页 5 项：你 | 臺 台 檯 颱 | 他 ...，第 2–5 位是「台」的写法组。
std::vector<WordItem> GroupStraddlingFirstPage()
{
    return Expand({WordItem("ni", "你", 9), WordItem("tai", "台", 8), WordItem("ta", "他", 7), WordItem("ta", "她", 6),
                   WordItem("ta", "它", 5)});
}

void RequireTaiGroupIntact(const std::vector<WordItem> &items)
{
    const auto head = std::find_if(items.begin(), items.end(),
                                   [](const WordItem &item) { return item.word == "台" && item.variant_rank == 0; });
    REQUIRE(head != items.end());
    REQUIRE(static_cast<size_t>(items.end() - head) >= 4);
    for (int rank = 0; rank < 4; ++rank)
    {
        REQUIRE_EQ((head + rank)->word, std::string("台"));
        REQUIRE_EQ((head + rank)->variant_rank, rank);
    }
}
} // namespace

TEST_CASE(mixed_candidate_order_moves_a_learned_english_slot_out_of_a_spelling_group)
{
    auto items = GroupStraddlingFirstPage();
    items.emplace_back("ta", "ta", 0, CandidateSource::EnglishDictionary);
    FanyImeIpc::EnglishPlacement placement;
    placement.slot = 3;
    placement.input = "ta";
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    RequireTaiGroupIntact(items);
    REQUIRE_EQ(Words(items), (Spellings{"你", "台", "台", "台", "台", "ta", "他", "她", "它"}));
}

// 落点本来就在组外时位置不变：组头之前、组后第一项。
TEST_CASE(mixed_candidate_order_keeps_absolute_positions_that_fall_outside_a_group)
{
    for (const int position : {1, 2, 6})
    {
        auto items = GroupStraddlingFirstPage();
        WordItem fixed("ta", "ta", 0, CandidateSource::EnglishDictionary);
        fixed.fixed_position = position;
        items.push_back(fixed);
        FanyImeIpc::NormalizeMixedCandidateOrder(items);
        REQUIRE_EQ(items[static_cast<size_t>(position - 1)].word, std::string("ta"));
        RequireTaiGroupIntact(items);
    }
}

// ---- 阶段二接线用的纯函数 ----

TEST_CASE(candidate_output_text_returns_the_word_when_not_converting)
{
    WordItem item("tai", "台", 9);
    item.output_text = "臺";
    int conversions = 0;
    const auto to_output = [&](const std::string &text) {
        ++conversions;
        return "converted:" + text;
    };
    REQUIRE_EQ(FanyImeIpc::CandidateOutputTextFor(item, false, to_output), std::string("台"));
    REQUIRE_EQ(conversions, 0);
}

TEST_CASE(candidate_output_text_prefers_the_listed_spelling_when_converting)
{
    WordItem item("tai", "台", 9);
    item.output_text = "檯";
    int conversions = 0;
    const auto to_output = [&](const std::string &text) {
        ++conversions;
        return "converted:" + text;
    };
    REQUIRE_EQ(FanyImeIpc::CandidateOutputTextFor(item, true, to_output), std::string("檯"));
    REQUIRE_EQ(conversions, 0);
}

TEST_CASE(candidate_output_text_converts_an_unexpanded_candidate)
{
    const WordItem item("kuai", "快捷短语", 0, CandidateSource::QuickPhrase);
    const auto to_output = [](const std::string &text) { return "converted:" + text; };
    REQUIRE_EQ(FanyImeIpc::CandidateOutputTextFor(item, true, to_output), std::string("converted:快捷短语"));
}

// 快捷短语组的「越过」学习：选了组后某个候选的第二写法，名次按这一组的第一写法算，每组算一个。
TEST_CASE(quick_phrase_bypass_rank_counts_each_spelling_group_once)
{
    auto items = Expand({WordItem("tai", "台", 9), WordItem("wei", "为", 8), WordItem("ta", "他", 7)});
    // 臺 台 檯 颱 | 短语 | 為 爲 | 他
    items.insert(items.begin() + 4, WordItem("tai", "短语", 0, CandidateSource::QuickPhrase));
    REQUIRE_EQ(items.size(), size_t{8});
    const size_t group_index = 4;
    const auto rank_of = [&](size_t selected) {
        return FanyImeIpc::QuickPhraseBypassRank(items, selected, group_index,
                                                 metasequoia::local_modes::counts_toward_quick_phrase_slot);
    };
    // 「為」与「爲」是同一组，名次都是 1（前面只有「台」一组）。
    REQUIRE(rank_of(5) == std::optional<size_t>(1));
    REQUIRE(rank_of(6) == std::optional<size_t>(1));
    REQUIRE(rank_of(7) == std::optional<size_t>(2));
    // 组前面的候选，以及组前面那组的后续写法，都不学。
    REQUIRE(!rank_of(0).has_value());
    REQUIRE(!rank_of(2).has_value());
    // 不计数的候选（固定位置）不学。
    items[7].fixed_position = 3;
    REQUIRE(!rank_of(7).has_value());
}

// 学到的英文槽位落在写法组里时，占槽位的英文挪到组后，首页的其余英文按原顺序紧跟在它后面。
TEST_CASE(mixed_candidate_order_keeps_extra_first_page_english_after_a_slot_moved_out_of_a_group)
{
    auto items = GroupStraddlingFirstPage();
    items.emplace_back("ta", "ta", 0, CandidateSource::EnglishDictionary);
    items.emplace_back("tab", "tab", 0, CandidateSource::EnglishDictionary);
    items.emplace_back("tad", "tad", 0, CandidateSource::EnglishDictionary);
    FanyImeIpc::EnglishPlacement placement;
    placement.slot = 3;
    placement.input = "ta";
    placement.page_size = 9;
    placement.first_page_count = 3;
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    RequireTaiGroupIntact(items);
    REQUIRE_EQ(Words(items), (Spellings{"你", "台", "台", "台", "台", "ta", "tab", "tad", "他", "她", "它"}));
}

// ---- 异步候选插入前的去重 ----

// 简体模式不展开，列表与新候选都没有写法：只按底词判，空写法不算同一种写法。
TEST_CASE(async_candidate_in_simplified_mode_is_judged_by_its_word_only)
{
    for (const auto source : {CandidateSource::CloudSuggestion, CandidateSource::AiSuggestion})
    {
        const std::vector<WordItem> items = {WordItem("tai", "台", 9), WordItem("fu", "复", 8)};
        REQUIRE(!FanyImeIpc::ListsWordOrSpellingOf(items, WordItem("tai", "臺", 0, source)));
        REQUIRE(!FanyImeIpc::ListsWordOrSpellingOf(items, WordItem("fu", "覆", 0, source)));
        REQUIRE(FanyImeIpc::ListsWordOrSpellingOf(items, WordItem("fu", "复", 0, source)));
    }
}

// ---- 异步候选要不要插入：探针构造加判定 ----

namespace
{
FanyImeIpc::CandidateSpellingSettings Settings(bool traditional_character_set, SchemeType scheme = SchemeType::Quanpin,
                                               bool radical_candidate_list = false)
{
    return {FakeIndex(), FanyImeIpc::TraditionalRegion::Taiwan, traditional_character_set, scheme,
            radical_candidate_list};
}

const CandidateSource kAsyncSources[] = {CandidateSource::CloudSuggestion, CandidateSource::AiSuggestion};
} // namespace

// 不做简转繁的三种情形：简体字形、日语方案、U 模式拆字列表。这时新候选不填写法，只比底词。列表项填着写法
// （「台」显示「臺」），新候选「臺」若被误填了写法就会撞上它而被跳过。
TEST_CASE(async_insertion_without_conversion_compares_words_only)
{
    WordItem tai("tai", "台", 9);
    tai.output_text = "臺";
    WordItem fu("fu", "复", 8);
    fu.output_text = "復";
    const std::vector<WordItem> items = {tai, fu};
    for (const auto &settings : {Settings(false), Settings(true, SchemeType::JapaneseRomaji),
                                 Settings(true, SchemeType::Quanpin, /*radical_candidate_list=*/true)})
    {
        for (const auto source : kAsyncSources)
        {
            REQUIRE(FanyImeIpc::ShouldInsertAsyncCandidate(items, "tai", "臺", source, settings));
            REQUIRE(FanyImeIpc::ShouldInsertAsyncCandidate(items, "fu", "覆", source, settings));
            REQUIRE(!FanyImeIpc::ShouldInsertAsyncCandidate(items, "fu", "复", source, settings));
        }
    }
}

// 列表里已有上一条云或 AI 候选：新候选与它同底词，或第一写法与它显示的相同，都跳过。
TEST_CASE(async_insertion_compares_with_an_earlier_async_candidate_in_the_list)
{
    for (const auto earlier_source : kAsyncSources)
    {
        auto items = Expand({WordItem("ta", "他", 9), WordItem("ta", "她", 8)});
        items.insert(items.begin() + 1, FanyImeIpc::WithFirstTraditionalSpellingOf(
                                            WordItem("tai", "台", 0, earlier_source), Settings(true)));
        REQUIRE_EQ(items[1].output_text, std::string("臺"));
        for (const auto source : kAsyncSources)
        {
            REQUIRE(!FanyImeIpc::ShouldInsertAsyncCandidate(items, "tai", "台", source, Settings(true)));
            REQUIRE(!FanyImeIpc::ShouldInsertAsyncCandidate(items, "tai", "臺", source, Settings(true)));
            REQUIRE(FanyImeIpc::ShouldInsertAsyncCandidate(items, "fu", "覆", source, Settings(true)));
        }
    }
}

// ---- 切换字形换写法后高亮落在哪一页哪一项 ----

namespace
{
WordItem WithRank(const std::string &word, int variant_rank)
{
    WordItem item("pin", word, 0);
    item.variant_rank = variant_rank;
    return item;
}

void RequirePagePosition(const std::optional<FanyImeIpc::CandidatePagePosition> &actual, int page_index,
                         int index_in_page)
{
    REQUIRE(actual.has_value());
    REQUIRE_EQ(actual->page_index, page_index);
    REQUIRE_EQ(actual->index_in_page, index_in_page);
}
} // namespace

// 列表是 你 臺 台 檯 颱 他，「台」那组的组首在下标 1，每页 5 项时落在第 0 页第 1 项。
TEST_CASE(page_position_of_word_finds_a_word_on_the_first_page)
{
    const auto items = Expand({WordItem("ni", "你", 9), WordItem("tai", "台", 8), WordItem("ta", "他", 7)});
    REQUIRE_EQ(Words(items), (Spellings{"你", "台", "台", "台", "台", "他"}));
    REQUIRE_EQ(Ranks(items), (std::vector<int>{0, 0, 1, 2, 3, 0}));
    RequirePagePosition(FanyImeIpc::PagePositionOfWord(items, "台", 5), 0, 1);
}

// 「他」在下标 5：每页 5 项时是第 1 页第 0 项，每页 4 项时是第 1 页第 1 项；十项的列表每页 3 项时，
// 下标 7 是第 2 页第 1 项。
TEST_CASE(page_position_of_word_finds_a_word_on_a_later_page)
{
    const auto items = Expand({WordItem("ni", "你", 9), WordItem("tai", "台", 8), WordItem("ta", "他", 7)});
    REQUIRE_EQ(Words(items), (Spellings{"你", "台", "台", "台", "台", "他"}));
    REQUIRE_EQ(Ranks(items), (std::vector<int>{0, 0, 1, 2, 3, 0}));
    RequirePagePosition(FanyImeIpc::PagePositionOfWord(items, "他", 5), 1, 0);
    RequirePagePosition(FanyImeIpc::PagePositionOfWord(items, "他", 4), 1, 1);

    std::vector<WordItem> ten;
    for (const char *word : {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"})
        ten.push_back(WithRank(word, 0));
    RequirePagePosition(FanyImeIpc::PagePositionOfWord(ten, "七", 3), 2, 1);
}

// 底词在新列表里展开成一组写法时落在组首，不落在组里的其他写法上。列表里排在组首前面、底词相同而
// variant_rank 不是 0 的项也不算：「台」的组首在下标 3，每页 2 项时是第 1 页第 1 项。
TEST_CASE(page_position_of_word_lands_on_the_first_spelling_of_its_group)
{
    const std::vector<WordItem> items = {WithRank("甲", 0), WithRank("台", 1), WithRank("乙", 0),
                                         WithRank("台", 0), WithRank("台", 1), WithRank("台", 2)};
    RequirePagePosition(FanyImeIpc::PagePositionOfWord(items, "台", 2), 1, 1);
}

// 同一个底词以不同拼音出现两组时，取前面那组的组首：「着」两组在下标 1 和 3，每页 2 项时是第 0 页第 1 项。
TEST_CASE(page_position_of_word_takes_the_first_group_of_a_repeated_word)
{
    const auto items = Expand({WordItem("ni", "你", 9), WordItem("zhe", "着", 8), WordItem("zhuo", "着", 7)});
    REQUIRE_EQ(Words(items), (Spellings{"你", "着", "着", "着", "着"}));
    RequirePagePosition(FanyImeIpc::PagePositionOfWord(items, "着", 2), 0, 1);
}

TEST_CASE(page_position_of_word_is_empty_when_the_word_is_not_listed)
{
    const auto items = Expand({WordItem("ni", "你", 9), WordItem("tai", "台", 8), WordItem("ta", "他", 7)});
    REQUIRE(!FanyImeIpc::PagePositionOfWord(items, "我", 5).has_value());
}

TEST_CASE(page_position_of_word_is_empty_for_an_empty_list)
{
    REQUIRE(!FanyImeIpc::PagePositionOfWord({}, "台", 5).has_value());
}

TEST_CASE(page_position_of_word_is_empty_without_a_page_size)
{
    const std::vector<WordItem> items = {WithRank("台", 0)};
    REQUIRE(!FanyImeIpc::PagePositionOfWord(items, "台", 0).has_value());
}

// ---- 组字途中切换字形：在当前列表上收拢，再按新字形展开 ----

namespace
{
// 切到简体时不展开写法，收拢后的列表原样就是新列表。
std::vector<WordItem> NoSpellings(std::vector<WordItem> items)
{
    return items;
}

FanyImeIpc::RespelledCandidateList RespellToTraditional(std::vector<WordItem> items, size_t highlighted_index,
                                                        int page_size)
{
    return FanyImeIpc::RespellCandidateList(std::move(items), highlighted_index, page_size, Expand);
}

FanyImeIpc::RespelledCandidateList RespellToSimplified(std::vector<WordItem> items, size_t highlighted_index,
                                                       int page_size)
{
    return FanyImeIpc::RespellCandidateList(std::move(items), highlighted_index, page_size, NoSpellings);
}

void RequireHighlight(const FanyImeIpc::CandidatePagePosition &actual, int page_index, int index_in_page)
{
    REQUIRE_EQ(actual.page_index, page_index);
    REQUIRE_EQ(actual.index_in_page, index_in_page);
}

// 简体列表：「词0」到「词{count - 1}」，每项一个候选。
std::vector<WordItem> NumberedWords(size_t count)
{
    std::vector<WordItem> items;
    for (size_t number = 0; number < count; ++number)
        items.emplace_back("ci", "词" + std::to_string(number), 100);
    return items;
}
} // namespace

// 简体列表 你 hello 台 群众(云) 我的邮箱(快捷短语) 因为(AI) 他 切到繁体：「台」就地展开成四项，云、AI 候选
// 只出第一写法，英文与快捷短语原样保留，先后顺序不变。高亮的「他」从下标 6 移到下标 9，每页 4 项时是第 2 页第 1 项。
TEST_CASE(respell_to_traditional_keeps_unexpanded_cloud_and_ai_candidates_in_their_places)
{
    const std::vector<WordItem> simplified = {WordItem("ni", "你", 9),
                                              Rich("hello", CandidateSource::EnglishDictionary),
                                              Rich("台"),
                                              Rich("群众", CandidateSource::CloudSuggestion),
                                              Rich("我的邮箱", CandidateSource::QuickPhrase),
                                              Rich("因为", CandidateSource::AiSuggestion),
                                              WordItem("ta", "他", 1)};
    const auto respelled = RespellToTraditional(simplified, 6, 4);
    const auto &items = respelled.items;
    REQUIRE_EQ(Words(items), (Spellings{"你", "hello", "台", "台", "台", "台", "群众", "我的邮箱", "因为", "他"}));
    REQUIRE_EQ(OutputTexts(items), (Spellings{"你", "", "臺", "台", "檯", "颱", "群眾", "", "因為", "他"}));
    REQUIRE_EQ(Ranks(items), (std::vector<int>{0, 0, 0, 1, 2, 3, 0, 0, 0, 0}));
    RequireUnchanged(items[1], simplified[1]);
    RequireUnchanged(items[7], simplified[4]);
    RequireSameExceptVariant(items[6], simplified[3]);
    RequireSameExceptVariant(items[8], simplified[5]);
    RequireHighlight(respelled.highlight, 2, 1);
}

// 繁体列表切回简体：每个写法组收成一项、写法清空，每一项与展开前的候选逐字段相同，
// 英文、云、AI 候选留在原来的先后位置上。
TEST_CASE(respell_to_simplified_collapses_each_spelling_group_to_one_item_without_a_spelling)
{
    const std::vector<WordItem> simplified = {Rich("台"),
                                              Rich("hello", CandidateSource::EnglishDictionary),
                                              Rich("群众", CandidateSource::CloudSuggestion),
                                              Rich("因为", CandidateSource::AiSuggestion),
                                              Rich("台湾", CandidateSource::UserDatabase),
                                              Rich("着")};
    const auto traditional = Expand(simplified);
    REQUIRE_EQ(Words(traditional),
               (Spellings{"台", "台", "台", "台", "hello", "群众", "因为", "台湾", "台湾", "着", "着"}));
    REQUIRE_EQ(OutputTexts(traditional),
               (Spellings{"臺", "台", "檯", "颱", "", "群眾", "因為", "臺灣", "台灣", "著", "着"}));
    REQUIRE_EQ(Ranks(traditional), (std::vector<int>{0, 1, 2, 3, 0, 0, 0, 0, 1, 0, 1}));
    const auto respelled = RespellToSimplified(traditional, 0, 5);
    REQUIRE_EQ(respelled.items.size(), simplified.size());
    for (size_t index = 0; index < simplified.size(); ++index)
        RequireUnchanged(respelled.items[index], simplified[index]);
}

// 繁体列表 你 臺 台 檯 颱 他，高亮在「台」组的第 3、第 4 项上：切回简体后都落在「台」上，
// 即列表 你 台 他 的下标 1，每页 2 项时是第 0 页第 1 项。
TEST_CASE(respell_puts_a_highlight_inside_a_spelling_group_on_the_word_of_that_group)
{
    const auto traditional = Expand({WordItem("ni", "你", 9), WordItem("tai", "台", 8), WordItem("ta", "他", 7)});
    REQUIRE_EQ(OutputTexts(traditional), (Spellings{"你", "臺", "台", "檯", "颱", "他"}));
    for (const size_t highlighted : {size_t{3}, size_t{4}})
    {
        const auto respelled = RespellToSimplified(traditional, highlighted, 2);
        REQUIRE_EQ(Words(respelled.items), (Spellings{"你", "台", "他"}));
        RequireHighlight(respelled.highlight, 0, 1);
    }
}

// 高亮在云候选上切到繁体：台 群众(云) 他 展开成 臺 台 檯 颱 群眾 他，「群众」在下标 4，每页 3 项时是第 1 页第 1 项。
TEST_CASE(respell_to_traditional_follows_a_highlighted_cloud_candidate)
{
    const auto respelled = RespellToTraditional({WordItem("tai", "台", 9),
                                                 WordItem("qun'zhong", "群众", 8, CandidateSource::CloudSuggestion),
                                                 WordItem("ta", "他", 7)},
                                                1, 3);
    REQUIRE_EQ(OutputTexts(respelled.items), (Spellings{"臺", "台", "檯", "颱", "群眾", "他"}));
    RequireHighlight(respelled.highlight, 1, 1);
}

// 高亮在 AI 候选上切回简体：臺 台 檯 颱 為 爲 因為(AI) 他 里「因为」在下标 6（第 2 页第 0 项），
// 收拢成 台 为 因为 他 后在下标 2，每页 3 项时是第 0 页第 2 项。
TEST_CASE(respell_to_simplified_follows_a_highlighted_ai_candidate)
{
    const auto traditional =
        Expand({WordItem("tai", "台", 9), WordItem("wei", "为", 8),
                WordItem("yin'wei", "因为", 7, CandidateSource::AiSuggestion), WordItem("ta", "他", 6)});
    REQUIRE_EQ(OutputTexts(traditional), (Spellings{"臺", "台", "檯", "颱", "為", "爲", "因為", "他"}));
    const auto respelled = RespellToSimplified(traditional, 6, 3);
    REQUIRE_EQ(Words(respelled.items), (Spellings{"台", "为", "因为", "他"}));
    RequireHighlight(respelled.highlight, 0, 2);
}

// 30 项的简体列表、高亮在第 28 项「词27」（每页 8 项时第 3 页第 3 项）时切到繁体：30 项全部留下，
// 排在前面的「台」展开多出 3 项，「词27」移到下标 30，是第 3 页第 6 项。再切回简体，回到原来的 30 项与第 3 页第 3 项。
TEST_CASE(respell_moves_the_highlight_across_pages_with_its_word_and_restores_the_list_on_the_way_back)
{
    auto simplified = NumberedWords(30);
    simplified[0] = WordItem("tai", "台", 200);
    const auto traditional = RespellToTraditional(simplified, 27, 8);
    REQUIRE_EQ(traditional.items.size(), size_t{33});
    REQUIRE_EQ(traditional.items[30].word, std::string("词27"));
    REQUIRE_EQ(traditional.items.back().word, std::string("词29"));
    RequireHighlight(traditional.highlight, 3, 6);

    const auto back = RespellToSimplified(traditional.items, 30, 8);
    REQUIRE_EQ(Words(back.items), Words(simplified));
    RequireHighlight(back.highlight, 3, 3);
}

// 「余」「馀」同在列表时，繁体下「餘」归「余」，「馀」整项不出现。列表 余 他 馀、高亮在「馀」上（每页 1 项时第 2 页）
// 切到繁体，新列表 余 余 他 里没有「馀」，高亮回到第 0 页第 0 项，不留在下标 2 的「他」上。
TEST_CASE(respell_returns_to_the_first_item_when_the_highlighted_word_is_no_longer_listed)
{
    const auto respelled =
        RespellToTraditional({WordItem("yu", "余", 9), WordItem("ta", "他", 8), WordItem("yu", "馀", 7)}, 2, 1);
    REQUIRE_EQ(Words(respelled.items), (Spellings{"余", "余", "他"}));
    RequireHighlight(respelled.highlight, 0, 0);
}

// 高亮下标越界、或每页项数不大于 0 时，高亮落在第 0 页第 0 项，列表照样按新字形展开。
TEST_CASE(respell_puts_an_unplaceable_highlight_on_the_first_item)
{
    const std::vector<WordItem> simplified = {WordItem("ta", "他", 9), WordItem("tai", "台", 8)};
    const auto out_of_range = RespellToTraditional(simplified, 2, 1);
    REQUIRE_EQ(OutputTexts(out_of_range.items), (Spellings{"他", "臺", "台", "檯", "颱"}));
    RequireHighlight(out_of_range.highlight, 0, 0);

    const auto no_page_size = RespellToTraditional(simplified, 1, 0);
    REQUIRE_EQ(OutputTexts(no_page_size.items), (Spellings{"他", "臺", "台", "檯", "颱"}));
    RequireHighlight(no_page_size.highlight, 0, 0);
}
