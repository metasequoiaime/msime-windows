#include "ipc/candidate_rescore_budget.h"
#include "tests/includes/test_framework.h"

using FanyImeIpc::RescoreRequeryBudget;

TEST_CASE(rescore_budget_caps_each_input_and_resets)
{
    RescoreRequeryBudget budget;
    for (int i = 0; i < RescoreRequeryBudget::kMaxRequestsPerInput; ++i)
        REQUIRE(budget.TryAcquire("photon"));
    REQUIRE(!budget.TryAcquire("photon"));
    REQUIRE(budget.count() == RescoreRequeryBudget::kMaxRequestsPerInput);

    budget.Reset();
    REQUIRE(budget.TryAcquire("photon"));
    REQUIRE(budget.count() == 1);

    REQUIRE(budget.TryAcquire("photo"));
    REQUIRE(budget.count() == 1);

    REQUIRE(!budget.TryAcquire(""));
    REQUIRE(budget.count() == 0);
    REQUIRE(budget.TryAcquire("photon"));
}
