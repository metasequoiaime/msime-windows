#include "ipc/event_listener_internal.h"
#include "tests/includes/test_framework.h"

#include <array>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>

namespace
{
using FanyNamedPipe::ClassifyTask;
using FanyNamedPipe::kMaintenanceMaxWaitMs;
using FanyNamedPipe::kTaskClassCount;
using FanyNamedPipe::NextTaskClass;
using FanyNamedPipe::TaskClass;
using FanyNamedPipe::TaskClassHead;
using FanyNamedPipe::TaskClassIndex;
using FanyNamedPipe::TaskClassName;
using FanyNamedPipe::TaskType;

std::array<TaskClassHead, kTaskClassCount> HeadsWith(std::initializer_list<std::pair<TaskClass, double>> waiting)
{
    std::array<TaskClassHead, kTaskClassCount> heads{};
    for (const auto &[task_class, wait_ms] : waiting)
    {
        heads[TaskClassIndex(task_class)] = TaskClassHead{true, wait_ms};
    }
    return heads;
}
} // namespace

// 映射表就是调度策略本身；任何一个 TaskType 归错类，按键都可能排在后台富化后面。
TEST_CASE(task_classification_covers_every_task_type)
{
    const std::pair<TaskType, TaskClass> expected[] = {
        {TaskType::ShowCandidate, TaskClass::Foreground},
        {TaskType::HideCandidate, TaskClass::Foreground},
        {TaskType::HideCaretState, TaskClass::Foreground},
        {TaskType::MoveCandidate, TaskClass::Foreground},
        {TaskType::ImeKeyEvent, TaskClass::Foreground},
        {TaskType::LangbarRightClick, TaskClass::Foreground},
        {TaskType::IMESwitch, TaskClass::Foreground},
        {TaskType::PuncSwitch, TaskClass::Foreground},
        {TaskType::DoubleSingleByteSwitch, TaskClass::Foreground},
        {TaskType::ApplyCloudCandidate, TaskClass::Enrichment},
        {TaskType::ApplyAiCandidate, TaskClass::Enrichment},
        {TaskType::ApplyEnglishCandidates, TaskClass::Enrichment},
        {TaskType::ApplyCandidateTranslations, TaskClass::Enrichment},
        {TaskType::ApplyMixedCandidates, TaskClass::Enrichment},
        {TaskType::StoreUserPhrase, TaskClass::Maintenance},
        {TaskType::ClientActivated, TaskClass::Foreground},
        {TaskType::ClientDeactivated, TaskClass::Foreground},
        {TaskType::ClientSuspended, TaskClass::Foreground},
        {TaskType::StatusSnapshot, TaskClass::Foreground},
        {TaskType::UiCommitCandidate, TaskClass::Foreground},
        {TaskType::UiPinCandidate, TaskClass::UserAction},
        {TaskType::UiDeleteCandidate, TaskClass::UserAction},
        {TaskType::UiFixCandidatePosition, TaskClass::UserAction},
        {TaskType::UiClearCandidatePosition, TaskClass::UserAction},
        {TaskType::UiPageUp, TaskClass::Foreground},
        {TaskType::UiPageDown, TaskClass::Foreground},
        {TaskType::UiCommitCandidateAt, TaskClass::Foreground},
        {TaskType::UiExpandCandidates, TaskClass::UserAction},
        {TaskType::ReloadInputSession, TaskClass::Maintenance},
        {TaskType::EnsureInputSessionMatchesConfig, TaskClass::Maintenance},
        {TaskType::ApplyCandidatePageSize, TaskClass::Maintenance},
        {TaskType::RefreshCandidatePage, TaskClass::Maintenance},
        {TaskType::ResetInputSessionCache, TaskClass::Maintenance},
        {TaskType::ExitEnglishInputMode, TaskClass::UserAction},
        {TaskType::ToolbarLanguageToggle, TaskClass::UserAction},
        {TaskType::ApplyRescoredOrder, TaskClass::Enrichment},
        {TaskType::RefreshWithEnrichment, TaskClass::Enrichment},
    };
    for (const auto &[type, task_class] : expected)
    {
        REQUIRE(ClassifyTask(type) == task_class);
    }
}

TEST_CASE(task_class_names_are_distinct_and_loggable)
{
    const wchar_t *names[] = {
        TaskClassName(TaskClass::Foreground),
        TaskClassName(TaskClass::UserAction),
        TaskClassName(TaskClass::Enrichment),
        TaskClassName(TaskClass::Maintenance),
    };
    for (std::size_t i = 0; i < std::size(names); ++i)
    {
        REQUIRE(names[i] != nullptr);
        REQUIRE(names[i][0] != L'\0');
        for (std::size_t j = i + 1; j < std::size(names); ++j)
        {
            REQUIRE(std::wstring(names[i]) != names[j]);
        }
    }
}

// 富化先到、按键后到：出队仍是按键先。这是 P1 唯一要保证的用户可见结果。
TEST_CASE(foreground_is_taken_before_an_older_enrichment_task)
{
    const auto both_waiting = HeadsWith({{TaskClass::Enrichment, 25.0}, {TaskClass::Foreground, 0.5}});
    REQUIRE(NextTaskClass(both_waiting) == TaskClass::Foreground);

    // 按键取走之后，剩下的富化才轮到。
    const auto enrichment_only = HeadsWith({{TaskClass::Enrichment, 25.5}});
    REQUIRE(NextTaskClass(enrichment_only) == TaskClass::Enrichment);
}

TEST_CASE(user_action_is_taken_before_enrichment)
{
    const auto both_waiting = HeadsWith({{TaskClass::Enrichment, 25.0}, {TaskClass::UserAction, 1.0}});
    REQUIRE(NextTaskClass(both_waiting) == TaskClass::UserAction);
}

TEST_CASE(maintenance_overtakes_enrichment_only_after_the_wait_limit)
{
    // 未到上限（含恰好等于）：仍让富化先走。
    REQUIRE(NextTaskClass(HeadsWith({{TaskClass::Enrichment, 900.0},
                                     {TaskClass::Maintenance, kMaintenanceMaxWaitMs}})) == TaskClass::Enrichment);
    // 超过上限：维护任务越过富化，否则用户一直打字时它永远轮不到。
    REQUIRE(NextTaskClass(
                HeadsWith({{TaskClass::Enrichment, 900.0}, {TaskClass::Maintenance, kMaintenanceMaxWaitMs + 1.0}})) ==
            TaskClass::Maintenance);
}

TEST_CASE(maintenance_never_overtakes_foreground_or_user_action)
{
    REQUIRE(NextTaskClass(HeadsWith({{TaskClass::Foreground, 0.1}, {TaskClass::Maintenance, 5000.0}})) ==
            TaskClass::Foreground);
    REQUIRE(NextTaskClass(HeadsWith({{TaskClass::UserAction, 0.1}, {TaskClass::Maintenance, 5000.0}})) ==
            TaskClass::UserAction);
    // 三更高优先级都空时，等过上限的维护任务才轮到。
    REQUIRE(NextTaskClass(HeadsWith({{TaskClass::Maintenance, 0.0}})) == TaskClass::Maintenance);
}
