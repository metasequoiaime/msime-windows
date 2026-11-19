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
    // DR Dict 查预编辑（Ctrl+Alt+Enter），键盘钩子在候选窗显示时投递。
    UiDictionaryPreeditLookup,
    ReloadInputSession,
    EnsureInputSessionMatchesConfig,
    ApplyCandidatePageSize,
    RefreshCandidatePage,
    ResetInputSessionCache,
    ExitEnglishInputMode,
    ToolbarLanguageToggle,
    ApplyRescoredOrder,
    RefreshWithEnrichment,
    // 横排固定宽度：候选窗量出当前页一行只放得下几个，回报给工作线程重新划页。
    UiCandidatePageFitted,
    // 英文候选模式里高亮候选的音标与释义（词库或 DR Dict）查完了。
    ApplyEnglishCandidateDetail,
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
// Boost input priority only while the worker runs a Foreground task.
bool NeedsInputPriorityBoost(TaskClass type);
// The caller supplies one head per class and at least one queued task.
TaskClass NextTaskClass(const std::array<TaskClassHead, kTaskClassCount> &heads);
} // namespace FanyNamedPipe
