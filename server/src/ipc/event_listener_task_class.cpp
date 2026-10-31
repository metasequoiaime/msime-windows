// 任务分级表与取任务次序。这两件事都不依赖队列和线程，单独放一个翻译单元，测试不必把整条
// event_listener 链路链进来。
#include "ipc/event_listener_internal.h"

#if defined(_MSC_VER)
#pragma warning(push)
// C4062（switch 没有覆盖某个枚举值）在 /W4 下默认不开，error 形式会显式打开并升为错误：
// TaskType / TaskClass 新增枚举值却忘了归类时，这里直接编译失败，而不是悄悄沿用某个默认分支。
#pragma warning(error : 4062)
#endif

namespace FanyNamedPipe
{
TaskClass ClassifyTask(TaskType type)
{
    switch (type)
    {
    // 用户按下键，以及按键直接引发的界面/焦点变化：这些是用户正在等的结果。
    case TaskType::ImeKeyEvent:
    case TaskType::ShowCandidate:
    case TaskType::HideCandidate:
    case TaskType::HideCaretState:
    case TaskType::MoveCandidate:
    case TaskType::ClientActivated:
    case TaskType::ClientDeactivated:
    case TaskType::ClientSuspended:
    case TaskType::StatusSnapshot:
    case TaskType::IMESwitch:
    case TaskType::PuncSwitch:
    case TaskType::DoubleSingleByteSwitch:
    case TaskType::LangbarRightClick:
    case TaskType::UiCommitCandidate:
    case TaskType::UiCommitCandidateAt:
    case TaskType::UiPageUp:
    case TaskType::UiPageDown:
        return TaskClass::Foreground;

    // 用户主动点的候选操作：有可见的后续，但当前按键的回复不依赖它们。
    case TaskType::UiPinCandidate:
    case TaskType::UiDeleteCandidate:
    case TaskType::UiFixCandidatePosition:
    case TaskType::UiClearCandidatePosition:
    case TaskType::UiExpandCandidates:
    case TaskType::UiCloseTranslationPanel:
    case TaskType::UiCommitExpansionPanelItem:
    case TaskType::ExitEnglishInputMode:
    case TaskType::ToolbarLanguageToggle:
        return TaskClass::UserAction;

    // 后台算完的富化结果：晚一轮就可能随下一次按键失效，但绝不能挡住下一次按键。
    case TaskType::ApplyCloudCandidate:
    case TaskType::ApplyAiCandidate:
    case TaskType::ApplyEnglishCandidates:
    case TaskType::ApplyCandidateTranslations:
    case TaskType::ApplyMixedCandidates:
    case TaskType::ApplyRescoredOrder:
    case TaskType::RefreshWithEnrichment:
        return TaskClass::Enrichment;

    // 没有人在等结果，但丢不得、也不能无限期拖着的事。
    case TaskType::StoreUserPhrase:
    case TaskType::ReloadInputSession:
    case TaskType::EnsureInputSessionMatchesConfig:
    case TaskType::ApplyCandidatePageSize:
    case TaskType::RefreshCandidatePage:
    case TaskType::ResetInputSessionCache:
        return TaskClass::Maintenance;
    }
    // 枚举已穷举；这里只是让编译器不再要求「所有路径都返回」。
    return TaskClass::Foreground;
}

const wchar_t *TaskClassName(TaskClass type)
{
    switch (type)
    {
    case TaskClass::Foreground:
        return L"foreground";
    case TaskClass::UserAction:
        return L"user-action";
    case TaskClass::Enrichment:
        return L"enrichment";
    case TaskClass::Maintenance:
        return L"maintenance";
    }
    return L"foreground";
}

bool NeedsInputPriorityBoost(TaskClass type)
{
    return type == TaskClass::Foreground;
}

TaskClass NextTaskClass(const std::array<TaskClassHead, kTaskClassCount> &heads)
{
    const TaskClassHead &foreground = heads[TaskClassIndex(TaskClass::Foreground)];
    const TaskClassHead &user_action = heads[TaskClassIndex(TaskClass::UserAction)];
    const TaskClassHead &enrichment = heads[TaskClassIndex(TaskClass::Enrichment)];
    const TaskClassHead &maintenance = heads[TaskClassIndex(TaskClass::Maintenance)];

    // 用户正在等的任务先取：这正是把富化从按键前面摘出去的全部目的。
    if (foreground.has_task)
        return TaskClass::Foreground;
    if (user_action.has_task)
        return TaskClass::UserAction;

    // 维护任务等满上限就放行。否则用户不停打字时富化队列始终非空，它永远轮不到。
    // 放行只越过 Enrichment：用户可见的那两类已经在上面判过了。
    if (maintenance.has_task && (!enrichment.has_task || maintenance.wait_ms > kMaintenanceMaxWaitMs))
        return TaskClass::Maintenance;
    if (enrichment.has_task)
        return TaskClass::Enrichment;
    if (maintenance.has_task)
        return TaskClass::Maintenance;

    // 调用方保证至少一类有任务；返回 Foreground 让误用时取空队列立刻暴露。
    return TaskClass::Foreground;
}
} // namespace FanyNamedPipe

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
