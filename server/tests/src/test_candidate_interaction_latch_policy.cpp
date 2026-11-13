#include "ipc/candidate_interaction_latch_policy.h"
#include "tests/includes/test_framework.h"

using FanyImeIpc::ShouldLatchCandidateInteraction;

TEST_CASE(settle_latches_interaction_against_the_rendered_page)
{
    // The interaction carries the generation the UI actually painted, not the newest published snapshot. A
    // coalesced publish that has not reached the screen yet must not invalidate a click on the visible page.
    REQUIRE(ShouldLatchCandidateInteraction(5, 5));
    REQUIRE(!ShouldLatchCandidateInteraction(5, 6));
    REQUIRE(!ShouldLatchCandidateInteraction(0, 0));
    REQUIRE(!ShouldLatchCandidateInteraction(0, 5));
}
