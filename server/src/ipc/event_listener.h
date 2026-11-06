#pragma once

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <cstdint>
#include "session/input_session.h"
#include "english/english_ime.h"
#include "mixed/mixed_candidates.h"
#include "engine/contracts/temporary_japanese_input.h"

inline std::condition_variable pipe_queueCv;
inline std::atomic_bool pipe_running = true;
inline std::shared_ptr<IInputSession> g_inputSession;
// Collapse bursty TSF show/move packets so the UI thread lays out once against
// the latest Global::Point / CandidateString instead of on every cloud refresh
// or layout notification.
inline std::atomic<bool> g_candidate_show_msg_pending{false};
inline std::atomic<bool> g_candidate_move_msg_pending{false};

namespace FanyNamedPipe
{
FanyImeTemporaryJapanese::Policy TemporaryJapanesePolicy();
std::wstring TemporaryJapanesePolicyPayload();
enum class CandidateUiAction
{
    Commit,
    Pin,
    Delete,
    FixPosition,
    ClearPosition,
    PageUp,
    PageDown,
};

void WorkerThread();
void EventListenerLoopThread();
void AuxPipeEventListenerLoopThread();
void TsfDiagnosticPipeEventListenerLoopThread();
void ToTsfPipeEventListenerLoopThread();
void ToTsfWorkerThreadPipeEventListenerLoopThread();

// Delivers an activation that arrived before the candidate window existed. Call
// once the window is available; a no-op when nothing was deferred.
void ReplayDeferredClientActivation();

void PrepareCandidateList(uint64_t client_id, uint64_t activation_epoch);
void ClearState();
void RegisterStatusSnapshotWindow(HWND toolbar_window);
void EnqueueCloudCandidate(const std::string &candidate, const std::string &pinyin, uint64_t generation);
void EnqueueAiCandidate(const std::string &candidate, const std::string &identity, uint64_t generation);
// 神经整句重排的后台线程算完一批后调这个；见 event_listener_async_candidates.cpp 的 ApplyRescoredOrder。
void EnqueueRescoredCandidates();
void CancelCloudCandidateRequest();
void EnqueueEnglishCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation);
void EnqueueCandidateTranslations(std::vector<EnglishIme::TranslationResult> results, uint64_t generation,
                                  bool merge = false);
void EnqueueMixedCandidates(MixedCandidates::Result result, const std::string &input, uint64_t generation);
void EnqueueCandidateUiAction(CandidateUiAction action, int one_based_index, int fixed_position = 0);
// 鼠标开始操作当前显示的候选页时，通知工作线程保持这批候选的顺序。界面线程调用，只记下页面
// 代次，由工作线程在处理下一个任务前认领（见 LatchCandidateUiInteraction）。
void NoteCandidateUiInteraction(uint64_t page_generation);
// Paging carries a step count rather than a candidate index. Steps coalesce
// into a page task already queued for the same client, so spinning the wheel
// cannot outrun the worker with a long run of engine-backed page moves.
void EnqueueCandidateUiPaging(CandidateUiAction action, int steps);
// 卷轴展开面板里点选了一个候选：absolute_index 是它在整份候选里的下标，expected_text 是界面上
// 显示的文字。工作线程先核对两者，顺序变了就按文字重新找，找不到就什么也不做。
void EnqueueCandidateUiCommitAt(int absolute_index, std::wstring expected_text);
// 卷轴展开面板打开或滚到底部：向会话再要一批候选，并重新发布带后续候选的候选页。
void EnqueueCandidateUiExpand();
// 竖排译文面板被点了外面：关掉面板状态并重新发布不带面板的一帧。
void EnqueueCandidateUiCloseTranslationPanel();
// 候选词展开面板里点选了某条译义：panel_index 是它在面板里的下标（0 起），与数字键 1..N 上屏
// 走同一条路径；面板已经不在或下标越界时什么也不做。
void EnqueueCandidateUiCommitExpansionPanel(int panel_index);
void EnqueuePipeSessionInvalidatedTask(uint64_t client_id, uint64_t invalidation_epoch);
void EnqueueReloadInputSessionTask();
void EnqueueEnsureInputSessionMatchesConfigTask();
void EnqueueApplyCandidatePageSizeTask();
void EnqueueRefreshCandidatePageTask();
void EnqueueResetInputSessionCacheTask();
void EnqueueExitEnglishInputModeTask();
// from_ime_enabled: the clicked toolbar icon showed the IME open (中/日) rather than English.
void EnqueueToolbarLanguageToggleTask(bool from_ime_enabled);
// Finishes the learning writes still queued (frequency adjustments, entered English words) and
// stops their thread. Call after the worker thread has been joined, so nothing posts new writes.
void ShutdownDictionaryWriter();
} // namespace FanyNamedPipe
