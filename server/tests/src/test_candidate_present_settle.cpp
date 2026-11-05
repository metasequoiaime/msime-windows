#include "ipc/candidate_present_settle_policy.h"
#include "tests/includes/test_framework.h"

#include <chrono>
#include <string>

namespace
{
using FanyImeIpc::CandidatePresentSettle;
using Clock = CandidatePresentSettle::Clock;
using std::chrono::milliseconds;

constexpr milliseconds kDelay{120};
} // namespace

TEST_CASE(present_settle_arms_once_for_the_same_input)
{
    CandidatePresentSettle settle;
    const Clock::time_point start{};
    REQUIRE(settle.Request("photon", false, start, kDelay));
    REQUIRE(!settle.Due(start + milliseconds{119}));
    REQUIRE(settle.Due(start + kDelay));
    REQUIRE(settle.Consume(start + kDelay, "photon", true));
    REQUIRE(!settle.Request("photon", false, start + milliseconds{400}, kDelay));
}

TEST_CASE(present_settle_late_request_folds_into_the_fast_frame)
{
    CandidatePresentSettle settle;
    const Clock::time_point start{};
    REQUIRE(settle.Request("photon", false, start, kDelay));
    REQUIRE(settle.Request("photon", true, start + milliseconds{10}, kDelay));
    REQUIRE(!settle.late_pending);
    REQUIRE(settle.Consume(start + kDelay, "photon", true));
    REQUIRE(!settle.late_done);

    REQUIRE(settle.Request("photon", true, start + milliseconds{300}, kDelay));
    REQUIRE(settle.late_pending);
    REQUIRE(settle.Consume(start + milliseconds{420}, "photon", true));
    REQUIRE(settle.late_done);
    REQUIRE(!settle.Request("photon", true, start + milliseconds{600}, kDelay));
}

TEST_CASE(present_settle_late_before_fast_keeps_fast_budget)
{
    CandidatePresentSettle settle;
    const Clock::time_point start{};
    // A gloss/effect-tier result can arrive before the mixed/rescore result. It may paint a late frame, but it must
    // not spend the fast frame's budget: the fast update still has to be shown.
    REQUIRE(settle.Request("photon", true, start, kDelay));
    REQUIRE(settle.Consume(start + kDelay, "photon", true));
    REQUIRE(settle.late_done);
    REQUIRE(!settle.done);

    REQUIRE(settle.Request("photon", false, start + milliseconds{200}, kDelay));
    REQUIRE(settle.Consume(start + milliseconds{320}, "photon", true));
    REQUIRE(settle.done);
}

TEST_CASE(present_settle_input_change_starts_a_new_budget)
{
    CandidatePresentSettle settle;
    const Clock::time_point start{};
    REQUIRE(settle.Request("photon", false, start, kDelay));
    REQUIRE(settle.Consume(start + kDelay, "photon", true));
    REQUIRE(!settle.Request("photon", false, start + kDelay, kDelay));
    REQUIRE(settle.Request("photo", false, start + kDelay, kDelay));
    REQUIRE(!settle.done);
}

TEST_CASE(present_settle_consumes_a_frame_even_without_showing)
{
    CandidatePresentSettle settle;
    const Clock::time_point start{};
    REQUIRE(settle.Request("photon", false, start, kDelay));
    REQUIRE(!settle.Consume(start + kDelay, "photon", false));
    REQUIRE(!settle.Request("photon", false, start + kDelay, kDelay));
}

TEST_CASE(present_settle_cancel_blocks_later_requests)
{
    CandidatePresentSettle settle;
    const Clock::time_point start{};
    REQUIRE(settle.Request("photon", false, start, kDelay));
    settle.Cancel();
    REQUIRE(!settle.Request("photon", false, start + milliseconds{200}, kDelay));
    REQUIRE(!settle.Request("photon", true, start + milliseconds{200}, kDelay));
}

TEST_CASE(present_settle_stale_input_is_not_shown)
{
    CandidatePresentSettle settle;
    const Clock::time_point start{};
    REQUIRE(settle.Request("photon", false, start, kDelay));
    REQUIRE(!settle.Consume(start + kDelay, "photo", true));
    REQUIRE(settle.done);
}
