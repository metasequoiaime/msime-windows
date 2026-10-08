#include "ipc/candidate_expansion_policy.h"
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
    RequireExpressiveTail(ui.items);
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
