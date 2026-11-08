#include "ipc/candidate_commit_guard_policy.h"
#include "tests/includes/test_framework.h"

TEST_CASE(commit_page_is_usable_only_for_its_own_composition)
{
    using FanyImeIpc::CommitPageState;
    using FanyImeIpc::CommitPageUsable;

    CommitPageState state;
    state.page_has_words = true;
    state.page_matches_composition = true;
    REQUIRE(CommitPageUsable(state));

    // A page from an older input must never decide what this key commits.
    state.page_matches_composition = false;
    REQUIRE(!CommitPageUsable(state));

    // No page at all.
    state.page_has_words = false;
    state.page_matches_composition = false;
    REQUIRE(!CommitPageUsable(state));

    // The creating-word and pure-punctuation paths have no input string left,
    // so the page is usable without an identity match.
    state.page_has_words = true;
    state.composition_empty = true;
    REQUIRE(CommitPageUsable(state));
}

TEST_CASE(commit_rebuilds_only_when_there_is_a_composition_to_query)
{
    using FanyImeIpc::CommitPageState;
    using FanyImeIpc::ShouldRebuildCommitPage;

    CommitPageState state;
    state.composition_empty = false;
    state.page_has_words = false;
    REQUIRE(ShouldRebuildCommitPage(state));

    state.page_has_words = true;
    state.page_matches_composition = false;
    REQUIRE(ShouldRebuildCommitPage(state));

    state.page_matches_composition = true;
    REQUIRE(!ShouldRebuildCommitPage(state));

    // Empty composition and no page is the pure-punctuation case, not a page
    // that went missing. Querying the engine would be pointless.
    state.composition_empty = true;
    state.page_has_words = false;
    REQUIRE(!ShouldRebuildCommitPage(state));
}

TEST_CASE(punctuation_commit_needs_a_candidate_when_a_composition_is_live)
{
    using FanyImeIpc::ShouldRejectPunctuationCommit;

    REQUIRE(ShouldRejectPunctuationCommit(true, false));
    REQUIRE(!ShouldRejectPunctuationCommit(true, true));
    // Pure punctuation has no composition to protect.
    REQUIRE(!ShouldRejectPunctuationCommit(false, false));
    REQUIRE(!ShouldRejectPunctuationCommit(false, true));
}
