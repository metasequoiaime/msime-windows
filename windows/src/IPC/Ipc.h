#pragma once

#include "KeyEventSendResult.h"
#include "VoiceCompositionPipe.h"
#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../../../engine/contracts/windows_ipc.h"
#include "../../../engine/contracts/direct_helpcode.h"
#include "../../../engine/contracts/v_mode_input.h"
#include "../../../engine/contracts/trilingual_input.h"

int InitIpc();
int InitNamedpipe();
int ConnectToAllNamedpipe();
int ConnectToTsfNamedpipe();
int CloseIpc();
int CloseNamedpipe();
void ResetNamedpipeReplyState();
HANDLE GetToTsfWorkerThreadNamedpipe();
void BindNamedpipeFocusState(
    _In_ const void *owner, _In_opt_ bool *focusResetPending, _In_opt_ bool *activationRequired,
    _In_opt_ std::atomic<uint64_t> *expectedWorkerFocusToken, _In_opt_ std::atomic<bool> *localSessionResetPending,
    _In_opt_ std::atomic<UINT> *localSessionResetToken, _In_opt_ std::atomic<bool> *workerCommitReady,
    _In_opt_ std::atomic<uint64_t> *acknowledgedWorkerFocusToken, _In_opt_ std::atomic<HANDLE> *workerPipeHandle,
    _In_opt_ std::atomic<UINT> *workerPipeGeneration);
// Wake-up events owned by the bound service, so the UI thread and the IPC worker
// thread block instead of polling. workerAckEvent is signalled by the worker when
// it records the expected focus token; workerPipePublishedEvent is signalled when
// a worker pipe handle is published. Either may be null, which falls back to the
// original polling. Only the current owner may bind; UnbindNamedpipeFocusState
// clears them, and the owner must rebind null before closing the handles.
void BindNamedpipeWakeEvents(_In_ const void *owner, _In_opt_ HANDLE workerAckEvent,
                             _In_opt_ HANDLE workerPipePublishedEvent);
void UnbindNamedpipeFocusState(_In_ const void *owner);
bool IsNamedpipeFocusStateOwner(_In_ const void *owner);
UINT BeginNamedpipeLocalSessionReset();
void InvalidateNamedpipeWorkerGeneration();
void MarkNamedpipeFocusLost();
void RequireNamedpipeFocusActivation();
void MarkNamedpipeSessionDirty();
bool MarkNamedpipeSessionDirtyForOwner(_In_ const void *owner);
bool EnsureNamedpipeFocusSessionActivated();
bool SupportsCharacterSetShortcut();
bool SupportsCompositionRestore();
bool SupportsCaretStateIndicator();
bool SupportsTrilingualCycle();
bool FlushNamedpipeFocusSessionReset();
bool FlushNamedpipeImeDeactivation(uint64_t focusToken = 0);

//
// For shared memory
//
int WriteDataToSharedMemory(           //
    UINT keycode,                      // VkCode
    WCHAR wch,                         // Unicode character converted from vkcode
    UINT modifiers_down,               //
    const int point[2],                //
    int pinyin_length,                 //
    const std::wstring &pinyin_string, //
    UINT write_flag                    //
);
KeyEventSendResult SendKeyEventToUIProcess(_Out_opt_ uint64_t *requestId = nullptr);
void DebugTsfKeyLatency(_In_z_ const wchar_t *stage, uint64_t requestId, double elapsedMs, HRESULT result);
void DebugTsfIssue47(_In_z_ const wchar_t *stage, uint64_t requestId, UINT code, WCHAR wch, UINT category,
                     UINT function, int eaten, BOOL composing, size_t virtualKeyLength, HRESULT result,
                     uint64_t correlationToken = 0);
void QueueTsfDiagnosticLog(const std::wstring &line);
int SendHideCandidateWndEventToUIProcess();
int SendShowCandidateWndEventToUIProcess();
int SendMoveCandidateWndEventToUIProcess();
int SendLangbarRightClickEventToUIProcess(const RECT *prcArea);
int SendIMEActivationEventToUIProcessViaNamedPipe();
int SendIMEDeactivationEventToUIProcessViaNamedPipe();
int SendClientActivatedEventToServerViaNamedPipe(uint64_t focusToken);
int SendClientDeactivatedEventToServerViaNamedPipe(uint64_t focusToken = 0);
int SendClientSuspendedEventToServerViaNamedPipe();
int SendIMEStatusSnapshotToUIProcessViaNamedPipe(bool kbdIsOpen, bool fullwidthIsOpen, bool puncIsOpen,
                                                 bool assertsFocusOwnership = false);
int SendIMEStatusEventToUIProcessViaNamedPipe(bool kbdIsOpen, bool fullwidthIsOpen, bool puncIsOpen);
// eventType is IMESwitch, PuncSwitch or DoubleSingleByteSwitch. No-op unless
// the Server negotiated FanyImeProtocol::CaretStateIndicator.
int SendCaretStateSwitchEventToUIProcessViaNamedPipe(UINT eventType, bool enabled, POINT anchor, UINT trigger,
                                                     bool capsLockEnabled, bool imeOpen);

bool SendToAuxNamedpipe(const std::wstring &pipeData, bool waitForAcknowledgement = false);

//
// For named pipe
//
int WriteDataToNamedPipe(              //
    UINT keycode,                      //
    WCHAR wch,                         //
    UINT modifiers_down,               //
    const int point[2],                //
    int pinyin_length,                 //
    const std::wstring &pinyin_string, //
    UINT write_flag                    //
);
KeyEventSendResult SendKeyEventToUIProcessViaNamedPipe(_Out_opt_ uint64_t *requestId = nullptr);
int SendHideCandidateWndEventToUIProcessViaNamedPipe();
int SendHideCaretStateEventToUIProcessViaNamedPipe();
int SendShowCandidateWndEventToUIProcessViaNamedPipe();
int SendMoveCandidateWndEventToUIProcessViaNamedPipe();
int SendLangbarRightClickEventToUIProcessViaNamedPipe(const RECT *prcArea);
// Best-effort read of the Server-published current candidate page (comma-
// separated). Used in UILess mode so ITfCandidateListUIElement::GetString can
// return real candidates after PrepareCandidateList has written shared memory.
bool TryReadCandidatePageFromSharedMemory(_Out_ std::wstring *candidatePage);
// When abortTransportOnTimeout is false, a missed reply leaves the pipe up and
// returns a non-TransportUnavailable empty frame for the caller to fall back.
struct FanyImeNamedpipeDataToTsf *TryReadDataFromServerPipeWithTimeout(uint64_t expectedRequestId,
                                                                       bool abortTransportOnTimeout);
// Replies that commit text (candidate selection, candidate + punctuation) get
// a longer budget than ordinary keys. The request has already been written, so
// a miss is DeliveryAmbiguous: the Server may have committed the selection.
// Waiting for the same request_id is the only way to learn which candidate it
// chose; a slow machine (battery, throttled disk) routinely needs more than
// the ordinary 50ms. A miss past this budget drops the key and resets the
// transport; the selection is never sent a second time.
constexpr DWORD FANY_IME_COMMIT_REPLY_TIMEOUT_MS = 300;
struct FanyImeNamedpipeDataToTsf *TryReadCommitReplyFromServerPipe(uint64_t expectedRequestId);
// True when requestId names a request that was actually written to the Server,
// i.e. a lost reply is DeliveryAmbiguous rather than DefinitelyNotSent.
inline bool IsDeliveredServerRequestId(uint64_t requestId)
{
    return requestId != FANY_IME_NO_REQUEST_ID && requestId != FANY_IME_UNSOLICITED_REQUEST_ID;
}
// Edit-session result for a commit whose reply never arrived although the
// request was delivered. The key is dropped with the composition and never
// sent again (DeferredKeyFailureReason::DeliveryAmbiguous).
constexpr HRESULT FANY_E_COMMIT_REPLY_AMBIGUOUS = __HRESULT_FROM_WIN32(ERROR_TIMEOUT);
// A key handler applied its key, but the result differs from what the deferred
// FIFO projected for the keys queued behind it. Maps to ProjectionInvalidated.
constexpr HRESULT FANY_S_PROJECTION_INVALIDATED = MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_ITF, 0x0301);

//
// Modifiers:
//     0b00000001: Shift
//     0b00000010: Control
//     0b00000100: Alt
// TODO: Make it able to denote explicit modifiers, e.g. LShift, RShift, we could use left keys
//
namespace Global
{
inline thread_local UINT Keycode = 0;
inline thread_local WCHAR wch = L'\0';
inline thread_local UINT ModifiersDown = 0;
inline thread_local int Point[2] = {100, 100};
inline thread_local int PinyinLength = 0;
inline thread_local std::wstring PinyinString = L"";

// TF_TMF_UIELEMENTENABLEDONLY at ActivateEx, and/or BeginUIElement pbShow=FALSE.
inline thread_local bool HostUiLessMode = false;
inline thread_local bool CandidateUiLessMode = false;
inline bool IsUiLessMode()
{
    return HostUiLessMode || CandidateUiLessMode;
}

inline thread_local int firefox_like_cnt = 0; // Apps like firefox, e.g. firefox, zen...
inline thread_local std::wstring current_process_name = L"";

inline thread_local wchar_t app_name[512] = {0};

namespace DataFromServerMsgType = FanyImeReplyType;

namespace DataToTsfWorkerThreadMsgType = FanyImeWorkerReplyType;

namespace PunctuationLock
{
constexpr int Follow = 0;
constexpr int AlwaysChinese = 1;
constexpr int AlwaysEnglish = 2;
} // namespace PunctuationLock

inline std::atomic<int> PunctuationLockMode{PunctuationLock::Follow};

inline BOOL ResolvePunctuationOpen(BOOL followImeOpen)
{
    switch (PunctuationLockMode.load(std::memory_order_relaxed))
    {
    case PunctuationLock::AlwaysChinese:
        return TRUE;
    case PunctuationLock::AlwaysEnglish:
        return FALSE;
    default:
        return followImeOpen;
    }
}

inline bool IsPunctuationLocked()
{
    return PunctuationLockMode.load(std::memory_order_relaxed) != PunctuationLock::Follow;
}

inline std::atomic_bool PagingCommaPeriodEnabled{false};
// Default off: the whole smart-punctuation family ships disabled and is only
// enabled by an explicit user opt-in; the Server overwrites this on connect.
inline std::atomic_bool SmartPunctuationEnabled{false};
// Default off until the Server sends the persisted setting.
inline std::atomic_bool SmartPunctuationRepeatToChineseEnabled{false};
// Default off: a space right after a committed Chinese punctuation converts it
// to its ASCII form (plain punctuation and individually-occurring pairs).
inline std::atomic_bool SmartPunctuationSpaceConvertEnabled{false};
// Default off: direct ASCII output for ',' '.' ':' after ASCII digits. Gated
// in CCompositionProcessorEngine::ResolvePunctuation.
inline std::atomic_bool SmartPunctuationDirectDigitEnabled{false};
// Default off: direct ASCII output for ',' '.' ':' after ASCII letters. Gated
// in CCompositionProcessorEngine::ResolvePunctuation.
inline std::atomic_bool SmartPunctuationDirectLetterEnabled{false};
// Default on until the Server sends the persisted setting.
inline std::atomic_bool PairedPunctuationEnabled{true};
inline std::atomic_bool MicrosoftShuangpinEnabled{false};
// 双拼句中辅助码开着、勾了反引号且当前是双拼：组合中的反引号按
// engine/contracts/mid_sentence_helpcode.h 的形状规则当编码键吃掉，见 CompositionProcessorEngine_KeyClassify.cpp。
inline std::atomic_bool MidSentenceHelpcodeEnabled{false};
// 同上，换成分号触发键（设置里多选）。分号触发的段在按键缓冲里同样记成反引号。
inline std::atomic_bool MidSentenceHelpcodeSemicolonEnabled{false};
// 句中辅助码的大写触发生效：完整音节后的大写字母自己开一段。字母键照常吃，只改变反引号/分号与 ; 韵母
// 判断时这一节怎么数（大写段不算键），规则见 engine/contracts/mid_sentence_helpcode.h。
inline std::atomic_bool MidSentenceHelpcodeUppercaseEnabled{false};
// 双拼直接辅助码（万象式）开着且当前是双拼：四码后的 / 当编码键吃掉，; 韵母不再看奇偶，规则见
// engine/contracts/direct_helpcode.h。
inline std::atomic_bool DirectHelpcodeEnabled{false};
// 直接辅助码开着时 / 是不是四码的终止键（设置里可以只留「第二位辅码大写」）。没勾时 / 仍按标点处理。
inline std::atomic_bool DirectHelpcodeSlashEnabled{true};
// V 模式（数字转中文、算式计算）由哪个前缀开启：双拼只认大写 V，全拼 V、v 都认，关着或不是全拼/双拼时
// 不开。V 后面的数字和 . + - * / ( ) 按 engine/contracts/v_mode_input.h 的规则当编码键吃掉。
inline std::atomic<FanyImeVModeInput::Trigger> VModeTrigger{FanyImeVModeInput::Trigger::Off};
inline std::atomic_bool JapaneseInputModeEnabled{false};
// Enabled only after a supporting Server sends the persisted setting.
inline std::atomic_bool TrilingualCycleEnabled{false};
inline std::atomic_bool CapsLockEnabled{false};
inline std::atomic_bool TsfDiagnosticLogEnabled{false};
// Default off, like the persisted setting: until the Server sends the switch on
// connect, the capture paths classify nothing and never touch the stats pipe.
inline std::atomic_bool StatisticsEnabled{false};
inline thread_local bool g_connected = false;

} // namespace Global
