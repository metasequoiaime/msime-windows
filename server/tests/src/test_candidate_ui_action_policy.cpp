#include "ipc/candidate_ui_action_policy.h"
#include "tests/includes/test_framework.h"

TEST_CASE(candidate_ui_action_accepts_exact_one_based_page_range)
{
    REQUIRE(FanyImeIpc::IsValidCandidateUiOneBasedIndex(1));
    REQUIRE(FanyImeIpc::IsValidCandidateUiOneBasedIndex(9));
    REQUIRE(FanyImeIpc::IsValidCandidateUiOneBasedIndex(10));

    REQUIRE(!FanyImeIpc::IsValidCandidateUiOneBasedIndex(0));
    REQUIRE(!FanyImeIpc::IsValidCandidateUiOneBasedIndex(-1));
    REQUIRE(!FanyImeIpc::IsValidCandidateUiOneBasedIndex(11));
}

TEST_CASE(candidate_down_expands_once_even_before_the_ui_handles_the_request)
{
    using FanyImeIpc::CandidateExpandState;
    std::atomic<CandidateExpandState> state{CandidateExpandState::Unavailable};
    REQUIRE(!FanyImeIpc::RequestCandidateExpandOnDown(0x28, 0, false, state));

    FanyImeIpc::PublishCandidateExpandAvailability(state, true, false);
    REQUIRE(FanyImeIpc::RequestCandidateExpandOnDown(0x28, 0, false, state));
    REQUIRE_EQ(state.load(), CandidateExpandState::Requested);
    // A queued repaint and a second Down must not cancel or duplicate the first request.
    FanyImeIpc::PublishCandidateExpandAvailability(state, true, false);
    REQUIRE_EQ(state.load(), CandidateExpandState::Requested);
    REQUIRE(!FanyImeIpc::RequestCandidateExpandOnDown(0x28, 0, false, state));

    FanyImeIpc::PublishCandidateExpandAvailability(state, true, true);
    REQUIRE_EQ(state.load(), CandidateExpandState::Expanded);
    REQUIRE(!FanyImeIpc::RequestCandidateExpandOnDown(0x28, 0, false, state));
}

TEST_CASE(candidate_down_expansion_leaves_other_keys_and_native_candidates_unchanged)
{
    using FanyImeIpc::CandidateExpandState;
    std::atomic<CandidateExpandState> state{CandidateExpandState::Collapsed};
    for (const uint32_t key : {0x26u, 0x25u, 0x27u, 0x20u})
        REQUIRE(!FanyImeIpc::RequestCandidateExpandOnDown(key, 0, false, state));
    for (const uint32_t modifiers : {1u, 2u, 4u, 3u, 7u})
        REQUIRE(!FanyImeIpc::RequestCandidateExpandOnDown(0x28, modifiers, false, state));
    REQUIRE(!FanyImeIpc::RequestCandidateExpandOnDown(0x28, 0, true, state));
    REQUIRE_EQ(state.load(), CandidateExpandState::Collapsed);
}

TEST_CASE(candidate_down_expansion_resets_after_hide_or_switching_to_a_non_expandable_skin)
{
    using FanyImeIpc::CandidateExpandState;
    std::atomic<CandidateExpandState> state{CandidateExpandState::Collapsed};
    REQUIRE(FanyImeIpc::RequestCandidateExpandOnDown(0x28, 0, false, state));
    FanyImeIpc::PublishCandidateExpandAvailability(state, false, false);
    REQUIRE_EQ(state.load(), CandidateExpandState::Unavailable);
    REQUIRE(!FanyImeIpc::RequestCandidateExpandOnDown(0x28, 0, false, state));

    FanyImeIpc::PublishCandidateExpandAvailability(state, true, false);
    REQUIRE(FanyImeIpc::RequestCandidateExpandOnDown(0x28, 0, false, state));
}
