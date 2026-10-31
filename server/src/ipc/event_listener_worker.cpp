// 任务队列与任务线程：Task 定义、WorkerThread 的分发循环，以及各个 Enqueue* 入口。
#include "ipc/event_listener_internal.h"
#include <Windows.h>
#include <array>
#include <string>
#include <chrono>
#include <cstdint>
#include <utility>
#include "ipc.h"
#include "ipc/selection_replay_guard.h"
#include "ipc/async_request_origin.h"
#include "ipc/candidate_ui_owner.h"
#include "ipc/input_key_policy.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include "utils/common_utils.h"
#include "utils/serial_task_runner.h"
#include "utils/thread_priority.h"
#include <utf8.h>
#include "global/globals.h"
#include "engine/user_dictionary/user_dictionary_journal.h"
#include "mixed/date_time_candidates.h"
#include "window/caret_state_indicator_policy.h"
#include "english/english_ime.h"
#include "config/ime_config.h"
#include "conversion/chinese_converter.h"
#include "ipc/traditional_variant_expansion_policy.h"
#include "window/window_hook.h"
#include "session/input_cadence.h"
#include "session/session_factory.h"
#include "log/candidate_diag_log.h"
#include "log/ftb_diag_log.h"
#include "voice-input/voice_input_service.h"

using namespace event_listener_detail;

namespace
{
std::shared_ptr<IInputSession> PersistentInputSession()
{
    return g_r_mode_original_session ? g_r_mode_original_session : g_inputSession;
}

// A task that waited at least this long was delivered behind a stalled worker,
// so the state it describes may already be superseded. Normal typing never gets
// near it: queue waits stay under a few milliseconds unless something blocks the
// task thread.
constexpr double kCandidateHideBacklogMs = 24.0;
// 排队日志与按键分段使用同一个 8 ms 门槛。高精度时钟让这个门槛真正可用。
constexpr double kSlowQueueThresholdMs = 8.0;

uint64_t g_last_status_snapshot_client_id = 0;

// After switching away from this IME, the next StatusSnapshot must restore the
// configured default mode even when the same process/thread client_id reconnects.
bool g_force_global_ime_sync = false;

int EnsureAuthoritativeCnMode()
{
    if (g_authoritative_cn_mode < 0)
    {
        ReloadImeConfigIfChanged();
        g_authoritative_cn_mode = GetConfiguredDefaultImeMode() == "english" ? 0 : 1;
    }
    return g_authoritative_cn_mode;
}

void PostCaretStatePosition(int x, int y)
{
    const HWND hwnd = ::global_hwnd_caret_state;
    // Visibility is owned by the UI thread, which ignores moves while hidden.
    if (!hwnd)
        return;
    auto *caret = new (std::nothrow) POINT{x, y};
    if (caret && !PostMessage(hwnd, WM_MOVE_CARET_STATE, 0, reinterpret_cast<LPARAM>(caret)))
        delete caret;
}

void PostHideCaretState()
{
    if (const HWND hwnd = ::global_hwnd_caret_state)
        PostMessage(hwnd, WM_HIDE_CARET_STATE, 0, 0);
}

// 日期时间候选的右键操作按格式 ID（item.pinyin）记，不按文字：日期文本每天都变。置顶挪进学到的
// 顺序，固定位置借用 fixed_candidate_positions；删除没有意义（格式不是词库里的行），不做。
void ApplyDateTimeCandidateUiAction(FanyNamedPipe::TaskType type, const WordItem &item, int fixed_position)
{
    const std::string keyword = DateTimeKeywordForFormat(item.pinyin);
    const std::string category = metasequoia::local_modes::date_time_category(keyword);
    if (category.empty())
        return;
    const std::string user_db_path = user_dictionary::default_user_db_path();
    const std::string context_key = user_dictionary::date_time_fixed_position_context(category);
    switch (type)
    {
    case FanyNamedPipe::TaskType::UiPinCandidate:
        (void)user_dictionary::pin_date_time_format(
            user_db_path, category, metasequoia::local_modes::date_time_format_ids(keyword), item.pinyin);
        break;
    case FanyNamedPipe::TaskType::UiFixCandidatePosition:
        (void)user_dictionary::set_fixed_position(user_db_path, context_key, item.pinyin, item.pinyin, fixed_position);
        break;
    case FanyNamedPipe::TaskType::UiClearCandidatePosition:
        (void)user_dictionary::clear_fixed_position(user_db_path, context_key, item.pinyin, item.pinyin);
        break;
    default:
        break;
    }
}

// The pipe server accepts clients before the candidate window exists, so an
// activation can arrive with nowhere to deliver it. Held here until the window
// creation path can replay it.
std::atomic_bool g_deferred_client_activation{false};
} // namespace

namespace FanyNamedPipe
{
void ReplayDeferredClientActivation()
{
    if (!::global_hwnd)
    {
        return;
    }
    if (!g_deferred_client_activation.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    FTB_DIAG_LOGF(L"replaying client activation deferred until candidate window existed");
    PostMessage(::global_hwnd, WM_IMEACTIVATE, 0, 0);
    PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
}

struct Task
{
    TaskType type;
    bool has_pipe_data = false;
    FanyImeNamedpipeData pipe_data = {};
    uint64_t client_id = 0;
    uint64_t activation_epoch = 0;
    std::chrono::steady_clock::time_point enqueued_at{};
    std::string cloud_candidate;
    std::string cloud_pinyin;
    uint64_t cloud_generation = 0;
    std::string ai_candidate;
    std::string ai_identity;
    uint64_t ai_generation = 0;
    std::optional<metasequoia::OnlineQuery> online_query;
    std::vector<WordItem> english_candidates;
    std::string english_input;
    uint64_t english_generation = 0;
    std::vector<EnglishIme::TranslationResult> translation_results;
    uint64_t translation_generation = 0;
    bool translation_merge = false;
    MixedCandidates::Result mixed_result;
    std::string mixed_input;
    uint64_t mixed_generation = 0;
    std::string session_pinyin;
    std::string session_word;
    bool session_pinyin_is_canonical = false;
    std::shared_ptr<IInputSession> phrase_session;
    int candidate_one_based_index = 0;
    // UiCommitCandidateAt：卷轴展开面板里点中的候选的绝对下标与显示文字。
    int candidate_absolute_index = 0;
    std::wstring candidate_expected_text;
    int fixed_position = 0;
    int page_steps = 0;
    bool toolbar_from_ime_enabled = false;
};

// 排队延迟与按键分段共用高精度时钟。零值表示该任务没有记录入队时间。
double QueueWaitMilliseconds(const Task &task)
{
    if (task.enqueued_at == std::chrono::steady_clock::time_point{})
        return 0.0;
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - task.enqueued_at).count();
}

// 按类别分的四条 FIFO 队列。同类内保持入队顺序；类别之间由 NextTaskClass 决定。
std::array<std::queue<Task>, kTaskClassCount> taskQueues;
std::mutex queueMutex;
// 队列里已经有一个还没开始跑的 ApplyRescoredOrder（受 queueMutex 保护）。它跑起来时会读所有模型
// 已算完的结果，所以排队期间再完成的批次不必再各排一个，免得每批都在按键前面重查一次词格。
bool rescoreApplyQueued = false;
// 同理，停手重查最多排一个：排队期间再关一次闸不该叠出第二次全量重查（受 queueMutex 保护）。
bool refreshEnrichmentQueued = false;

// 调用方持 queueMutex。
void PushTaskLocked(Task &&task)
{
    // 入队时间在取任务时才用来算队首年龄；维护任务原先都不记这个字段，统一在这里补上，
    // 否则它们的等待时长恒为 0，「等满 500 ms 就越过富化」永远不会触发。
    if (task.enqueued_at == std::chrono::steady_clock::time_point{})
        task.enqueued_at = std::chrono::steady_clock::now();
    taskQueues[TaskClassIndex(ClassifyTask(task.type))].push(std::move(task));
}

// 某个任务类别自己的队尾：同类型合并只该看同类队列，不能看整个队列的最新一格。
std::queue<Task> &TaskQueueFor(TaskType type)
{
    return taskQueues[TaskClassIndex(ClassifyTask(type))];
}

bool AnyTaskQueuedLocked()
{
    for (const std::queue<Task> &queue : taskQueues)
    {
        if (!queue.empty())
            return true;
    }
    return false;
}

// 调用方持 queueMutex。每类队首现算一次年龄，再交给纯函数决定下一类。
Task TakeNextTaskLocked()
{
    std::array<TaskClassHead, kTaskClassCount> heads;
    for (std::size_t i = 0; i < kTaskClassCount; ++i)
    {
        if (!taskQueues[i].empty())
        {
            heads[i].has_task = true;
            heads[i].wait_ms = QueueWaitMilliseconds(taskQueues[i].front());
        }
    }
    std::queue<Task> &queue = taskQueues[TaskClassIndex(NextTaskClass(heads))];
    Task task = std::move(queue.front());
    queue.pop();
    return task;
}

// 顶字推送后的 HideCandidate 抑制标记：CommitCandidateAndContinue 会让 DLL 提交文本并
// 结束旧组合，TSF 随之发来 HideCandidateWnd；若 HideCandidate 处理器照常 ClearState，
// 会把服务端刚重建好的余码组合（如「数据」顶字后剩下的 x）抹掉，用户后续按键从空组合
// 开始组词——这正是「顶字后 x 没进组词」的根因。所有值都只在 worker 线程读写。
// 顶字与四码唯一自动上屏都会推送，每次推送成功记一笔、每个 HideCandidate 消费一笔；
// 不能在下一个按键时撤销：快打时下一个字母常常先于 DLL 应用推送到达服务端，那时撤销
// 标记，随后到来的 HideCandidateWnd 就会把余码清掉。推送被 DLL 丢弃（焦点/组合纪元已变）
// 时不会有对应的 HideCandidateWnd，所以另设一个时限，过期的记账不再压制真正的清理。
constexpr ULONGLONG kTopCommitHideSuppressMs = 1000;
uint64_t g_topCommitRemainderClient = 0;
uint64_t g_topCommitRemainderEpoch = 0;
uint32_t g_topCommitPendingHides = 0;
ULONGLONG g_topCommitLastPushMs = 0;

void NoteTopCommitPushed(uint64_t client_id, uint64_t activation_epoch)
{
    if (client_id != g_topCommitRemainderClient || activation_epoch != g_topCommitRemainderEpoch)
    {
        g_topCommitPendingHides = 0;
    }
    g_topCommitRemainderClient = client_id;
    g_topCommitRemainderEpoch = activation_epoch;
    ++g_topCommitPendingHides;
    g_topCommitLastPushMs = GetTickCount64();
}

// 消费一笔推送记账；返回这个 HideCandidate 是否对应一次仍在时限内的顶字/自动上屏推送。
bool ConsumeTopCommitPendingHide(uint64_t client_id, uint64_t activation_epoch)
{
    if (g_topCommitPendingHides == 0 || client_id != g_topCommitRemainderClient ||
        activation_epoch != g_topCommitRemainderEpoch)
    {
        return false;
    }
    --g_topCommitPendingHides;
    return GetTickCount64() - g_topCommitLastPushMs <= kTopCommitHideSuppressMs;
}

void WorkerThread()
{
    // 输入节奏闸门只归这个线程：按键出队时刷新 deadline，停手到点后在同一条
    // 条件变量上醒来，把效果档放回来并排一次重查。别的线程只通过 TypingInProgress
    // 的原子量看见「正在打字」。放成循环外的局部量，闸门状态就不需要任何同步。
    InputCadence cadence;
    while (pipe_running)
    {
        Task task;
        // 闸门只服务效果档：效果档没开时连节奏状态都不维护，按键路径退回原来的
        // 无限等待，一次超时唤醒都不多，单开速度档与两档全关的行为逐字节不变。
        // 阈值是内部常数，不暴露配置：它不是可调参数，而是「打字」与「停下来看」的分界。
        const int idle_gate_ms = GetConfiguredAssocSentenceNeuralDesktop() ? kNeuralDesktopIdleGateMs : 0;
        {
            std::unique_lock lock(queueMutex);
            const auto has_work = [] { return AnyTaskQueuedLocked() || !pipe_running; };
            // 效果档在运行中被关掉时，可能残留着上一次按键置起的打字状态；在这里收掉，
            // 否则效果档会被一个不再刷新的 deadline 永远压着。
            if (idle_gate_ms <= 0 && cadence.typing())
            {
                cadence.CloseGate();
                SetTypingInProgress(false);
            }
            const auto gate_deadline = cadence.Deadline(idle_gate_ms);
            if (gate_deadline == InputCadence::Clock::time_point{})
            {
                // 闸门关着（或不在打字）：保持原来的无限等待。
                pipe_queueCv.wait(lock, has_work);
            }
            else
            {
                pipe_queueCv.wait_until(lock, gate_deadline, has_work);
            }
            if (!pipe_running)
                break;
            if (cadence.ShouldCloseGate(InputCadence::Clock::now(), idle_gate_ms))
            {
                // 到点且没有新按键：这段连续打字结束。先关闸再排重查，保证
                // recompute_candidates 里重新应用的配置看到的是 Idle。
                cadence.CloseGate();
                SetTypingInProgress(false);
                // 排队期间又关了一次闸不该再叠一个：每次都会跑一遍全量重查。
                if (!refreshEnrichmentQueued)
                {
                    refreshEnrichmentQueued = true;
                    Task refresh;
                    refresh.type = TaskType::RefreshWithEnrichment;
                    PushTaskLocked(std::move(refresh));
                }
            }
            // wait 与上面的超时分支都保证此刻队列非空；这里只是防御 TakeNextTaskLocked
            // 的 front() 没有空检查，别在后续改动里把「到点但没排到重查」变成空队列取任务。
            if (!AnyTaskQueuedLocked())
                continue;
            task = TakeNextTaskLocked();
            // 出队即放行：之后完成的批次结果不一定被这次重查读到，得能再排一个。
            if (task.type == TaskType::ApplyRescoredOrder)
                rescoreApplyQueued = false;
            if (task.type == TaskType::RefreshWithEnrichment)
                refreshEnrichmentQueued = false;
        }
        LatchCandidateUiInteraction();

        if (task.type == TaskType::ClientDeactivated || task.type == TaskType::ClientSuspended)
        {
            if (!IsPipeActivationCurrent(0, task.activation_epoch))
            {
                FTB_DIAG_LOGF(L"task {} epoch={} rejected as stale",
                              task.type == TaskType::ClientDeactivated ? L"ClientDeactivated" : L"ClientSuspended",
                              task.activation_epoch);
                CAND_DIAG_LOGF(L"candidate lifecycle task rejected stale type={} epoch={}",
                               task.type == TaskType::ClientDeactivated ? L"deactivated" : L"suspended",
                               task.activation_epoch);
                continue;
            }
        }
        else if (task.client_id != 0 && task.activation_epoch != 0 &&
                 !IsPipeActivationCurrent(task.client_id, task.activation_epoch))
        {
            // Every task carrying an owner is rejected after a focus/session
            // transition, including UI-originated candidate actions.
            // 这条丢弃无其他日志；排查丢键时先看这里（2026-09 曾疑似顶字丢键，探针证实
            // 该路径并未触发，日志留作以后定位任务消失的入口）。
            CAND_DIAG_LOGF(L"task stale-dropped type={} client={} task_epoch={} current_epoch={}",
                           static_cast<int>(task.type), task.client_id, task.activation_epoch,
                           GetActivePipeClient().epoch);
            continue;
        }

        if (task.type == TaskType::ImeKeyEvent)
        {
            // 只给真正会被受理的按键记账：被判 stale 丢弃的键（焦点/会话已经翻页）不该压住
            // 效果档一次，也不该让它去排一次注定被跳过的重查。用入队时间而不是出队时间，
            // worker 被别的任务拖住时闸门不会把「实际已经停手」的那段时间也算成打字。
            cadence.NoteKey(task.enqueued_at, idle_gate_ms);
            SetTypingInProgress(cadence.typing());
        }

        const bool candidateUiAction =
            task.type == TaskType::UiCommitCandidate || task.type == TaskType::UiPinCandidate ||
            task.type == TaskType::UiDeleteCandidate || task.type == TaskType::UiFixCandidatePosition ||
            task.type == TaskType::UiClearCandidatePosition || task.type == TaskType::UiPageUp ||
            task.type == TaskType::UiPageDown || task.type == TaskType::UiCommitCandidateAt ||
            task.type == TaskType::UiExpandCandidates || task.type == TaskType::UiCloseTranslationPanel ||
            task.type == TaskType::UiCommitExpansionPanelItem;
        if (candidateUiAction && !CandidateUiOwnerIsCurrent({task.client_id, task.activation_epoch}))
        {
            // The page was hidden or replaced after the click was posted.
            continue;
        }

        if (task.has_pipe_data)
        {
            namedpipeData = task.pipe_data;
            ApplyUiLessFromPacket(namedpipeData);
        }

        const TaskClass task_class = ClassifyTask(task.type);
        // Microsoft's scheduling guidance: the thread that processes user input
        // should run at Above normal. Only boost Foreground tasks; UserAction /
        // Enrichment / Maintenance keep the process's Normal class.
        ThreadPriority::ScopedBoost foreground_boost(THREAD_PRIORITY_ABOVE_NORMAL, NeedsInputPriorityBoost(task_class));
        switch (task.type)
        {
        case TaskType::ShowCandidate: {
            static int cnt = 0;
            // A TSF ShowCandidate packet owns a complete candidate payload,
            // including the caret anchor. Read it here rather than inside
            // PrepareCandidateList: that function is also used for server-side
            // refreshes (creating-word progress and candidate context-menu
            // actions), whose current packet does not carry a valid point.
            ::ReadDataFromNamedPipe(0b111111);
            CAND_DIAG_LOGF(L"task ShowCandidate client={} epoch={} request={} caret=({},{}) input_units={}",
                           task.client_id, task.activation_epoch, task.pipe_data.request_id, Global::Point[0],
                           Global::Point[1], GlobalIme::composition.raw_input_with_cases.size());
            if (FanyImeIpc::IsCaretPrefixEmpty(g_inputSession->prefix_end(),
                                               g_inputSession->get_pinyin_sequence_with_cases().size()))
            {
                // R4：光标前缀为空时 DLL 的 Show 事件不得唤醒一个空候选窗。
                HideCandidateWindowAndDropItems();
                break;
            }
            PrepareCandidateList(task.client_id, task.activation_epoch);
            RequestShowCandidateWindow();
            break;
        }

        case TaskType::HideCandidate: {
            ::ReadDataFromNamedPipe(0b100000);
            const double queue_elapsed_ms = QueueWaitMilliseconds(task);
            CAND_DIAG_LOGF(L"task HideCandidate client={} epoch={} request={} queued_ms={:.3f}", task.client_id,
                           task.activation_epoch, task.pipe_data.request_id, queue_elapsed_ms);
            // 顶字/自动上屏推送引发的 TSF HideCandidateWnd：余码组合还活着，绝不能 ClearState，
            // 否则用户刚敲下的那个字母就从服务端组合里消失了。无论组合是否为空都消费一笔记账，
            // 组合为空（用户没有抢敲）时照常走下面的清理。
            const bool pushed_hide = ConsumeTopCommitPendingHide(task.client_id, task.activation_epoch);
            const bool top_commit_remainder_alive = pushed_hide && !g_inputSession->get_pinyin_sequence().empty();
            if (top_commit_remainder_alive)
            {
                CAND_DIAG_LOGF(L"task HideCandidate suppressed (top-commit remainder alive) client={} epoch={}",
                               task.client_id, task.activation_epoch);
                RequestShowCandidateWindow();
                break;
            }
            // Only a hide this thread delivered late can belong to a keystroke the
            // user has already typed past — that is the one worth holding briefly,
            // because a show for a later keystroke is right behind it. A hide
            // delivered on time is a real commit or focus loss and must take effect
            // now; delaying it leaves the candidate window hanging after the word is
            // already on screen, which is exactly the snap that typing loses.
            PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, queue_elapsed_ms >= kCandidateHideBacklogMs ? 1 : 0, 0);
            /* 清理状态 */
            ClearState();
            break;
        }

        case TaskType::HideCaretState:
            PostHideCaretState();
            break;

        case TaskType::MoveCandidate: {
            static int cnt = 0;
            ::ReadDataFromNamedPipe(0b001000);
            CAND_DIAG_LOGF(L"task MoveCandidate client={} epoch={} caret=({},{})", task.client_id,
                           task.activation_epoch, Global::Point[0], Global::Point[1]);
            PostCaretStatePosition(Global::Point[0], Global::Point[1]);
            bool expected = false;
            if (!g_candidate_move_msg_pending.compare_exchange_strong(expected, true))
            {
                CAND_DIAG_LOGF(L"move request coalesced caret=({},{})", Global::Point[0], Global::Point[1]);
                break;
            }
            if (!PostMessage(::global_hwnd, WM_MOVE_CANDIDATE_WINDOW, 0, 0))
            {
                g_candidate_move_msg_pending.store(false);
            }
            break;
        }

        case TaskType::ImeKeyEvent: {
            const double queue_elapsed_ms = QueueWaitMilliseconds(task);
            if (queue_elapsed_ms >= kSlowQueueThresholdMs)
            {
                // 只记类别、请求标识和等待时长：按键内容、候选和文本一律不进诊断日志。
                DIAG_LOGF(L"[key-latency] side=server stage=queue class={} request={} client={} epoch={} "
                          L"elapsed_ms={:.3f}",
                          TaskClassName(task_class), task.pipe_data.request_id, task.client_id, task.activation_epoch,
                          queue_elapsed_ms);
            }
            // 顶字后第 4 码丢失的定位探针：DLL 侧 keydown-sent 已确认发出，若这里没打出来，
            // 说明任务根本没进队列（reader 未收到/未入队）；打出来了但组合没变，才是 HandleImeKey 内部问题。
            CAND_DIAG_LOGF(L"task ImeKeyEvent dispatch request={} keycode=0x{:X} wch=U+{:04X} epoch={}",
                           task.pipe_data.request_id, task.pipe_data.keycode, static_cast<unsigned>(task.pipe_data.wch),
                           task.activation_epoch);
            HandleImeKey(task.client_id, task.activation_epoch, task.pipe_data.request_id);
            break;
        }

        case TaskType::LangbarRightClick: {
            ::ReadDataFromNamedPipe(0b001101);
            // Snapshot the icon rect here: later key and candidate tasks reuse
            // these globals before a deferred menu show gets replayed.
            auto *icon = new (std::nothrow) RECT{Global::Point[0], Global::Point[1], static_cast<LONG>(Global::Keycode),
                                                 static_cast<LONG>(Global::ModifiersDown)};
            if (icon && !PostMessage(::global_hwnd_menu, WM_LANGBAR_RIGHTCLICK, 0, reinterpret_cast<LPARAM>(icon)))
                delete icon;
            break;
        }

        // Clients send the three switch events only after negotiating
        // CaretStateIndicator; they are badge requests, not state updates.
        case TaskType::IMESwitch: {
            const auto trigger = FanyImeUi::DecodeInputModeTrigger(task.pipe_data.wch);
            const bool imeEnabled = task.pipe_data.keycode != 0;
            const auto capsLockSnapshot =
                FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(task.pipe_data.modifiers_down);
            if (capsLockSnapshot.has_value())
                SyncServerCapsLockState(*capsLockSnapshot);
            const bool capsLockEnabled =
                capsLockSnapshot.has_value() ? *capsLockSnapshot : GetServerCapsLockState() != 0;
            const bool japaneseMode = GetActiveInputMode() == "japanese";
            if (FanyImeUi::ShouldShowInputModeEvent(trigger, GetConfiguredCaretStateIndicatorOnFocus(), capsLockEnabled,
                                                    imeEnabled, japaneseMode))
            {
                PostCaretStateBadge(FanyImeUi::InputModeBadge(imeEnabled, japaneseMode, capsLockEnabled),
                                    task.pipe_data.point[0], task.pipe_data.point[1]);
            }
            break;
        }

        case TaskType::PuncSwitch: {
            PostCaretStateBadge(FanyImeUi::PunctuationBadge(task.pipe_data.keycode != 0, task.pipe_data.wch != 0,
                                                            GetActiveInputMode() == "japanese"),
                                task.pipe_data.point[0], task.pipe_data.point[1]);
            break;
        }

        case TaskType::DoubleSingleByteSwitch: {
            PostCaretStateBadge(
                FanyImeUi::SingleStateBadge(FanyImeUi::CaretStateKind::Width, task.pipe_data.keycode != 0),
                task.pipe_data.point[0], task.pipe_data.point[1]);
            break;
        }

        case TaskType::ApplyCloudCandidate: {
            ApplyCloudCandidate(task.cloud_candidate, task.cloud_pinyin, task.cloud_generation, task.online_query);
            break;
        }

        case TaskType::ApplyAiCandidate: {
            ApplyAiCandidate(task.ai_candidate, task.ai_identity, task.ai_generation, task.online_query);
            break;
        }

        case TaskType::ApplyEnglishCandidates: {
            ApplyEnglishCandidates(std::move(task.english_candidates), task.english_input, task.english_generation);
            break;
        }

        case TaskType::ApplyCandidateTranslations: {
            ApplyCandidateTranslations(std::move(task.translation_results), task.translation_generation,
                                       task.translation_merge);
            break;
        }

        case TaskType::ApplyMixedCandidates: {
            ApplyMixedCandidates(std::move(task.mixed_result), task.mixed_input, task.mixed_generation);
            break;
        }

        case TaskType::StoreUserPhrase: {
            const auto session = task.phrase_session;
            if (task.session_pinyin_is_canonical)
            {
                session->store_user_phrase_from_canonical_pinyin(task.session_pinyin, task.session_word);
            }
            else
            {
                session->store_user_phrase(task.session_pinyin, task.session_word);
            }
            session->reset_cache();
            break;
        }

        case TaskType::ClientActivated: {
            CAND_DIAG_LOGF(L"client activated client={} epoch={} hwnd_present={}", task.client_id,
                           task.activation_epoch, ::global_hwnd != nullptr);
            VoiceInput::SetImeActive(true);
            PostHideCaretState();
            // Activation replaces all composition/candidate state from the
            // previous focus session. A terminal TIP activation also makes
            // the configured floating toolbar visible. Re-activation after a
            // suspension is idempotent and therefore does not flash it.
            // PostMessage to a null window is not a no-op: it delivers to this
            // pipe thread's own queue, where nothing reads it. Defer instead, so
            // an activation that races window creation is replayed rather than
            // swallowed.
            if (::global_hwnd)
            {
                PostMessage(::global_hwnd, WM_IMEACTIVATE, 0, 0);
                PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
            }
            else
            {
                g_deferred_client_activation.store(true, std::memory_order_release);
            }
            ClearState();

            // Show the incoming client's own mode now rather than the outgoing
            // client's, which would otherwise stay up for the whole handoff and
            // indefinitely if this client never sends another snapshot.
            int remembered = RecallClientStatusSnapshot(task.client_id);
            if (remembered >= 0)
            {
                ReloadImeConfigIfChanged();
                if (IsConfiguredImeModeScopeGlobal())
                {
                    remembered = (EnsureAuthoritativeCnMode() << 2) | (remembered & 0x3);
                }
                PublishStatusSnapshotValue(remembered);
            }
            break;
        }

        case TaskType::ClientDeactivated: {
            CAND_DIAG_LOGF(L"client deactivated client={} epoch={}", task.client_id, task.activation_epoch);
            VoiceInput::SetImeActive(false);
            PostHideCaretState();
            // Unlike a route-only suspension, terminal TIP deactivation means
            // the user switched to another input method. Forget the previous
            // global authority so switching back starts from default_ime_mode
            // instead of restoring the mode used before deactivation.
            g_authoritative_cn_mode = -1;
            g_force_global_ime_sync = true;
            // Supersede any activation still waiting for the window, otherwise
            // the replay would resurrect a toolbar the user just switched away
            // from. Without a window the flag is already false, so only the
            // deferred activation needs cancelling.
            g_deferred_client_activation.store(false, std::memory_order_release);
            if (::global_hwnd)
            {
                PostMessage(::global_hwnd, WM_IMEDEACTIVATE, 0, 0);
                PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
            }
            ClearState();
            break;
        }

        case TaskType::ClientSuspended: {
            CAND_DIAG_LOGF(L"client suspended client={} epoch={}", task.client_id, task.activation_epoch);
            PostHideCaretState();
            // A suspension rotates the IPC focus session while the TIP may
            // still own thread focus. It clears candidates just like terminal
            // deactivation, but never changes floating-toolbar visibility.
            PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
            ClearState();
            break;
        }

        case TaskType::StatusSnapshot: {
            // 设置页改字形只写配置文件，server 在这里重读它；切到繁体时在后台预载转换字典。组字途中改了字形，
            // 当前候选按新字形换写法。
            if (ReloadImeConfigIfChanged())
            {
                if (GetConfiguredCharacterSet() == "traditional")
                    ChineseConverter::PreloadInBackground();
                RespellCandidatesForCharacterSetChange();
            }
            const int cn_state = task.pipe_data.keycode != 0 ? 1 : 0;
            const int fullwidth_state = task.pipe_data.modifiers_down != 0 ? 1 : 0;
            const int punctuation_state = task.pipe_data.pinyin_length != 0 ? 1 : 0;
            int effective_cn = cn_state;
            const bool force_global_sync = g_force_global_ime_sync;
            g_force_global_ime_sync = false;

            // client_id is pid<<32|tid, so every window of one Chromium/Electron
            // host reports the same id: a change here means the focused TSF
            // thread changed, not merely the focused window.

            if (IsConfiguredImeModeScopeGlobal())
            {
                const int authoritative = EnsureAuthoritativeCnMode();
                const bool client_changed =
                    g_last_status_snapshot_client_id != 0 && task.client_id != g_last_status_snapshot_client_id;
                if (client_changed || force_global_sync)
                {
                    // Focus moved to another app: keep the unified mode. After
                    // switching back from another IME, the authority was reset
                    // on ClientDeactivated and is seeded from default_ime_mode.
                    effective_cn = authoritative;
                    if (cn_state != authoritative && task.client_id != 0)
                    {
                        // The toolbar is moved to the authority below whether or
                        // not the tip accepts this packet, so a rejected switch
                        // leaves the two indicators disagreeing.
                        SendToTsfWorkerThreadClientViaNamedpipe(
                            task.client_id, task.activation_epoch,
                            authoritative != 0 ? Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToCn
                                               : Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToEn,
                            L"");
                    }
                }
                else
                {
                    // Same client (or first report): accept as the new authority.
                    g_authoritative_cn_mode = cn_state;
                    effective_cn = cn_state;
                }
            }
            else
            {
                g_authoritative_cn_mode = cn_state;
            }

            if (task.client_id != 0)
            {
                g_last_status_snapshot_client_id = task.client_id;
            }

            const int caps_state = GetServerCapsLockState();
            const int packed_state =
                (caps_state << 3) | (effective_cn << 2) | (fullwidth_state << 1) | punctuation_state;
            if (FanyImeIpc::ShouldResetCompositionForImeMode(effective_cn != 0))
            {
                if (effective_cn == 0)
                    SetEnglishInputMode(false);
                PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
                ClearState();
            }
            RememberClientStatusSnapshot(task.client_id, packed_state);
            PublishStatusSnapshotValue(packed_state);
            break;
        }

        case TaskType::ExitEnglishInputMode: {
            if (g_english_input_mode)
            {
                SetEnglishInputMode(false);
                PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
                ClearState();
            }
            break;
        }

        case TaskType::ToolbarLanguageToggle: {
            HandleToolbarLanguageToggle(task.toolbar_from_ime_enabled);
            break;
        }

        case TaskType::UiCommitCandidate:
        case TaskType::UiCommitCandidateAt: {
            int index_in_page = task.candidate_one_based_index - 1;
            if (task.type == TaskType::UiCommitCandidateAt)
            {
                index_in_page = MoveToExpandedCandidate(task.candidate_absolute_index, task.candidate_expected_text);
                if (index_in_page < 0)
                {
                    break;
                }
            }
            ProcessSelectionKey(0, task.client_id, task.activation_epoch, index_in_page);
            if (Global::MsgTypeToTsf == Global::DataFromServerMsgType::Normal)
            {
                // The worker packet is the complete edit-session-owned commit
                // for a normal UI click. Do not also leave an unsolicited
                // request-id-0 reply on the normal reverse pipe.
                if (SendToTsfWorkerThreadClientViaNamedpipe(
                        task.client_id, task.activation_epoch,
                        Global::DataFromServerMsgTypeToTsfWorkerThread::CommitCandidate,
                        Global::candidate_ui.selected_text))
                {
                    ClearState();
                }
            }
            else if (SendCurrentDataToClient(task.client_id, task.activation_epoch, 0))
            {
                // NeedToCreateWord/OutOfRange require the normal reply's
                // subtype. An empty worker packet is only the ordered trigger;
                // TSF consumes (rather than discards) the id-0 reply.
                SendToTsfWorkerThreadClientViaNamedpipe(task.client_id, task.activation_epoch,
                                                        Global::DataFromServerMsgTypeToTsfWorkerThread::CommitCandidate,
                                                        L"");
            }
            break;
        }

        case TaskType::UiCommitExpansionPanelItem: {
            // 面板点选与数字键同一条上屏内核；面板已不在或下标越界时什么都不投递，
            // 与「点击落空」一致。成功时内核已置好 selected_text。
            if (!CommitCandidateExpansionPanelSenseText(task.candidate_one_based_index - 1, 0))
            {
                break;
            }
            if (SendToTsfWorkerThreadClientViaNamedpipe(task.client_id, task.activation_epoch,
                                                        Global::DataFromServerMsgTypeToTsfWorkerThread::CommitCandidate,
                                                        Global::candidate_ui.selected_text))
            {
                ClearState();
            }
            break;
        }

        case TaskType::UiPinCandidate:
        case TaskType::UiDeleteCandidate:
        case TaskType::UiFixCandidatePosition:
        case TaskType::UiClearCandidatePosition: {
            WordItem item;
            if (!ResolveCandidateItem(task.candidate_one_based_index, item))
            {
                break;
            }
            if (metasequoia::local_modes::is_date_time_menu_item(item))
            {
                break;
            }
            if (item.source == CandidateSource::DateTime)
            {
                ApplyDateTimeCandidateUiAction(task.type, item, task.fixed_position);
                if (g_date_time_page_active)
                    RebuildDateTimeCandidatePage();
                else
                    PrepareCandidateList(task.client_id, task.activation_epoch);
                RequestShowCandidateWindow();
                break;
            }
            if (item.source == CandidateSource::QuickPhrase || item.source == CandidateSource::Emoji ||
                item.source == CandidateSource::Kaomoji || item.source == CandidateSource::Symbol ||
                item.source == CandidateSource::Generated)
            {
                break;
            }
            // 日文候选没有拼音词库的行：置顶会按罗马音键写进拼音日志，升级回放时变成中文候选（#869）。
            if (task.type == TaskType::UiPinCandidate && item.scheme == SchemeType::JapaneseRomaji)
            {
                break;
            }

            const bool english_candidate = item.source == CandidateSource::EnglishDictionary;
            const auto ranking_keys = RankingKeysForCandidate(item);
            const std::string context_key = english_candidate ? EnglishRankingContextKey() : ranking_keys.first;
            const std::string entry_key = english_candidate ? item.pinyin : ranking_keys.second;

            if (task.type == TaskType::UiPinCandidate)
            {
                if (english_candidate)
                {
                    (void)user_dictionary::adjust_english_candidate_ranking(
                        CommonUtils::get_ime_data_path() + "\\english.db", user_dictionary::default_user_db_path(),
                        context_key, Global::candidate_ui.items, entry_key, item.word, "pin", 1, 1, true);
                    // 上面只把它排到英文里的第一；混输里还要把这个输入的英文槽位挪到首位。
                    if (!g_english_input_mode && !IsYModeInput(g_inputSession->get_pinyin_sequence_with_cases()))
                        (void)user_dictionary::set_english_slot(user_dictionary::default_user_db_path(),
                                                                EnglishInputKey(), 0);
                }
                else
                    (void)user_dictionary::adjust_candidate_ranking(
                        CommonUtils::get_ime_data_path() + "\\msime.db", user_dictionary::default_user_db_path(),
                        context_key, FanyImeIpc::FirstVariantsOnly(Global::candidate_ui.items), entry_key, item.word,
                        "pin", 1, 1, true, nullptr,
                        item.scheme == SchemeType::Wubi ? user_dictionary::DictionaryKind::Wubi
                                                        : user_dictionary::DictionaryKind::Pinyin);
            }
            else if (task.type == TaskType::UiDeleteCandidate)
            {
                if (english_candidate)
                    (void)user_dictionary::delete_english_candidate(CommonUtils::get_ime_data_path() + "\\english.db",
                                                                    user_dictionary::default_user_db_path(), entry_key,
                                                                    item.word);
                else if (utf8::distance(item.word.begin(), item.word.end()) == 1)
                {
                    break;
                }
                else
                {
                    // Pinyin candidates carry both the typed code and, when
                    // available, the canonical quanpin database key.  Delete
                    // with the canonical key so a raw shuangpin sequence is
                    // not mistaken for an equally valid quanpin spelling.
                    const std::string delete_pinyin =
                        item.canonical_pinyin.empty() ? item.pinyin : item.canonical_pinyin;
                    g_inputSession->remove_candidate(delete_pinyin, item.word, item.scheme);
                }
            }
            else if (task.type == TaskType::UiFixCandidatePosition)
            {
                (void)user_dictionary::set_fixed_position(user_dictionary::default_user_db_path(), context_key,
                                                          entry_key, item.word, task.fixed_position);
            }
            else
            {
                (void)user_dictionary::clear_fixed_position(user_dictionary::default_user_db_path(), context_key,
                                                            entry_key, item.word);
            }
            g_inputSession->reset_cache();
            g_inputSession->recompute_candidates();
            PrepareCandidateList(task.client_id, task.activation_epoch);
            RequestShowCandidateWindow();
            break;
        }

        case TaskType::UiPageUp:
        case TaskType::UiPageDown: {
            const int offset = (task.type == TaskType::UiPageDown) ? 1 : -1;
            // A coalesced burst of wheel notches replays as several page moves
            // and a single refresh at the end, so a fast scroll costs one redraw
            // rather than one per notch.
            bool refresh = false;
            for (int step = 0; step < task.page_steps; ++step)
            {
                const PageMoveResult moved = MoveCandidatePage(offset);
                if (moved == PageMoveResult::Unchanged)
                {
                    break;
                }
                refresh = true;
                if (moved == PageMoveResult::Moved)
                {
                    // Mouse paging restarts the highlight at the top of the new
                    // page; the pointer, unlike the arrow keys, carries no
                    // notion of which row the user was on.
                    Global::candidate_ui.selected_index_in_page = 0;
                }
            }
            if (refresh)
            {
                RefreshCandidatePageUi(true);
            }
            break;
        }

        case TaskType::UiExpandCandidates: {
            if (Global::candidate_ui.items.empty())
            {
                break;
            }
            // 用户在浏览展开面板，保持已显示的顺序，新展开的候选只追加在后面。
            Global::candidate_ui.begin_selection();
            ExpandCandidatesKeepingPagePosition();
            // 刚打开面板时即使没有新候选，也要重新发布一次，候选页才带上后续候选。
            RefreshCandidatePageUi(true);
            break;
        }

        case TaskType::UiCloseTranslationPanel: {
            // 展开面板被点了外面：关掉状态并重新发布不带面板的一帧（presenter 已经
            // 把 Popup 从场景里摘掉，这里只让服务端状态跟上）。
            if (!Global::candidate_ui.expansion_panel_active)
            {
                break;
            }
            CloseCandidateExpansionPanel();
            RefreshCandidatePageUi(true);
            break;
        }

        case TaskType::ReloadInputSession: {
            ClearState();
            Global::candidate_ui.page_size = GetConfiguredCandidatePageSize();
            g_language_cycle_parked_session.reset();
            g_inputSession = CreateInputSessionFromConfig();
            Global::candidate_ui.set_items({});
            PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
            break;
        }

        case TaskType::EnsureInputSessionMatchesConfig: {
            const SchemeType wanted = GetConfiguredActiveInputScheme();
            const bool has_session = g_inputSession != nullptr;
            const bool configured_scheme_matches = has_session && g_inputSession->current_scheme_type() == wanted;
            const bool session_is_japanese =
                has_session && g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji;
            if (FanyImeIpc::InputSessionMatchesConfig(configured_scheme_matches, g_r_mode_triggered,
                                                      session_is_japanese))
            {
                break;
            }
            ClearState();
            Global::candidate_ui.page_size = GetConfiguredCandidatePageSize();
            g_language_cycle_parked_session.reset();
            g_inputSession = CreateInputSessionFromConfig();
            Global::candidate_ui.set_items({});
            PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
            break;
        }

        case TaskType::ApplyCandidatePageSize: {
            // 界面线程重读到配置变化后发来这个任务；字形也可能一起变了，那样就按新字形换写法。
            const int pageSize = GetConfiguredCandidatePageSize();
            const bool page_size_changed = Global::candidate_ui.page_size != pageSize;
            if (page_size_changed)
            {
                Global::candidate_ui.page_size = pageSize;
                Global::candidate_ui.page_index = 0;
                Global::candidate_ui.clear_page();
            }
            if (RespellCandidatesForCharacterSetChange() || !page_size_changed)
                break;
            const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
            if (owner && IsPipeActivationCurrent(owner.client_id, owner.activation_epoch))
            {
                RefreshCandidatePageUi(true);
            }
            break;
        }

        case TaskType::RefreshCandidatePage: {
            // 悬浮工具栏切换字形后发来这个任务：组字途中换了字形就按新字形换写法。
            if (RespellCandidatesForCharacterSetChange())
                break;
            if (!Global::candidate_ui.items.empty())
            {
                Global::candidate_ui.clear_page();
                const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
                if (owner && IsPipeActivationCurrent(owner.client_id, owner.activation_epoch))
                {
                    RefreshCandidatePageUi(true);
                }
            }
            break;
        }

        case TaskType::ResetInputSessionCache: {
            const auto session = PersistentInputSession();
            if (session)
            {
                session->reset_cache();
            }
            break;
        }

        case TaskType::ApplyRescoredOrder: {
            ApplyRescoredOrder();
            break;
        }

        case TaskType::RefreshWithEnrichment: {
            // 停手到点后的唯一一次重查：recompute_candidates 会重新执行 ApplyConfiguration，
            // 此时 TypingInProgress 已清零，效果档按配置值放行，打字期间缺的整句候选才补得回来。
            // 排队期间用户又开始了下一段打字：这次重查已经过期，跳过它，别让一次全量重查
            // 落在一颗正在等的键上；下一键的 ApplyConfiguration 会重新压住效果档，不用收尾。
            if (TypingInProgress())
                break;
            // 重查被跳过时效果档必须放回来，否则这条组合剩下的展开/翻页查询都按打字中的
            // 配置走，停手后整句候选依然不出现。
            const auto restore_effect_tier = [] {
                if (g_inputSession)
                    g_inputSession->refresh_configuration();
            };
            if (!g_inputSession)
                break;
            // 守卫和 ApplyRescoredOrder 一致：造词/译文页占着、用户已开始选词、候选页已经
            // 不在了，都不该拿引擎结果去覆盖界面。英文输入与日语等非拼音方案没有可重排的
            // 拼音整句，而且 PrepareCandidateList 在英文模式下会故意发出空页，直接跳过。
            // 五笔本身没有拼音整句，但开了拼音混输时同一个混合查询会把全拼词典的整句一并
            // 算出来（见 ImeSession::refresh_candidates 的混输分支），停手后同样要补这一次。
            const SchemeType scheme = g_inputSession->current_scheme_type();
            const bool pinyin_sentence_scheme = scheme == SchemeType::Quanpin || scheme == SchemeType::Shuangpin ||
                                                (scheme == SchemeType::Wubi && GetConfiguredWubiMixedPinyin());
            if (g_english_input_mode || !pinyin_sentence_scheme || !CanMergeAsyncCandidates() ||
                GlobalIme::composition.creating_word.active || Global::candidate_ui.items.empty())
            {
                restore_effect_tier();
                break;
            }
            const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
            if (!owner || !IsPipeActivationCurrent(owner.client_id, owner.activation_epoch))
            {
                restore_effect_tier();
                break;
            }
            // Global::candidate_ui.items 是按键时那份副本，重查后必须重新组页才看得到新的
            // 整句候选；只刷新页面会把旧列表再发一遍。与 ApplyRescoredOrder 共用同一段。
            (void)RequeryCandidatesPreservingOnline(owner);
            break;
        }
        }
    }

    // 进程停在打字中时退出，闸门状态不能留给下一个读它的人。
    SetTypingInProgress(false);
    ShutdownPipeClients();
    WakeNamedPipeListenersForShutdown();
}

void EnqueueTask(TaskType type, const FanyImeNamedpipeData &pipeData, uint64_t activation_epoch)
{
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = type;
        task.has_pipe_data = true;
        task.pipe_data = pipeData;
        task.client_id = pipeData.client_id;
        task.activation_epoch = activation_epoch;
        task.enqueued_at = std::chrono::steady_clock::now();
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueCloudCandidate(const std::string &candidate, const std::string &pinyin, uint64_t generation)
{
    const AsyncRequestOrigin origin = FindCloudRequestOrigin(pinyin, generation);
    if (origin.client_id == 0 || origin.activation_epoch == 0)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyCloudCandidate;
        task.cloud_candidate = candidate;
        task.cloud_pinyin = pinyin;
        task.cloud_generation = generation;
        task.online_query = origin.engine_query;
        task.client_id = origin.client_id;
        task.activation_epoch = origin.activation_epoch;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

// 后台线程算完了一批整句重排。它不知道自己算的是不是当前这次输入——那要在任务线程上、拿着
// g_inputSession 才判断得了——所以这里只负责把「去看一眼」排进队列，判断留给 ApplyRescoredOrder。
void EnqueueRescoredCandidates()
{
    {
        std::lock_guard lock(queueMutex);
        if (rescoreApplyQueued)
            return;
        rescoreApplyQueued = true;
        Task task;
        task.type = TaskType::ApplyRescoredOrder;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueAiCandidate(const std::string &candidate, const std::string &identity, uint64_t generation)
{
    const AsyncRequestOrigin origin = FindAiRequestOrigin(identity, generation);
    if (origin.client_id == 0 || origin.activation_epoch == 0)
        return;
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyAiCandidate;
        task.ai_candidate = candidate;
        task.ai_identity = identity;
        task.ai_generation = generation;
        task.online_query = origin.engine_query;
        task.client_id = origin.client_id;
        task.activation_epoch = origin.activation_epoch;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueEnglishCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation)
{
    const AsyncRequestOrigin origin = FindEnglishRequestOrigin(input, generation);
    if (origin.client_id == 0 || origin.activation_epoch == 0)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyEnglishCandidates;
        task.english_candidates = std::move(candidates);
        task.english_input = input;
        task.english_generation = generation;
        task.client_id = origin.client_id;
        task.activation_epoch = origin.activation_epoch;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueCandidateTranslations(std::vector<EnglishIme::TranslationResult> results, uint64_t generation, bool merge)
{
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyCandidateTranslations;
        task.translation_results = std::move(results);
        task.translation_generation = generation;
        task.translation_merge = merge;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueMixedCandidates(MixedCandidates::Result result, const std::string &input, uint64_t generation)
{
    const AsyncRequestOrigin origin = FindMixedRequestOrigin(input, generation);
    if (origin.client_id == 0 || origin.activation_epoch == 0)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyMixedCandidates;
        task.mixed_result = std::move(result);
        task.mixed_input = input;
        task.mixed_generation = generation;
        task.client_id = origin.client_id;
        task.activation_epoch = origin.activation_epoch;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueStoreUserPhraseTask(const std::string &pinyin, const std::string &word, bool pinyin_is_canonical)
{
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::StoreUserPhrase;
        task.session_pinyin = pinyin;
        task.session_word = word;
        task.session_pinyin_is_canonical = pinyin_is_canonical;
        // A language cycle can replace the active language session before this
        // queued learning task runs. Store into the session that chose the word.
        task.phrase_session = PersistentInputSession();
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

// Learning writes (frequency adjustments, entered English words) run on their own low-priority
// thread. On the worker they sat in the same FIFO as the keystrokes: every commit and fsync of the
// user journal -- plus msime.db once a pick crosses the trigger count -- was paid before the next
// key, and a busy disk or antivirus scan turned that into a visible stall. The writes only touch
// user_dictionary, which opens its own connection per thread, never the input session; the one
// session effect, dropping the engine's candidate cache after a ranking change, is posted back to
// the worker. Jobs stay in submission order, and Stop() drains them so no pick is lost at exit.
namespace
{
SerialTaskRunner &DictionaryWriter()
{
    struct Writer
    {
        Writer()
        {
            // The checkpoint copies the journal's write-ahead log back into the database, which SQLite would otherwise
            // do on whichever commit crosses its threshold -- inside a selection the user is waiting for. Here it runs
            // once the queue is empty, i.e. after the user stops typing.
            runner.SetIdleHandler([] { user_dictionary::checkpoint_user_journal_if_needed(); });
        }
        SerialTaskRunner runner{/*below_normal_priority=*/true};
    };
    static Writer writer;
    return writer.runner;
}
} // namespace

// The candidates are copied because the page moves on before the write runs. Called from the
// worker thread only, which is also what serializes the replay guard.
void EnqueueAdjustCandidateRankingTask(bool english, bool wubi, const std::string &context_key,
                                       const std::string &entry_key, const std::string &word, uint64_t client_id,
                                       uint64_t activation_epoch, const std::vector<WordItem> *ranking_candidates)
{
    static FanyImeIpc::SelectionRankingReplayGuard replay_guard;
    const std::string replay_key =
        std::string(english ? "e" : (wubi ? "w" : "p")) + '\x1f' + context_key + '\x1f' + entry_key + '\x1f' + word;
    if (!replay_guard.should_apply(replay_key, client_id, activation_epoch, GetTickCount64()))
    {
        CAND_DIAG_LOGF(L"candidate-ranking-replay-skipped client={} epoch={}", client_id, activation_epoch);
        return;
    }
    const auto &frequency = GetConfiguredFrequencyAdjustment();
    DictionaryWriter().Post([english, wubi, context_key,
                             candidates = FanyImeIpc::FirstVariantsOnly(
                                 ranking_candidates ? *ranking_candidates : Global::candidate_ui.items),
                             entry_key, word, mode = frequency.mode, linear_step = frequency.linear_step,
                             trigger_count = frequency.trigger_count] {
        if (english)
        {
            (void)user_dictionary::adjust_english_candidate_ranking(
                CommonUtils::get_ime_data_path() + "\\english.db", user_dictionary::default_user_db_path(), context_key,
                candidates, entry_key, word, mode, linear_step, trigger_count, false);
            return;
        }
        bool ranking_changed = false;
        (void)user_dictionary::adjust_candidate_ranking(
            CommonUtils::get_ime_data_path() + "\\msime.db", user_dictionary::default_user_db_path(), context_key,
            candidates, entry_key, word, mode, linear_step, trigger_count, false, &ranking_changed,
            wubi ? user_dictionary::DictionaryKind::Wubi : user_dictionary::DictionaryKind::Pinyin);
        if (ranking_changed)
        {
            EnqueueResetInputSessionCacheTask();
        }
    });
}

// 快捷短语每次都现查，不进引擎的候选缓存，写完不用清缓存。
void EnqueueLearnQuickPhraseOrderTask(const std::string &code, const std::string &word, bool first_in_group,
                                      int ordinary_rank, uint64_t client_id, uint64_t activation_epoch)
{
    static FanyImeIpc::SelectionRankingReplayGuard replay_guard;
    const std::string replay_key = "q\x1f" + code + '\x1f' + (word.empty() ? std::to_string(ordinary_rank) : word);
    if (code.empty() || !replay_guard.should_apply(replay_key, client_id, activation_epoch, GetTickCount64()))
        return;
    const auto &frequency = GetConfiguredFrequencyAdjustment();
    const int max_slot = (std::max)(GetConfiguredCandidatePageSize() - 1, 0);
    DictionaryWriter().Post([code, word, first_in_group, ordinary_rank, max_slot, mode = frequency.mode,
                             linear_step = frequency.linear_step, trigger_count = frequency.trigger_count] {
        if (word.empty())
        {
            (void)user_dictionary::learn_quick_phrase_bypass(user_dictionary::default_user_db_path(), code,
                                                             ordinary_rank, mode, linear_step, trigger_count, max_slot);
            return;
        }
        (void)user_dictionary::learn_quick_phrase_selection(CommonUtils::get_ime_data_path() + "\\msime.db",
                                                            user_dictionary::default_user_db_path(), code, word,
                                                            first_in_group, mode, linear_step, trigger_count);
    });
}

// 英文槽位同样每次现查，写完不用清缓存。selected_index 为空表示选中的就是占槽位的英文。
void EnqueueLearnEnglishSlotTask(const std::string &code, int english_index, std::optional<int> selected_index,
                                 uint64_t client_id, uint64_t activation_epoch)
{
    static FanyImeIpc::SelectionRankingReplayGuard replay_guard;
    const std::string replay_key = "s\x1f" + code + '\x1f' + std::to_string(english_index) + '\x1f' +
                                   (selected_index ? std::to_string(*selected_index) : std::string("english"));
    if (code.empty() || !replay_guard.should_apply(replay_key, client_id, activation_epoch, GetTickCount64()))
        return;
    const auto &frequency = GetConfiguredFrequencyAdjustment();
    const int max_slot = EnglishSlotMaximum();
    DictionaryWriter().Post([code, english_index, selected_index, max_slot, mode = frequency.mode,
                             linear_step = frequency.linear_step, trigger_count = frequency.trigger_count] {
        if (selected_index)
        {
            (void)user_dictionary::learn_english_slot_bypass(user_dictionary::default_user_db_path(), code,
                                                             english_index, *selected_index, mode, linear_step,
                                                             trigger_count, max_slot);
            return;
        }
        (void)user_dictionary::learn_english_slot_selection(user_dictionary::default_user_db_path(), code,
                                                            english_index, mode, linear_step, trigger_count);
    });
}

// 日期时间格式的顺序同样每次现查，写完不用清缓存。
void EnqueueLearnDateTimeOrderTask(const std::string &format_id, uint64_t client_id, uint64_t activation_epoch)
{
    static FanyImeIpc::SelectionRankingReplayGuard replay_guard;
    const std::string keyword = DateTimeKeywordForFormat(format_id);
    if (keyword.empty() ||
        !replay_guard.should_apply("d\x1f" + format_id, client_id, activation_epoch, GetTickCount64()))
        return;
    const auto &frequency = GetConfiguredFrequencyAdjustment();
    DictionaryWriter().Post([format_id, category = metasequoia::local_modes::date_time_category(keyword),
                             default_ids = metasequoia::local_modes::date_time_format_ids(keyword),
                             mode = frequency.mode, linear_step = frequency.linear_step,
                             trigger_count = frequency.trigger_count] {
        (void)user_dictionary::learn_date_time_selection(user_dictionary::default_user_db_path(), category, default_ids,
                                                         format_id, mode, linear_step, trigger_count);
    });
}

void EnqueueLearnEnteredEnglishWordTask(const std::string &word)
{
    if (word.empty())
        return;
    DictionaryWriter().Post([word] {
        (void)user_dictionary::learn_entered_english_word(CommonUtils::get_ime_data_path() + "\\english.db",
                                                          user_dictionary::default_user_db_path(), word);
    });
}

void ShutdownDictionaryWriter()
{
    DictionaryWriter().Stop();
}

void EnqueuePipeSessionInvalidatedTask(uint64_t client_id, uint64_t invalidation_epoch)
{
    if (!pipe_running || client_id == 0 || invalidation_epoch == 0)
    {
        return;
    }

    FanyImeNamedpipeData disconnectData = {};
    disconnectData.event_type = FanyImePipeEventType::ClientSuspended;
    disconnectData.client_id = client_id;
    EnqueueTask(TaskType::ClientSuspended, disconnectData, invalidation_epoch);
}

void EnqueueCandidateUiAction(CandidateUiAction action, int one_based_index, int fixed_position)
{
    if (!pipe_running || one_based_index <= 0 || one_based_index > 10)
    {
        return;
    }

    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (!owner)
    {
        return;
    }

    TaskType type = TaskType::UiCommitCandidate;
    if (action == CandidateUiAction::Pin)
    {
        type = TaskType::UiPinCandidate;
    }
    else if (action == CandidateUiAction::Delete)
    {
        type = TaskType::UiDeleteCandidate;
    }
    else if (action == CandidateUiAction::FixPosition)
    {
        if (fixed_position < 1 || fixed_position > 5)
            return;
        type = TaskType::UiFixCandidatePosition;
    }
    else if (action == CandidateUiAction::ClearPosition)
    {
        type = TaskType::UiClearCandidatePosition;
    }
    else if (action == CandidateUiAction::PageUp || action == CandidateUiAction::PageDown)
    {
        // Paging has no candidate index; it goes through EnqueueCandidateUiPaging.
        return;
    }

    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = type;
        task.client_id = owner.client_id;
        task.activation_epoch = owner.activation_epoch;
        task.candidate_one_based_index = one_based_index;
        task.fixed_position = fixed_position;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueCandidateUiPaging(CandidateUiAction action, int steps)
{
    if (!pipe_running || steps <= 0)
    {
        return;
    }
    if (action != CandidateUiAction::PageUp && action != CandidateUiAction::PageDown)
    {
        return;
    }

    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (!owner)
    {
        return;
    }

    const TaskType type = action == CandidateUiAction::PageUp ? TaskType::UiPageUp : TaskType::UiPageDown;
    {
        std::lock_guard lock(queueMutex);
        // One notch of the wheel is one step, and every step can hit the engine
        // and the user dictionary. Folding a burst into the task already waiting
        // for the same page keeps a fast scroll from queueing an unbounded run.
        // 合并只看同类队列的队尾：队列分成四条之后，全队列的最新一格可能是别的类别。
        std::queue<Task> &queue = TaskQueueFor(type);
        if (!queue.empty() && queue.back().type == type && queue.back().client_id == owner.client_id &&
            queue.back().activation_epoch == owner.activation_epoch)
        {
            queue.back().page_steps += steps;
        }
        else
        {
            Task task;
            task.type = type;
            task.client_id = owner.client_id;
            task.activation_epoch = owner.activation_epoch;
            task.page_steps = steps;
            PushTaskLocked(std::move(task));
        }
    }
    pipe_queueCv.notify_one();
}

void EnqueueCandidateUiCommitAt(int absolute_index, std::wstring expected_text)
{
    if (!pipe_running || absolute_index < 0 || expected_text.empty())
    {
        return;
    }
    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (!owner)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::UiCommitCandidateAt;
        task.client_id = owner.client_id;
        task.activation_epoch = owner.activation_epoch;
        task.candidate_absolute_index = absolute_index;
        task.candidate_expected_text = std::move(expected_text);
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueCandidateUiCommitExpansionPanel(int panel_index)
{
    if (!pipe_running || panel_index < 0)
    {
        return;
    }
    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (!owner)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::UiCommitExpansionPanelItem;
        task.client_id = owner.client_id;
        task.activation_epoch = owner.activation_epoch;
        task.candidate_one_based_index = panel_index + 1;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueCandidateUiExpand()
{
    if (!pipe_running)
    {
        return;
    }
    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (!owner)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        // 连续滚到底部只需要展开一次。同样只看它自己那类队列的队尾。
        std::queue<Task> &queue = TaskQueueFor(TaskType::UiExpandCandidates);
        if (!queue.empty() && queue.back().type == TaskType::UiExpandCandidates &&
            queue.back().client_id == owner.client_id && queue.back().activation_epoch == owner.activation_epoch)
        {
            return;
        }
        Task task;
        task.type = TaskType::UiExpandCandidates;
        task.client_id = owner.client_id;
        task.activation_epoch = owner.activation_epoch;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueCandidateUiCloseTranslationPanel()
{
    if (!pipe_running)
    {
        return;
    }
    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (!owner)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        // 连点外面只需要关一次。同样只看它自己那类队列的队尾。
        std::queue<Task> &queue = TaskQueueFor(TaskType::UiCloseTranslationPanel);
        if (!queue.empty() && queue.back().type == TaskType::UiCloseTranslationPanel &&
            queue.back().client_id == owner.client_id && queue.back().activation_epoch == owner.activation_epoch)
        {
            return;
        }
        Task task;
        task.type = TaskType::UiCloseTranslationPanel;
        task.client_id = owner.client_id;
        task.activation_epoch = owner.activation_epoch;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueReloadInputSessionTask()
{
    if (!pipe_running)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ReloadInputSession;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueEnsureInputSessionMatchesConfigTask()
{
    if (!pipe_running)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::EnsureInputSessionMatchesConfig;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueApplyCandidatePageSizeTask()
{
    if (!pipe_running)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyCandidatePageSize;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueRefreshCandidatePageTask()
{
    if (!pipe_running)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::RefreshCandidatePage;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueResetInputSessionCacheTask()
{
    if (!pipe_running)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ResetInputSessionCache;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueExitEnglishInputModeTask()
{
    if (!pipe_running)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ExitEnglishInputMode;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueToolbarLanguageToggleTask(bool from_ime_enabled)
{
    if (!pipe_running)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ToolbarLanguageToggle;
        task.toolbar_from_ime_enabled = from_ime_enabled;
        PushTaskLocked(std::move(task));
    }
    pipe_queueCv.notify_one();
}
} // namespace FanyNamedPipe
