#include "global/globals.h"
#include "tests/includes/test_framework.h"

namespace
{
Global::CandidateUiState MakeCandidateUiState(int item_count, int page_size)
{
    Global::CandidateUiState state;
    state.item_total_count = item_count;
    state.page_size = page_size;
    return state;
}
} // namespace

TEST_CASE(candidate_ui_selection_moves_from_page_end_to_next_page_start)
{
    auto state = MakeCandidateUiState(8, 3);
    state.page_index = 0;
    state.selected_index_in_page = 2;

    REQUIRE(state.move_selection(1));
    REQUIRE_EQ(state.page_index, 1);
    REQUIRE_EQ(state.selected_index_in_page, 0);
}

TEST_CASE(candidate_ui_expanded_selection_keeps_its_anchor_until_the_panel_collapses)
{
    auto state = MakeCandidateUiState(30, 3);
    state.page_index = 2;
    state.expanded_count = 12;
    state.selected_index_in_page = 1;
    REQUIRE(state.move_selection(7));
    REQUIRE_EQ(state.page_index, 2);
    REQUIRE_EQ(state.selected_index_in_page, 8);
    REQUIRE_EQ(state.current_page_start() + state.selected_index_in_page, 14);
    REQUIRE(state.selection_started);
    state.collapse_expanded_selection();
    REQUIRE_EQ(state.expanded_count, 0);
    REQUIRE_EQ(state.page_index, 4);
    REQUIRE_EQ(state.selected_index_in_page, 2);
    REQUIRE_EQ(state.current_page_start() + state.selected_index_in_page, 14);
}

TEST_CASE(candidate_ui_expanded_selection_stops_at_the_loaded_grid_boundaries)
{
    auto state = MakeCandidateUiState(30, 3);
    state.expanded_count = 7;
    REQUIRE(!state.move_selection(-1));
    REQUIRE(state.move_selection(6));
    REQUIRE(!state.move_selection(1));
    REQUIRE_EQ(state.page_index, 0);
    REQUIRE_EQ(state.selected_index_in_page, 6);
    state.set_items({WordItem("hao", "好", 1)});
    REQUIRE_EQ(state.expanded_count, 0);
    REQUIRE_EQ(state.selected_index_in_page, 0);
}

TEST_CASE(candidate_ui_selection_moves_from_page_start_to_previous_page_end)
{
    auto state = MakeCandidateUiState(8, 3);
    state.page_index = 1;
    state.selected_index_in_page = 0;

    REQUIRE(state.move_selection(-1));
    REQUIRE_EQ(state.page_index, 0);
    REQUIRE_EQ(state.selected_index_in_page, 2);
}

TEST_CASE(candidate_ui_selection_stops_at_first_and_last_candidate)
{
    auto state = MakeCandidateUiState(8, 3);
    REQUIRE(!state.move_selection(-1));
    REQUIRE_EQ(state.page_index, 0);
    REQUIRE_EQ(state.selected_index_in_page, 0);

    state.page_index = 2;
    state.selected_index_in_page = 1;
    REQUIRE(!state.move_selection(1));
    REQUIRE_EQ(state.page_index, 2);
    REQUIRE_EQ(state.selected_index_in_page, 1);
}

TEST_CASE(candidate_ui_selection_stays_started_until_items_are_rebuilt)
{
    auto state = MakeCandidateUiState(8, 3);
    REQUIRE(!state.selection_started);
    REQUIRE(state.move_selection(1));
    REQUIRE(state.selection_started);
    state.clear_page();
    state.select_first_on_page();
    REQUIRE(state.selection_started);
    // 到达边界也已经表达了选词意图，异步结果不能趁这次按键没有移动而重排。
    REQUIRE(!state.move_selection(-1));
    REQUIRE(state.selection_started);
    state.set_items({WordItem("hao", "好", 1)});
    REQUIRE(!state.selection_started);
    state.begin_selection();
    REQUIRE(state.selection_started);
    state.set_items({});
    state.begin_selection();
    REQUIRE(!state.selection_started);
}

TEST_CASE(candidate_ui_detects_only_the_last_loaded_candidate_as_exhausted)
{
    auto state = MakeCandidateUiState(24, 6);

    state.page_index = 0;
    state.selected_index_in_page = 5;
    REQUIRE(!state.is_selection_at_last_candidate());
    REQUIRE(state.has_next_page());

    state.page_index = 3;
    state.selected_index_in_page = 4;
    REQUIRE(!state.is_selection_at_last_candidate());
    REQUIRE(!state.has_next_page());

    state.selected_index_in_page = 5;
    REQUIRE(state.is_selection_at_last_candidate());
}

TEST_CASE(candidate_ui_detects_last_candidate_on_a_partial_page)
{
    auto state = MakeCandidateUiState(14, 6);
    state.page_index = 2;
    state.selected_index_in_page = 1;

    REQUIRE(state.is_selection_at_last_candidate());
}

TEST_CASE(candidate_ui_reports_whether_the_current_page_is_full)
{
    auto full = MakeCandidateUiState(24, 6);
    full.page_index = 3;
    REQUIRE(full.is_current_page_full());

    auto partial = MakeCandidateUiState(26, 6);
    partial.page_index = 4;
    REQUIRE(!partial.is_current_page_full());
}

TEST_CASE(candidate_ui_detects_entry_into_a_partial_last_page)
{
    auto partial = MakeCandidateUiState(26, 6);
    partial.page_index = 3;
    partial.selected_index_in_page = 5;
    REQUIRE(partial.is_next_page_partial_last_page());
    REQUIRE(partial.is_selection_at_current_page_end());

    partial.page_index = 2;
    REQUIRE(!partial.is_next_page_partial_last_page());

    auto full = MakeCandidateUiState(24, 6);
    full.page_index = 2;
    full.selected_index_in_page = 5;
    REQUIRE(!full.is_next_page_partial_last_page());
    REQUIRE(full.is_selection_at_current_page_end());
}

TEST_CASE(candidate_page_snapshot_survives_a_rebuild_of_the_live_state)
{
    Global::ClearCandidatePageSnapshot();
    Global::candidate_ui.clear_page();
    Global::candidate_ui.page_views.push_back(CandidateViewItem{"ni", "", "", "", false});
    Global::candidate_ui.page_words.push_back(L"ni");
    Global::candidate_ui.cur_page_item_cnt = 1;
    Global::candidate_ui.item_total_count = 1;
    Global::candidate_ui.page_size = 8;
    Global::candidate_ui.selected_index_in_page = 0;

    auto published = std::make_shared<Global::CandidatePageSnapshot>();
    published->page_views = Global::candidate_ui.page_views;
    published->page_words = Global::candidate_ui.page_words;
    published->candidate_string = L"1.ni";
    published->selected_index_in_page = Global::candidate_ui.selected_index_in_page;
    published->page_count = Global::candidate_ui.current_page_count();
    published->page_item_count = Global::candidate_ui.cur_page_item_cnt;
    Global::PublishCandidatePageSnapshot(std::move(published));

    const Global::CandidatePageSnapshotPtr page = Global::LoadCandidatePageSnapshot();

    // The worker rebuilds the live page while the UI thread still holds the snapshot it was handed. Reading the live
    // vectors here is exactly the use-after-free the snapshot exists to prevent, so the loaded page has to be
    // unaffected.
    Global::candidate_ui.set_items({});

    REQUIRE_EQ(page->page_views.size(), static_cast<size_t>(1));
    REQUIRE_EQ(page->page_views[0].text, std::string("ni"));
    REQUIRE_EQ(page->page_words.size(), static_cast<size_t>(1));
    REQUIRE_EQ(page->candidate_string, std::wstring(L"1.ni"));
    REQUIRE_EQ(page->page_count, 1);
    REQUIRE_EQ(page->page_item_count, 1);
    REQUIRE(Global::candidate_ui.page_views.empty());

    Global::ClearCandidatePageSnapshot();
    REQUIRE(Global::LoadCandidatePageSnapshot()->page_views.empty());
}

TEST_CASE(candidate_ui_page_navigation_boundaries)
{
    auto state = MakeCandidateUiState(15, 5);
    state.page_index = 0;
    state.selected_index_in_page = 0;

    REQUIRE(!state.has_prev_page());
    REQUIRE(state.has_next_page());

    state.page_index += 1;
    REQUIRE(state.has_prev_page());
    REQUIRE(state.has_next_page());

    state.page_index += 1;
    REQUIRE(state.has_prev_page());
    REQUIRE(!state.has_next_page());
    REQUIRE(state.current_page_count() == 5);
}

// Paging expands the candidate list ahead of the move when these predicates
// fire. Which page they fire on is what keeps a half-empty last page from ever
// being shown.
TEST_CASE(candidate_ui_flags_only_the_page_before_a_partial_last_page)
{
    auto state = MakeCandidateUiState(13, 5);

    state.page_index = 0;
    REQUIRE(!state.is_next_page_partial_last_page());
    REQUIRE(state.is_current_page_full());

    state.page_index = 1;
    REQUIRE(state.is_next_page_partial_last_page());
    REQUIRE(state.is_current_page_full());

    state.page_index = 2;
    REQUIRE(!state.is_next_page_partial_last_page());
    REQUIRE(!state.is_current_page_full());
    REQUIRE(state.current_page_count() == 3);
}

TEST_CASE(candidate_ui_never_flags_a_partial_last_page_when_pages_divide_evenly)
{
    auto state = MakeCandidateUiState(15, 5);

    for (int page = 0; page < 3; ++page)
    {
        state.page_index = page;
        REQUIRE(!state.is_next_page_partial_last_page());
        REQUIRE(state.is_current_page_full());
    }
}
