#include "ipc/candidate_expansion_policy.h"
#include "ipc/traditional_variant_expansion_policy.h"
#include "tests/includes/fake_traditional_spelling_index.h"
#include "tests/includes/test_framework.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
std::vector<WordItem> LocalCandidates(int count)
{
    std::vector<WordItem> items;
    for (int index = 0; index < count; ++index)
        items.emplace_back("ni", "local-" + std::to_string(index), count - index);
    return items;
}

std::vector<WordItem> MixedCandidates()
{
    return {WordItem("ni", "Ni", 100, CandidateSource::EnglishDictionary),
            WordItem("ninja", "Ninja", 50, CandidateSource::EnglishDictionary),
            WordItem("ni", "😀", 0, CandidateSource::Emoji),
            WordItem("ni", "😁", 0, CandidateSource::Emoji),
            WordItem("ni", "(^_^)", 0, CandidateSource::Kaomoji),
            WordItem("ni", "(T_T)", 0, CandidateSource::Kaomoji),
            WordItem("date:ymd_dash", "2026-10-08", 0, CandidateSource::DateTime),
            metasequoia::local_modes::date_time_menu_item("rq"),
            WordItem("ni", "cloud", 0, CandidateSource::CloudSuggestion),
            WordItem("ni", "ai", 0, CandidateSource::AiSuggestion)};
}

void RequireExpressiveTail(const std::vector<WordItem> &items)
{
    REQUIRE(items.size() >= 4);
    const size_t start = items.size() - 4;
    REQUIRE_EQ(items[start].word, std::string("😀"));
    REQUIRE_EQ(items[start + 1].word, std::string("😁"));
    REQUIRE_EQ(items[start + 2].word, std::string("(^_^)"));
    REQUIRE_EQ(items[start + 3].word, std::string("(T_T)"));
}
} // namespace

TEST_CASE(candidate_expansion_preserves_mixed_results_before_entering_a_partial_last_page)
{
    Global::CandidateUiState ui;
    ui.page_size = 8;
    auto initial = LocalCandidates(24);
    const auto mixed = MixedCandidates();
    initial.insert(initial.end(), mixed.begin(), mixed.end());
    FanyImeIpc::NormalizeMixedCandidateOrder(initial);
    ui.set_items(std::move(initial));
    ui.page_index = 3;
    ui.selected_index_in_page = 2;
    REQUIRE(ui.is_next_page_partial_last_page());

    // This is the replacement step used by ExpandCandidatesKeepingPagePosition after the
    // session has loaded its next batch. The old set_items(expanded) loses every mixed result.
    FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(ui, LocalCandidates(60));
    REQUIRE_EQ(ui.page_index, 3);
    REQUIRE_EQ(ui.selected_index_in_page, 2);
    REQUIRE_EQ(ui.item_total_count, 70);
    REQUIRE_EQ(ui.items[0].word, std::string("local-0"));
    REQUIRE_EQ(ui.items[1].source, CandidateSource::DateTime);
    REQUIRE(metasequoia::local_modes::is_date_time_menu_item(ui.items[2]));
    REQUIRE_EQ(ui.items[3].source, CandidateSource::CloudSuggestion);
    REQUIRE_EQ(ui.items[4].source, CandidateSource::AiSuggestion);
    REQUIRE_EQ(ui.items[5].source, CandidateSource::EnglishDictionary);
    RequireExpressiveTail(ui.items);

    // The final page remains reachable and still contains Emoji followed by kaomoji.
    while (ui.has_next_page())
        ++ui.page_index;
    REQUIRE_EQ(ui.page_index, 8);
    REQUIRE_EQ(ui.current_page_count(), 6);
    REQUIRE_EQ(ui.items[ui.item_total_count - 1].source, CandidateSource::Kaomoji);
}

TEST_CASE(candidate_expansion_refills_the_current_last_page_and_keeps_learned_english_placement)
{
    Global::CandidateUiState ui;
    ui.page_size = 8;
    auto initial = LocalCandidates(24);
    const auto mixed = MixedCandidates();
    initial.insert(initial.end(), mixed.begin(), mixed.end());
    ui.set_items(std::move(initial));
    ui.page_index = 4;
    ui.selected_index_in_page = 1;
    REQUIRE(!ui.has_next_page());
    REQUIRE(!ui.is_current_page_full());
    FanyImeIpc::EnglishPlacement placement;
    placement.slot = 6;
    placement.input = "ni";
    placement.page_size = 8;

    FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(ui, LocalCandidates(60), placement);
    REQUIRE_EQ(ui.page_index, 4);
    REQUIRE_EQ(ui.selected_index_in_page, 1);
    REQUIRE(ui.is_current_page_full());
    const auto english_index = FanyImeIpc::SlottedEnglishIndex(ui.items);
    REQUIRE(english_index.has_value());
    REQUIRE_EQ(*english_index, size_t{6});
    // 知道每页候选数时，最匹配的 emoji 展开后仍在首页末位，其余 emoji 和颜文字在末尾。
    REQUIRE_EQ(ui.items[7].word, std::string("😀"));
    const size_t start = ui.items.size() - 3;
    REQUIRE_EQ(ui.items[start].word, std::string("😁"));
    REQUIRE_EQ(ui.items[start + 1].word, std::string("(^_^)"));
    REQUIRE_EQ(ui.items[start + 2].word, std::string("(T_T)"));
}

TEST_CASE(candidate_expansion_deduplicates_results_and_preserves_fixed_english_metadata)
{
    Global::CandidateUiState ui;
    auto initial = LocalCandidates(24);
    auto mixed = MixedCandidates();
    mixed.front().fixed_position = 1;
    mixed.front().corrected_from = "metadata";
    initial.insert(initial.end(), mixed.begin(), mixed.end());
    ui.set_items(std::move(initial));

    auto expanded = LocalCandidates(60);
    expanded.push_back(mixed[1]);
    expanded.emplace_back("ni", "cloud", 1);
    expanded.emplace_back("ni", "local-quick-phrase", 0, CandidateSource::QuickPhrase);
    FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(ui, expanded);
    REQUIRE_EQ(ui.item_total_count, 71);
    REQUIRE_EQ(ui.items[0].word, std::string("Ni"));
    REQUIRE_EQ(ui.items[0].fixed_position, 1);
    REQUIRE_EQ(ui.items[0].corrected_from, std::string("metadata"));
    REQUIRE_EQ(
        std::count_if(ui.items.begin(), ui.items.end(), [](const WordItem &item) { return item.word == "cloud"; }), 1);
    RequireExpressiveTail(ui.items);

    FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(ui, std::move(expanded));
    REQUIRE_EQ(ui.item_total_count, 71);
    RequireExpressiveTail(ui.items);
}

TEST_CASE(candidate_expansion_rebuilds_local_candidates_without_carrying_stale_fallbacks_or_phrases)
{
    Global::CandidateUiState ui;
    ui.set_items({WordItem("ni", "old-local", 1), WordItem("ni", "old-fallback", 0, CandidateSource::Fallback),
                  WordItem("ni", "old-phrase", 0, CandidateSource::QuickPhrase)});
    auto expanded = LocalCandidates(60);
    FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(ui, expanded);
    REQUIRE_EQ(ui.item_total_count, 60);
    for (size_t index = 0; index < expanded.size(); ++index)
        REQUIRE_EQ(ui.items[index].word, expanded[index].word);
}

TEST_CASE(candidate_expansion_keeps_every_visible_position_after_selection_started)
{
    Global::CandidateUiState ui;
    ui.page_size = 3;
    auto initial = LocalCandidates(5);
    initial.insert(initial.begin() + 1, WordItem("ni", "cloud", 0, CandidateSource::CloudSuggestion));
    ui.set_items(initial);
    ui.page_index = 1;
    ui.selected_index_in_page = 1;
    ui.begin_selection();

    // 新的引擎结果顺序已经变化，且带有重复的云候选；扩展只能在原列表后追加新词。
    auto expanded = LocalCandidates(10);
    std::reverse(expanded.begin(), expanded.end());
    expanded.push_back(initial[1]);
    REQUIRE(FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(ui, std::move(expanded)));
    REQUIRE(ui.selection_started);
    REQUIRE_EQ(ui.page_index, 1);
    REQUIRE_EQ(ui.selected_index_in_page, 1);
    REQUIRE_EQ(ui.item_total_count, 11);
    for (size_t index = 0; index < initial.size(); ++index)
    {
        REQUIRE_EQ(ui.items[index].word, initial[index].word);
        REQUIRE(ui.items[index].source == initial[index].source);
    }
    REQUIRE_EQ(ui.items[6].word, std::string("local-9"));
    REQUIRE_EQ(ui.items.back().word, std::string("local-5"));
    REQUIRE(ui.has_next_page());
}

TEST_CASE(candidate_expansion_after_selection_keeps_same_word_with_other_pinyin_or_source)
{
    Global::CandidateUiState ui;
    ui.page_size = 3;
    ui.set_items({WordItem("hang", "行", 2), WordItem("ni", "cloud", 0, CandidateSource::CloudSuggestion)});
    ui.begin_selection();

    // 多音字和快捷短语与已显示的同字，却是另外的候选；与界面独有的云候选同字的本地候选仍要挡掉。
    std::vector<WordItem> expanded{WordItem("hang", "行", 2), WordItem("xing", "行", 1),
                                   WordItem("hang", "行", 0, CandidateSource::QuickPhrase), WordItem("ni", "cloud", 0)};
    REQUIRE(FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(ui, std::move(expanded)));
    REQUIRE_EQ(ui.item_total_count, 4);
    REQUIRE_EQ(ui.items[2].pinyin, std::string("xing"));
    REQUIRE(ui.items[3].source == CandidateSource::QuickPhrase);
}

TEST_CASE(candidate_expansion_after_selection_reports_no_change_when_nothing_new)
{
    Global::CandidateUiState ui;
    ui.page_size = 3;
    ui.set_items(LocalCandidates(4));
    ui.begin_selection();

    auto expanded = LocalCandidates(4);
    std::reverse(expanded.begin(), expanded.end());
    REQUIRE(!FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(ui, std::move(expanded)));
    REQUIRE_EQ(ui.item_total_count, 4);
    REQUIRE_EQ(ui.items.front().word, std::string("local-0"));
}

TEST_CASE(candidate_expansion_after_selection_tells_spellings_of_the_same_word_apart)
{
    const auto spelling = [](const char *output_text, int variant_rank) {
        WordItem item("tai", "台", 9);
        item.output_text = output_text;
        item.variant_rank = variant_rank;
        return item;
    };
    Global::CandidateUiState ui;
    ui.page_size = 3;
    ui.set_items({spelling("臺", 0), spelling("台", 1)});
    ui.begin_selection();

    // 同一底词的写法词、拼音、来源都相同，只有写法不同：已显示的「台」抵掉，没显示过的「檯」要追加。
    std::vector<WordItem> expanded{spelling("台", 1), spelling("檯", 2)};
    REQUIRE(FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(ui, std::move(expanded)));
    REQUIRE_EQ(ui.item_total_count, 3);
    REQUIRE_EQ(ui.items[0].output_text, std::string("臺"));
    REQUIRE_EQ(ui.items[1].output_text, std::string("台"));
    REQUIRE_EQ(ui.items[2].output_text, std::string("檯"));
}

namespace
{
// 照 OpenCC 数据造的假索引：「复」出 復 複（字表里的「覆」是独立的简体字，索引不列在「复」下），「覆」只出它自己；
// 「着」的台湾写法是「著」，「著」只出它自己；别的字各出一种写法。底词都是简体写法。
const test::FakeTraditionalSpellingIndex &FakeIndex()
{
    static const test::FakeTraditionalSpellingIndex index({
        {"复", {"復", "複"}},
        {"覆", {"覆"}},
        {"着", {"著", "着"}},
        {"著", {"著"}},
    });
    return index;
}

std::vector<WordItem> Expand(std::vector<WordItem> items)
{
    return FanyImeIpc::ExpandTraditionalVariants(std::move(items), FakeIndex(), FanyImeIpc::TraditionalRegion::Taiwan);
}

// 只敲首字母时引擎给的候选：first 排在前面，后面跟 filler_count 个别的字，later 排在 position_of_later 处
// （超出范围就不放）。别的字没有写法表条目，各自只出「字N」一种写法。
std::vector<WordItem> InitialCandidates(const std::string &first, const std::string &later, size_t filler_count,
                                        size_t position_of_later)
{
    std::vector<WordItem> items{WordItem("f", first, 1000)};
    for (size_t index = 0; index < filler_count; ++index)
    {
        if (items.size() == position_of_later)
            items.emplace_back("f", later, 900);
        items.emplace_back("f", "字" + std::to_string(index), 500 - static_cast<int>(index));
    }
    return items;
}

size_t CountShownAs(const std::vector<WordItem> &items, const std::string &text)
{
    return static_cast<size_t>(
        std::count_if(items.begin(), items.end(), [&](const WordItem &item) { return item.output_text == text; }));
}
} // namespace

// 台湾地区「着」组是 著 着，被收回的「著」是组首：第一批只有「着」，「着」组显示「著」；整份候选里底词「著」在后面，
// 重新展开后「著」改归底词「著」。已显示的项不撤，追加批里同写法、不同底词的项丢掉，列表里只有一个「著」。
TEST_CASE(candidate_expansion_after_selection_keeps_each_spelling_with_the_word_already_shown)
{
    constexpr size_t kInitialCount = 24;
    constexpr size_t kFullFillerCount = 40;
    constexpr size_t kPositionOfZhu = 30;
    Global::CandidateUiState ui;
    ui.page_size = 9;
    ui.set_items(Expand(InitialCandidates("着", "著", kInitialCount - 1, kPositionOfZhu)));
    REQUIRE_EQ(ui.items.size(), kInitialCount + 1);
    REQUIRE_EQ(ui.items[0].output_text, std::string("著"));
    REQUIRE_EQ(ui.items[0].word, std::string("着"));
    const auto shown = ui.items;
    ui.begin_selection();

    auto full = Expand(InitialCandidates("着", "著", kFullFillerCount, kPositionOfZhu));
    REQUIRE_EQ(CountShownAs(full, "著"), size_t{1});
    REQUIRE(std::any_of(full.begin(), full.end(), [](const WordItem &item) { return item.word == "著"; }));
    REQUIRE(FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(ui, std::move(full)));

    REQUIRE_EQ(CountShownAs(ui.items, "著"), size_t{1});
    REQUIRE_EQ(ui.items[0].output_text, std::string("著"));
    REQUIRE_EQ(ui.items[0].word, std::string("着"));
    for (size_t index = 0; index < shown.size(); ++index)
    {
        REQUIRE_EQ(ui.items[index].word, shown[index].word);
        REQUIRE_EQ(ui.items[index].output_text, shown[index].output_text);
    }
    // 追加的是没显示过的 字23 … 字39；底词「著」唯一的写法已经在列表里，它不出现。
    REQUIRE_EQ(ui.item_total_count, static_cast<int>(shown.size() + (kFullFillerCount - (kInitialCount - 1))));
    REQUIRE_EQ(ui.items.back().output_text, std::string("字39"));
    REQUIRE(std::none_of(ui.items.begin(), ui.items.end(), [](const WordItem &item) { return item.word == "著"; }));
}

// 香港地区「着」组是 着 著，被追加批收回的「著」在已显示组里排第 2、是组尾：已显示的每一种写法都算已占，
// 不只组首。追加批的底词「著」照样丢掉，列表里只有一个「著」，「着」组原样不动。
TEST_CASE(candidate_expansion_after_selection_keeps_a_shown_spelling_that_is_not_first_in_its_group)
{
    const test::FakeTraditionalSpellingIndex hong_kong_order({{"着", {"着", "著"}}, {"著", {"著"}}});
    const auto expand = [&](std::vector<WordItem> items) {
        return FanyImeIpc::ExpandTraditionalVariants(std::move(items), hong_kong_order,
                                                     FanyImeIpc::TraditionalRegion::Taiwan);
    };
    constexpr size_t kInitialCount = 24;
    constexpr size_t kFullFillerCount = 40;
    constexpr size_t kPositionOfZhu = 30;
    Global::CandidateUiState ui;
    ui.page_size = 9;
    ui.set_items(expand(InitialCandidates("着", "著", kInitialCount - 1, kPositionOfZhu)));
    REQUIRE_EQ(ui.items[0].output_text, std::string("着"));
    REQUIRE_EQ(ui.items[1].output_text, std::string("著"));
    REQUIRE_EQ(ui.items[1].word, std::string("着"));
    REQUIRE_EQ(ui.items[1].variant_rank, 1);
    const auto shown = ui.items;
    ui.begin_selection();

    REQUIRE(FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(
        ui, expand(InitialCandidates("着", "著", kFullFillerCount, kPositionOfZhu))));
    for (size_t index = 0; index < shown.size(); ++index)
    {
        REQUIRE_EQ(ui.items[index].word, shown[index].word);
        REQUIRE_EQ(ui.items[index].output_text, shown[index].output_text);
        REQUIRE_EQ(ui.items[index].variant_rank, shown[index].variant_rank);
    }
    REQUIRE_EQ(CountShownAs(ui.items, "著"), size_t{1});
    REQUIRE(std::none_of(ui.items.begin(), ui.items.end(), [](const WordItem &item) { return item.word == "著"; }));
    REQUIRE_EQ(ui.item_total_count, static_cast<int>(shown.size() + (kFullFillerCount - (kInitialCount - 1))));
}

// 第一批只有「锺」：「鍾」只有它一个简体底词展开，归「锺」。整份候选里首选底词「钟」也在，重新展开后「鍾」归「钟」，
// 但「鍾」已显示在「锺」下，追加批里「钟」的组首「鍾」被丢掉。「钟」剩下的 鐘 鈡 紧挨着、从 0 起重新编号，
// 组首「鐘」是它此刻的第一写法：每个底词只留第一写法时拿得到「钟」，按组找组首不会把它分进前一组，
// 收拢再展开后「钟」仍在。
TEST_CASE(candidate_expansion_after_selection_renumbers_a_group_whose_first_spelling_was_already_shown)
{
    const test::FakeTraditionalSpellingIndex index({{"钟", {"鍾", "鐘", "鈡"}}, {"锺", {"鍾"}}}, {}, {{"鍾", "钟"}});
    const auto expand = [&](std::vector<WordItem> items) {
        return FanyImeIpc::ExpandTraditionalVariants(std::move(items), index, FanyImeIpc::TraditionalRegion::Taiwan);
    };
    constexpr size_t kInitialCount = 24;
    constexpr size_t kFullFillerCount = 40;
    constexpr size_t kPositionOfZhong = 30;
    Global::CandidateUiState ui;
    ui.page_size = 9;
    ui.set_items(expand(InitialCandidates("锺", "钟", kInitialCount - 1, kPositionOfZhong)));
    REQUIRE_EQ(ui.items[0].word, std::string("锺"));
    REQUIRE_EQ(ui.items[0].output_text, std::string("鍾"));
    const auto shown = ui.items;
    ui.begin_selection();

    REQUIRE(FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(
        ui, expand(InitialCandidates("锺", "钟", kFullFillerCount, kPositionOfZhong))));
    for (size_t index = 0; index < shown.size(); ++index)
    {
        REQUIRE_EQ(ui.items[index].word, shown[index].word);
        REQUIRE_EQ(ui.items[index].output_text, shown[index].output_text);
        REQUIRE_EQ(ui.items[index].variant_rank, shown[index].variant_rank);
    }
    REQUIRE_EQ(CountShownAs(ui.items, "鍾"), size_t{1});

    const auto head =
        std::find_if(ui.items.begin(), ui.items.end(), [](const WordItem &item) { return item.word == "钟"; });
    REQUIRE(head != ui.items.end());
    const size_t head_index = static_cast<size_t>(head - ui.items.begin());
    REQUIRE(head_index + 1 < ui.items.size());
    REQUIRE_EQ(ui.items[head_index].output_text, std::string("鐘"));
    REQUIRE_EQ(ui.items[head_index].variant_rank, 0);
    REQUIRE_EQ(ui.items[head_index + 1].word, std::string("钟"));
    REQUIRE_EQ(ui.items[head_index + 1].output_text, std::string("鈡"));
    REQUIRE_EQ(ui.items[head_index + 1].variant_rank, 1);
    REQUIRE_EQ(std::count_if(ui.items.begin(), ui.items.end(), [](const WordItem &item) { return item.word == "钟"; }),
               std::ptrdiff_t{2});

    const auto first = FanyImeIpc::FirstVariantsOnly(ui.items);
    const auto first_zhong =
        std::find_if(first.begin(), first.end(), [](const WordItem &item) { return item.word == "钟"; });
    REQUIRE(first_zhong != first.end());
    REQUIRE_EQ(first_zhong->output_text, std::string("鐘"));

    REQUIRE_EQ(FanyImeIpc::SpellingGroupStart(ui.items, head_index + 1), head_index);
    REQUIRE_EQ(FanyImeIpc::SpellingGroupStart(ui.items, head_index), head_index);

    const auto reexpanded = FanyImeIpc::ReorderAsOneItemPerWord(ui.items, [](std::vector<WordItem> &) {}, expand);
    REQUIRE(std::any_of(reexpanded.begin(), reexpanded.end(), [](const WordItem &item) { return item.word == "钟"; }));
    REQUIRE_EQ(CountShownAs(reexpanded, "鍾"), size_t{1});
    REQUIRE_EQ(CountShownAs(reexpanded, "鐘"), size_t{1});
    REQUIRE_EQ(CountShownAs(reexpanded, "鈡"), size_t{1});
}

// 同一底词以两个读音相邻追加时是两组，各自从 0 起编号。第一批只有「锺」，「鍾」显示在它下面；整份候选里
// 「钟」以 zhong、chong 两个读音紧挨着出现，两组的组首「鍾」都被丢掉，各剩 鐘 鈡。每个底词只留第一写法时
// 两个读音都在，收拢再展开后两个读音也都在。
TEST_CASE(candidate_expansion_after_selection_renumbers_two_adjacent_groups_of_the_same_word)
{
    const test::FakeTraditionalSpellingIndex index({{"钟", {"鍾", "鐘", "鈡"}}, {"锺", {"鍾"}}}, {}, {{"鍾", "钟"}});
    const auto expand = [&](std::vector<WordItem> items) {
        return FanyImeIpc::ExpandTraditionalVariants(std::move(items), index, FanyImeIpc::TraditionalRegion::Taiwan);
    };
    Global::CandidateUiState ui;
    ui.page_size = 9;
    ui.set_items(expand({WordItem("z", "锺", 9), WordItem("z", "字0", 8)}));
    REQUIRE_EQ(ui.items[0].output_text, std::string("鍾"));
    const auto shown = ui.items;
    ui.begin_selection();

    REQUIRE(FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(
        ui, expand({WordItem("z", "锺", 9), WordItem("z", "字0", 8), WordItem("zhong", "钟", 7),
                    WordItem("chong", "钟", 6), WordItem("z", "字1", 5)})));
    REQUIRE_EQ(ui.items.size(), shown.size() + 5);
    struct Appended
    {
        const char *pinyin;
        const char *spelling;
        int rank;
    };
    const Appended appended[] = {
        {"zhong", "鐘", 0}, {"zhong", "鈡", 1}, {"chong", "鐘", 0}, {"chong", "鈡", 1}, {"z", "字1", 0}};
    for (size_t index = 0; index < std::size(appended); ++index)
    {
        const auto &item = ui.items[shown.size() + index];
        REQUIRE_EQ(item.pinyin, std::string(appended[index].pinyin));
        REQUIRE_EQ(item.output_text, std::string(appended[index].spelling));
        REQUIRE_EQ(item.variant_rank, appended[index].rank);
    }

    const auto readings_of_zhong = [](const std::vector<WordItem> &items) {
        std::vector<std::string> readings;
        for (const auto &item : items)
            if (item.word == "钟" && item.variant_rank == 0)
                readings.push_back(item.pinyin);
        return readings;
    };
    const std::vector<std::string> both = {"zhong", "chong"};
    REQUIRE_EQ(readings_of_zhong(FanyImeIpc::FirstVariantsOnly(ui.items)), both);
    REQUIRE_EQ(
        readings_of_zhong(FanyImeIpc::ReorderAsOneItemPerWord(ui.items, [](std::vector<WordItem> &) {}, expand)), both);
}

// 显示文字相同、底词也相同的项（同一底词的另一个读音）是另一个候选，照样追加。
TEST_CASE(candidate_expansion_after_selection_appends_the_same_spelling_of_the_same_word_with_another_pinyin)
{
    const auto spelling = [](const char *pinyin, const char *output_text) {
        WordItem item(pinyin, "行", 9);
        item.output_text = output_text;
        return item;
    };
    Global::CandidateUiState ui;
    ui.page_size = 3;
    ui.set_items({spelling("hang", "行")});
    ui.begin_selection();

    std::vector<WordItem> expanded{spelling("hang", "行"), spelling("xing", "行")};
    REQUIRE(FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(ui, std::move(expanded)));
    REQUIRE_EQ(ui.item_total_count, 2);
    REQUIRE_EQ(ui.items[1].pinyin, std::string("xing"));
    REQUIRE_EQ(ui.items[1].output_text, std::string("行"));
}
