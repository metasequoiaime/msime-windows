#include "ipc/candidate_settle_arbitration_policy.h"
#include "tests/includes/test_framework.h"

using FanyImeIpc::DecideSettleFlush;
using FanyImeIpc::SettleFlushDecision;
using FanyNamedPipe::TaskClass;

TEST_CASE(settle_flush_yields_to_foreground_and_user_tasks)
{
    REQUIRE(DecideSettleFlush(true, false, TaskClass::Foreground) == SettleFlushDecision::FlushSettle);
    REQUIRE(DecideSettleFlush(true, true, TaskClass::Foreground) == SettleFlushDecision::ProcessTask);
    REQUIRE(DecideSettleFlush(true, true, TaskClass::UserAction) == SettleFlushDecision::ProcessTask);
    REQUIRE(DecideSettleFlush(true, true, TaskClass::Enrichment) == SettleFlushDecision::ProcessTask);
    REQUIRE(DecideSettleFlush(true, true, TaskClass::Maintenance) == SettleFlushDecision::FlushSettle);
    REQUIRE(DecideSettleFlush(false, true, TaskClass::Foreground) == SettleFlushDecision::ProcessTask);
}
