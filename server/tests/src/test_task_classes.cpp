#include "ipc/event_listener_task_class.h"
#include "tests/includes/test_framework.h"

#include <array>
#include <deque>
#include <future>
#include <initializer_list>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using FanyNamedPipe::ClassifyTask;
using FanyNamedPipe::kMaintenanceMaxWaitMs;
using FanyNamedPipe::kTaskClassCount;
using FanyNamedPipe::NeedsInputPriorityBoost;
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

std::vector<TaskType> DrainBlockedWorker(std::initializer_list<TaskType> posted)
{
    struct QueuedTask
    {
        TaskType type;
        std::uint64_t sequence;
    };
    std::array<std::deque<QueuedTask>, kTaskClassCount> queues;
    std::vector<TaskType> completed;
    std::promise<void> worker_started;
    std::promise<void> release_worker;
    auto started = worker_started.get_future();
    auto released = release_worker.get_future();
    std::thread worker([&] {
        worker_started.set_value();
        released.wait();
        for (;;)
        {
            std::array<TaskClassHead, kTaskClassCount> heads{};
            bool has_work = false;
            for (std::size_t i = 0; i < queues.size(); ++i)
            {
                heads[i].has_task = !queues[i].empty();
                has_work |= heads[i].has_task;
                if (heads[i].has_task)
                    heads[i].enqueue_sequence = queues[i].front().sequence;
            }
            if (!has_work)
                break;
            auto &queue = queues[TaskClassIndex(NextTaskClass(heads))];
            completed.push_back(queue.front().type);
            queue.pop_front();
        }
    });
    started.wait();
    // The gate keeps the worker from taking a task until every task is queued.
    std::uint64_t sequence = 0;
    for (TaskType type : posted)
        queues[TaskClassIndex(ClassifyTask(type))].push_back({type, ++sequence});
    release_worker.set_value();
    worker.join();
    return completed;
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
        {TaskType::UiPinCandidate, TaskClass::Foreground},
        {TaskType::UiDeleteCandidate, TaskClass::Foreground},
        {TaskType::UiFixCandidatePosition, TaskClass::Foreground},
        {TaskType::UiClearCandidatePosition, TaskClass::Foreground},
        {TaskType::UiPageUp, TaskClass::Foreground},
        {TaskType::UiPageDown, TaskClass::Foreground},
        {TaskType::UiCommitCandidateAt, TaskClass::Foreground},
        {TaskType::UiExpandCandidates, TaskClass::UserAction},
        {TaskType::ReloadInputSession, TaskClass::Foreground},
        {TaskType::EnsureInputSessionMatchesConfig, TaskClass::Foreground},
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

// 输入线程的 boost 只给 Foreground；其余三类必须保持进程的 Normal。
TEST_CASE(input_priority_boost_is_foreground_only)
{
    REQUIRE(NeedsInputPriorityBoost(TaskClass::Foreground));
    REQUIRE(!NeedsInputPriorityBoost(TaskClass::UserAction));
    REQUIRE(!NeedsInputPriorityBoost(TaskClass::Enrichment));
    REQUIRE(!NeedsInputPriorityBoost(TaskClass::Maintenance));
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

TEST_CASE(older_user_action_runs_before_later_foreground)
{
    auto heads = HeadsWith({{TaskClass::UserAction, 10.0}, {TaskClass::Foreground, 1.0}});
    heads[TaskClassIndex(TaskClass::UserAction)].enqueue_sequence = 1;
    heads[TaskClassIndex(TaskClass::Foreground)].enqueue_sequence = 2;
    REQUIRE(NextTaskClass(heads) == TaskClass::UserAction);

    heads[TaskClassIndex(TaskClass::UserAction)].enqueue_sequence = 2;
    heads[TaskClassIndex(TaskClass::Foreground)].enqueue_sequence = 1;
    REQUIRE(NextTaskClass(heads) == TaskClass::Foreground);
}

TEST_CASE(earlier_mode_action_does_not_clear_a_later_key)
{
    const TaskType actions[] = {TaskType::ExitEnglishInputMode, TaskType::ToolbarLanguageToggle};
    const TaskType rebuilds[] = {TaskType::ReloadInputSession, TaskType::EnsureInputSessionMatchesConfig};
    for (TaskType action : actions)
    {
        for (TaskType rebuild : rebuilds)
        {
            for (const auto &completed : {DrainBlockedWorker({action, rebuild, TaskType::ImeKeyEvent}),
                                          DrainBlockedWorker({rebuild, action, TaskType::ImeKeyEvent})})
            {
                std::string preedit;
                for (TaskType type : completed)
                {
                    if (type == action || type == rebuild)
                        preedit.clear();
                    if (type == TaskType::ImeKeyEvent)
                        preedit += 'x';
                }
                REQUIRE(preedit == "x");
                REQUIRE(completed.back() == TaskType::ImeKeyEvent);
            }
        }
    }
}

TEST_CASE(earlier_expand_runs_before_later_page_change)
{
    const auto completed = DrainBlockedWorker({TaskType::UiExpandCandidates, TaskType::UiPageDown});
    REQUIRE(completed.size() == 2);
    REQUIRE(completed[0] == TaskType::UiExpandCandidates);
    REQUIRE(completed[1] == TaskType::UiPageDown);
}

TEST_CASE(indexed_candidate_actions_keep_the_clicked_page)
{
    const TaskType edits[] = {TaskType::UiPinCandidate, TaskType::UiDeleteCandidate, TaskType::UiFixCandidatePosition,
                              TaskType::UiClearCandidatePosition};
    for (TaskType edit : edits)
    {
        const auto completed = DrainBlockedWorker({edit, TaskType::UiPageDown, TaskType::ImeKeyEvent});
        int page = 0;
        std::string edited_word;
        const std::string words[] = {"clicked-word", "next-page-word"};
        for (TaskType type : completed)
        {
            if (type == TaskType::UiPageDown || type == TaskType::ImeKeyEvent)
                page = 1;
            if (type == edit)
                edited_word = words[page];
        }
        REQUIRE(edited_word == "clicked-word");
        REQUIRE(completed.size() == 3);
    }
}

TEST_CASE(session_rebuild_finishes_before_later_key)
{
    const TaskType rebuilds[] = {TaskType::ReloadInputSession, TaskType::EnsureInputSessionMatchesConfig};
    for (TaskType rebuild : rebuilds)
    {
        const auto completed = DrainBlockedWorker({rebuild, TaskType::ImeKeyEvent});
        std::string scheme = "old";
        std::string preedit;
        std::string key_scheme;
        for (TaskType type : completed)
        {
            if (type == rebuild)
            {
                scheme = "new";
                preedit.clear();
            }
            if (type == TaskType::ImeKeyEvent)
            {
                key_scheme = scheme;
                preedit += 'x';
            }
        }
        REQUIRE(key_scheme == "new");
        REQUIRE(preedit == "x");
    }
}
