#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace FanyNamedPipe
{
enum class TaskType
{
    ShowCandidate,
    HideCandidate,
    HideCaretState,
    MoveCandidate,
    ImeKeyEvent,
    LangbarRightClick,
    IMESwitch,
    PuncSwitch,
    DoubleSingleByteSwitch,
    ApplyCloudCandidate,
    ApplyAiCandidate,
    ApplyEnglishCandidates,
    ApplyCandidateTranslations,
    ApplyMixedCandidates,
    StoreUserPhrase,
    ClientActivated,
    ClientDeactivated,
    ClientSuspended,
    StatusSnapshot,
    UiCommitCandidate,
    UiPinCandidate,
    UiDeleteCandidate,
    UiFixCandidatePosition,
    UiClearCandidatePosition,
    UiPageUp,
    UiPageDown,
    UiCommitCandidateAt,
    UiExpandCandidates,
    ReloadInputSession,
    EnsureInputSessionMatchesConfig,
    ApplyCandidatePageSize,
    RefreshCandidatePage,
    ResetInputSessionCache,
    ExitEnglishInputMode,
    ToolbarLanguageToggle,
    ApplyRescoredOrder,
    RefreshWithEnrichment,
};

// The worker keeps FIFO order within each class.
// It also preserves posting order across Foreground and UserAction.
enum class TaskClass
{
    Foreground,
    UserAction,
    Enrichment,
    Maintenance,
};

constexpr std::size_t kTaskClassCount = 4;

constexpr std::size_t TaskClassIndex(TaskClass type)
{
    return static_cast<std::size_t>(type);
}

struct TaskClassHead
{
    bool has_task = false;
    double wait_ms = 0.0;
    std::uint64_t enqueue_sequence = 0;
};

// A waiting Maintenance task can pass Enrichment after this limit.
// It cannot pass Foreground or UserAction.
constexpr double kMaintenanceMaxWaitMs = 500.0;

// Keep classification exhaustive in event_listener_task_class.cpp.
TaskClass ClassifyTask(TaskType type);
const wchar_t *TaskClassName(TaskClass type);
// The caller supplies one head per class and at least one queued task.
TaskClass NextTaskClass(const std::array<TaskClassHead, kTaskClassCount> &heads);
} // namespace FanyNamedPipe
