#include "mixed/mixed_candidates_policy.h"
#include "tests/includes/test_framework.h"

TEST_CASE(mixed_candidates_delivers_an_empty_answer)
{
    MixedCandidates::Result empty;
    MixedCandidates::Result non_empty;
    non_empty.english.emplace_back("photon", "photon", 1, CandidateSource::EnglishDictionary);

    REQUIRE(FanyImeIpc::ShouldDeliverMixedResult(empty, false, true));
    REQUIRE(FanyImeIpc::ShouldDeliverMixedResult(non_empty, false, true));
    REQUIRE(!FanyImeIpc::ShouldDeliverMixedResult(empty, true, true));
    REQUIRE(!FanyImeIpc::ShouldDeliverMixedResult(empty, false, false));
}
