// The hidden message window: CMetasequoiaIME_WindowProc and its handlers, plus the theme registry
// watcher and language-bar icon refresh that post to it.

#include "Private.h"
#include "Globals.h"
#include "MetasequoiaIME.h"
#include "CandidateListUIPresenter.h"
#include "CompositionProcessorEngine.h"
#include "Compartment.h"
#include "define.h"
#include <debugapi.h>
#include <namedpipeapi.h>
#include <winnt.h>
#include <winuser.h>
#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include "FanyLog.h"
#include "Ipc.h"
#include "CommonUtils.h"
#include "Global/FanyDefines.h"
#include "Utils/FanyUtils.h"
#include "../Utils/PerfTimer.h"
#include "MetasequoiaIMEInternal.h"

using namespace metasequoia_ime_detail;

namespace
{
constexpr UINT REFRESH_LANG_BAR_THEME_DELAY_MS = 150;
constexpr UINT CONNECT_NAMEDPIPE_MAX_RETRY_INTERVAL_MS = 2000;

void SendCurrentImeStatusSnapshot(CMetasequoiaIME *pIME, bool assertsFocusOwnership = false)
{
    if (!Global::g_connected || !pIME || !pIME->GetCompositionProcessorEngine() || !pIME->_GetThreadMgr())
    {
        // Distinct from the drop inside the pipe layer: the compartments are
        // never even read here, so no value was available to resend later.
        return;
    }

    if (assertsFocusOwnership)
    {
        // The claim makes the Server re-route to this client, so it must be
        // backed by the one authority that cannot be held by two threads at
        // once. Document focus alone is not enough: an app may call SetFocus
        // on a background thread manager.
        BOOL hasThreadFocus = FALSE;
        if (FAILED(pIME->_GetThreadMgr()->IsThreadFocus(&hasThreadFocus)) || !hasThreadFocus)
        {
            return;
        }
    }

    CCompositionProcessorEngine *engine = pIME->GetCompositionProcessorEngine();
    SendIMEStatusSnapshotToUIProcessViaNamedPipe(
        engine->GetIMEMode(pIME->_GetThreadMgr(), pIME->_GetClientId()),
        engine->GetDoubleSingleByteMode(pIME->_GetThreadMgr(), pIME->_GetClientId()),
        engine->GetPunctuationMode(pIME->_GetThreadMgr(), pIME->_GetClientId()), assertsFocusOwnership);
}

// Applies a Server-pushed "commit this text, then keep the letters the user typed past it"
// delivery (CommitCandidateAndContinue). The Server decides when to send it; this session owns
// the edit-session and focus/epoch validation, delegating the actual composition surgery to
// CMetasequoiaIME::_HandleCommitCandidateAndContinue.
class CCommitCandidateAndContinueEditSession : public CEditSessionBase
{
  public:
    CCommitCandidateAndContinueEditSession(CMetasequoiaIME *pTextService, ITfContext *pContext, std::wstring payload,
                                           uint64_t focusToken, uint64_t compositionEpoch)
        : CEditSessionBase(pTextService, pContext), _payload(std::move(payload)), _focusToken(focusToken),
          _compositionEpoch(compositionEpoch)
    {
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        // Focus or composition may have moved between the posted message and this grant; the
        // delivery belongs to whichever session scheduled it and must not land in the next one.
        if (!_pTextService->_IsFocusSessionCurrent(_focusToken, _pContext) ||
            !_pTextService->_IsCompositionEpochCurrent(_compositionEpoch))
        {
            return S_FALSE;
        }
        return _pTextService->_HandleCommitCandidateAndContinue(ec, _pContext, _payload);
    }

  private:
    std::wstring _payload;
    uint64_t _focusToken;
    uint64_t _compositionEpoch;
};

} // namespace

//+---------------------------------------------------------------------------
//
// Theme registry watcher / language bar icon refresh
//
//----------------------------------------------------------------------------

void CMetasequoiaIME::_RefreshLanguageBarThemeIcons()
{
    CCompositionProcessorEngine *pEngine = GetCompositionProcessorEngine();
    if (pEngine)
    {
        pEngine->RefreshLanguageBarIcons();
    }
}

void CMetasequoiaIME::_RequestLanguageBarCapsIconRefresh()
{
    if (_msgWndHandle && IsWindow(_msgWndHandle))
    {
        PostMessage(_msgWndHandle, WM_RefreshLanguageBarTheme, 1, 0);
    }
}

void CMetasequoiaIME::_StartThemeRegistryWatcher()
{
    if (_pThemeWatcherThread)
    {
        return;
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_NOTIFY, &_themeRegKey) != ERROR_SUCCESS)
    {
        return;
    }

    _themeRegEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!_themeRegEvent)
    {
        RegCloseKey(_themeRegKey);
        _themeRegKey = nullptr;
        return;
    }

    if (RegNotifyChangeKeyValue(_themeRegKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, _themeRegEvent, TRUE) != ERROR_SUCCESS)
    {
        CloseHandle(_themeRegEvent);
        _themeRegEvent = nullptr;
        RegCloseKey(_themeRegKey);
        _themeRegKey = nullptr;
        return;
    }

    _stopThemeWatcher.store(false);
    _pThemeWatcherThread = new (std::nothrow) std::thread([this]() {
        while (!_stopThemeWatcher.load(std::memory_order_acquire))
        {
            const DWORD wait = WaitForSingleObject(_themeRegEvent, INFINITE);
            if (_stopThemeWatcher.load(std::memory_order_acquire))
            {
                break;
            }
            if (wait != WAIT_OBJECT_0)
            {
                continue;
            }
            if (_stopThemeWatcher.load(std::memory_order_acquire))
            {
                break;
            }

            ResetEvent(_themeRegEvent);
            RegNotifyChangeKeyValue(_themeRegKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, _themeRegEvent, TRUE);

            const HWND ownerWindow = _msgWndHandle;
            if (ownerWindow && IsWindow(ownerWindow))
            {
                PostMessage(ownerWindow, WM_RefreshLanguageBarTheme, 0, 0);
            }
        }
    });

    if (!_pThemeWatcherThread)
    {
        CloseHandle(_themeRegEvent);
        _themeRegEvent = nullptr;
        RegCloseKey(_themeRegKey);
        _themeRegKey = nullptr;
    }
}

void CMetasequoiaIME::_StopThemeRegistryWatcher()
{
    _stopThemeWatcher.store(true, std::memory_order_release);
    if (_themeRegEvent)
    {
        SetEvent(_themeRegEvent);
    }

    if (_pThemeWatcherThread)
    {
        if (_pThemeWatcherThread->joinable())
        {
            _pThemeWatcherThread->join();
        }
        delete _pThemeWatcherThread;
        _pThemeWatcherThread = nullptr;
    }

    if (_themeRegEvent)
    {
        CloseHandle(_themeRegEvent);
        _themeRegEvent = nullptr;
    }

    if (_themeRegKey)
    {
        RegCloseKey(_themeRegKey);
        _themeRegKey = nullptr;
    }

    _stopThemeWatcher.store(false, std::memory_order_release);
}

//+---------------------------------------------------------------------------
//
// CMetasequoiaIME_WindowProc
//
//----------------------------------------------------------------------------
LRESULT CALLBACK CMetasequoiaIME_WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    CMetasequoiaIME *pIME = (CMetasequoiaIME *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (!pIME)
    {
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    switch (message)
    {
    case WM_BareShiftRelease:
        pIME->_HandleHookedBareShiftRelease(static_cast<UINT>(wParam));
        return 0;

    case WM_CheckGlobalCompartment: {
        CMetasequoiaIME::WorkerCompartmentSwitch request;
        if (!pIME->_TakeWorkerCompartmentSwitch(static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken))
        {
            // The Server already moved the toolbar to the value it asked for,
            // so a rejection here leaves the two indicators disagreeing.
            break;
        }

        if (request.messageType == Global::DataToTsfWorkerThreadMsgType::SwitchToEnglish)
        {
            if (pIME->_IsComposing() || pIME->_pCandidateListUIPresenter != nullptr)
            {
                ITfDocumentMgr *documentMgr = nullptr;
                ITfContext *context = nullptr;
                if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&documentMgr)) && documentMgr)
                {
                    if (SUCCEEDED(documentMgr->GetTop(&context)) && context)
                    {
                        _KEYSTROKE_STATE keyState = {};
                        keyState.Category = CATEGORY_COMPOSING;
                        keyState.Function = FUNCTION_TOGGLE_IME_MODE;
                        if (FAILED(pIME->_InvokeKeyHandler(context, 0, L'\0', 0, keyState, FANY_IME_NO_REQUEST_ID, {},
                                                           0, request.compositionEpoch, request.focusToken)))
                        {
                            // The host refused the commit; the pipes are fine.
                            pIME->_ResetSessionAfterFailure(DeferredKeyFailureKind::Resync);
                        }
                        context->Release();
                    }
                    documentMgr->Release();
                }
            }
            pIME->GetCompositionProcessorEngine()->SetIMEMode(pIME->_GetThreadMgr(), pIME->_GetClientId(), FALSE);
        }
        else if (request.messageType == Global::DataToTsfWorkerThreadMsgType::SwitchToChinese)
        {
            pIME->GetCompositionProcessorEngine()->SetIMEMode(pIME->_GetThreadMgr(), pIME->_GetClientId(), TRUE);
        }
        else if (request.messageType == Global::DataToTsfWorkerThreadMsgType::SwitchToPuncEn)
        {
            pIME->GetCompositionProcessorEngine()->SetPunctuationMode(pIME->_GetThreadMgr(), pIME->_GetClientId(),
                                                                      FALSE);
        }
        else if (request.messageType == Global::DataToTsfWorkerThreadMsgType::SwitchToPuncCn)
        {
            pIME->GetCompositionProcessorEngine()->SetPunctuationMode(pIME->_GetThreadMgr(), pIME->_GetClientId(),
                                                                      TRUE);
        }
        else if (request.messageType == Global::DataToTsfWorkerThreadMsgType::SwitchToFullwidth)
        {
            pIME->GetCompositionProcessorEngine()->SetDoubleSingleByteMode(pIME->_GetThreadMgr(), pIME->_GetClientId(),
                                                                           TRUE);
        }
        else if (request.messageType == Global::DataToTsfWorkerThreadMsgType::SwitchToHalfwidth)
        {
            pIME->GetCompositionProcessorEngine()->SetDoubleSingleByteMode(pIME->_GetThreadMgr(), pIME->_GetClientId(),
                                                                           FALSE);
        }
        // Read all three compartments after applying the exact worker command.
        // Sending a complete ordered snapshot prevents two queued status WMs
        // from each observing/overwriting a different partial state.
        SendCurrentImeStatusSnapshot(pIME);
        break;
    }
    case WM_ConnectNamedpipe: {
        if (Global::g_connected)
        {
            PostMessage(hWnd, WM_IpcReconnect, 0, 0);
        }
        break;
    }
    case WM_DisconnectNamedpipe: {
        if (Global::g_connected)
        {
            // A newer OnSetThreadFocus already superseded this queued kill.
            // Never let a stale focus-loss message deactivate the recovered
            // client (the Win+. round-trip can produce exactly this ordering).
            break;
        }
        KillTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE);
        KillTimer(hWnd, TIMER_CONNECT_TO_TSF_NAMEDPIPE);
        FlushNamedpipeFocusSessionReset();
        pIME->_ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
        pIME->_ipcConsecutiveFailures = 0;
        break;
    }
    case WM_ConnectToTsfNamedpipe: {
        SetTimer(hWnd, TIMER_CONNECT_TO_TSF_NAMEDPIPE, CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS, nullptr);
        break;
    }
    case WM_IpcWorkerDisconnected: {
        // The worker posts this only after its OVERLAPPED read has completed,
        // so it is now safe for the UI thread to close and recreate all pipes.
        const HANDLE disconnectedPipe = reinterpret_cast<HANDLE>(wParam);
        const UINT disconnectedGeneration = static_cast<UINT>(lParam);
        if (!disconnectedPipe || disconnectedGeneration == 0 ||
            disconnectedGeneration != pIME->_workerPipeGeneration.load(std::memory_order_acquire) ||
            disconnectedPipe != GetToTsfWorkerThreadNamedpipe() ||
            pIME->_hToTsfWorkerThreadPipe.load(std::memory_order_acquire) != nullptr)
        {
            // A delayed notification from an older registration must not tear
            // down handles opened by a newer reconnect attempt.
            break;
        }
        pIME->_workerCommitReady.store(false, std::memory_order_release);
        // The Server side of this composition is gone. Cancel the local one
        // and swallow the keys queued against it; the user starts from empty.
        pIME->_ResetSessionAfterFailure(DeferredKeyFailureKind::Transport);
        CloseNamedpipe();
        if (Global::g_connected)
        {
            pIME->_ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
            SetTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE, pIME->_ipcReconnectDelayMs, nullptr);
        }
        break;
    }
    case WM_IpcSessionDirty: {
        const UINT resetToken = static_cast<UINT>(wParam);
        if (resetToken == 0)
        {
            // Worker-thread callbacks cannot access the UI thread's TLS IPC
            // binding.  Let the owner thread allocate and post an exact reset
            // token here.
            pIME->_ResetSessionAfterFailure(lParam == IPC_SESSION_DIRTY_RESYNC ? DeferredKeyFailureKind::Resync
                                                                               : DeferredKeyFailureKind::Transport);
        }
        else
        {
            pIME->_RequestLocalSessionReset(nullptr, resetToken);
        }
        break;
    }
    case WM_IpcReconnect: {
        // Main and reply-pipe failures invalidate only their own TLS handle.
        // Preserve a healthy worker read and let ConnectToAllNamedpipe reopen
        // just the missing channel(s).
        if (Global::g_connected)
        {
            const bool resetPending = pIME->_localSessionResetPending.load(std::memory_order_acquire);
            const bool activated = !resetPending && EnsureNamedpipeFocusSessionActivated();
            if (activated)
            {
                KillTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE);
                pIME->_ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
                pIME->_ipcConsecutiveFailures = 0;
                pIME->_TryLeaveServerUnavailableFallback();
                SendCurrentImeStatusSnapshot(pIME);
                pIME->_ScheduleDeferredKeyDownDrain();
                break;
            }
            pIME->_ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
            SetTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE, pIME->_ipcReconnectDelayMs, nullptr);
        }
        break;
    }
    case WM_TIMER: {
        if (wParam == TIMER_DEFERRED_FOCUS_LOSS)
        {
            KillTimer(hWnd, TIMER_DEFERRED_FOCUS_LOSS);
            pIME->_focusLossDeferPending = false;
            if (Global::g_connected)
            {
                // The document focus did not return within the deferral
                // window, so this is a real focus loss (app switch), not a
                // transient in-app route change (Excel cell-edit entry).
                MarkNamedpipeFocusLost();
                Global::g_connected = false;
                PostMessage(hWnd, WM_DisconnectNamedpipe, 0, 0);
            }
            break;
        }
        if (wParam == TIMER_FOCUS_CARET_STATE)
        {
            KillTimer(hWnd, TIMER_FOCUS_CARET_STATE);
            pIME->_AnnounceFocusedInputMode();
            break;
        }
        if (wParam == TIMER_FOCUS_STATUS_RESEND)
        {
            KillTimer(hWnd, TIMER_FOCUS_STATUS_RESEND);
            SendCurrentImeStatusSnapshot(pIME, true);
            break;
        }
        if (wParam == TIMER_PAIRED_PUNCTUATION_CARET)
        {
            // _RunPairedPunctuationCaretMove kills this timer itself once the
            // modifier chord is released, the deadline passes, or focus moves.
            pIME->_RunPairedPunctuationCaretMove();
            break;
        }
        if (wParam == TIMER_CONNECT_ALL_NAMEDPIPE)
        {
            // 如果用户已经切换走了，就不用继续重试
            if (!Global::g_connected)
            {
                KillTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE);
                pIME->_ipcConsecutiveFailures = 0;
                break;
            }

            if (pIME->_localSessionResetPending.load(std::memory_order_acquire))
            {
                const UINT resetToken = pIME->_localSessionResetToken.load(std::memory_order_acquire);
                pIME->_RequestLocalSessionReset(nullptr, resetToken);
                if (pIME->_localSessionResetPending.load(std::memory_order_acquire))
                {
                    pIME->_ipcReconnectDelayMs =
                        min(pIME->_ipcReconnectDelayMs * 2, CONNECT_NAMEDPIPE_MAX_RETRY_INTERVAL_MS);
                    SetTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE, pIME->_ipcReconnectDelayMs, nullptr);
                    break;
                }
            }

            if (EnsureNamedpipeFocusSessionActivated())
            {
                KillTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE);
                pIME->_ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
                pIME->_ipcConsecutiveFailures = 0;
                SendCurrentImeStatusSnapshot(pIME);
                pIME->_ScheduleDeferredKeyDownDrain();
                break;
            }
            // Keep retrying while this TSF thread owns UI focus. The server
            // can be restarted independently of the host application.
            pIME->_ipcReconnectDelayMs = min(pIME->_ipcReconnectDelayMs * 2, CONNECT_NAMEDPIPE_MAX_RETRY_INTERVAL_MS);
            SetTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE, pIME->_ipcReconnectDelayMs, nullptr);
            break;
        }

        if (wParam == TIMER_CONNECT_TO_TSF_NAMEDPIPE)
        {
            if (ConnectToTsfNamedpipe())
            {
                KillTimer(hWnd, TIMER_CONNECT_TO_TSF_NAMEDPIPE);
                break;
            }
            break;
        }

        if (wParam == TIMER_REFRESH_LANG_BAR_THEME)
        {
            KillTimer(hWnd, TIMER_REFRESH_LANG_BAR_THEME);
            pIME->_RefreshLanguageBarThemeIcons();
            break;
        }
        break;
    }
    case WM_IMEActivation: {
        // Retained only for message-number compatibility.  Main-pipe lifecycle
        // messages no longer control floating-toolbar visibility.
        break;
    }
    case WM_ThreadFocus: {
        // Some Chromium/Electron window switches surface only as thread-focus
        // changes. Refresh this thread's cached TSF language-bar item even when
        // there was no document-focus transition.
        pIME->_RefreshLanguageBarThemeIcons();
        SendCurrentImeStatusSnapshot(pIME);
        break;
    }
    case WM_DrainDeferredKeyDown: {
        pIME->_DrainOneDeferredKeyDown();
        break;
    }
    case WM_UpdateIMEStatus: {
        SendCurrentImeStatusSnapshot(pIME);
        break;
    }
    case WM_UpdateDoubleSingleByte: {
        SendCurrentImeStatusSnapshot(pIME);
        break;
    }
    case WM_UpdatePuncMode: {
        SendCurrentImeStatusSnapshot(pIME);
        break;
    }
    case WM_CommitCandidate: {
        CMetasequoiaIME::WorkerCandidateCommit request;
        if (!pIME->_TakeServerCandidateCommit(static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            break;
        }
        ITfDocumentMgr *pDocMgrFocus = nullptr;
        ITfContext *pContext = nullptr;

        if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
        {
            if (SUCCEEDED(pDocMgrFocus->GetTop(&pContext)) && pContext)
            {
                _KEYSTROKE_STATE KeystrokeState;
                KeystrokeState.Category = CATEGORY_CANDIDATE;
                KeystrokeState.Function = FUNCTION_FINALIZE_CANDIDATELIST;
                pIME->_InvokeKeyHandler(pContext, 0, 0, 0, KeystrokeState, FANY_IME_UNSOLICITED_REQUEST_ID,
                                        std::move(request.text), 0, request.compositionEpoch, request.focusToken);
                pContext->Release();
            }
            pDocMgrFocus->Release();
        }
        break;
    }
    case WM_CommitCandidateAndContinue: {
        CMetasequoiaIME::WorkerCandidateCommit request;
        if (!pIME->_TakeServerCandidateCommit(static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            break;
        }
        ITfDocumentMgr *pDocMgrFocus = nullptr;
        ITfContext *pContext = nullptr;

        if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
        {
            if (SUCCEEDED(pDocMgrFocus->GetTop(&pContext)) && pContext)
            {
                auto *pEditSession = new (std::nothrow) CCommitCandidateAndContinueEditSession(
                    pIME, pContext, std::move(request.text), request.focusToken, request.compositionEpoch);
                if (pEditSession)
                {
                    HRESULT editSessionHr = E_FAIL;
                    pContext->RequestEditSession(pIME->_tfClientId, pEditSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                                                 &editSessionHr);
                    pEditSession->Release();
                }
                pContext->Release();
            }
            pDocMgrFocus->Release();
        }
        break;
    }
    case WM_InsertText: {
        CMetasequoiaIME::WorkerCandidateCommit request;
        if (!pIME->_TakeServerCandidateCommit(static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            break;
        }
        ITfDocumentMgr *pDocMgrFocus = nullptr;
        ITfContext *pContext = nullptr;

        if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
        {
            if (SUCCEEDED(pDocMgrFocus->GetTop(&pContext)) && pContext)
            {
                _KEYSTROKE_STATE KeystrokeState;
                KeystrokeState.Category = CATEGORY_COMPOSING;
                KeystrokeState.Function = FUNCTION_INSERT_TEXT;
                pIME->_InvokeKeyHandler(pContext, 0, 0, 0, KeystrokeState, FANY_IME_UNSOLICITED_REQUEST_ID,
                                        std::move(request.text), 0, request.compositionEpoch, request.focusToken);
                pContext->Release();
            }
            pDocMgrFocus->Release();
        }
        break;
    }
    case WM_UpdateVoiceComposition:
        pIME->_DispatchUnsolicitedVoiceText(wParam, FUNCTION_UPDATE_VOICE_COMPOSITION);
        break;
    case WM_CommitVoiceComposition:
        pIME->_DispatchUnsolicitedVoiceText(wParam, FUNCTION_COMMIT_VOICE_COMPOSITION);
        break;
    case WM_CancelVoiceComposition:
        pIME->_DispatchUnsolicitedVoiceText(wParam, FUNCTION_CANCEL_VOICE_COMPOSITION);
        break;
    case WM_AsyncFinalizeCandidate: {
        CMetasequoiaIME::AsyncKeyRequest request;
        if (!pIME->_TakeAsyncKeyRequest(WM_AsyncFinalizeCandidate, static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            pIME->_FailDeferredKey(request.deferredReplayToken, DeferredKeyFailureReason::Superseded);
            break;
        }
        PerfTimer timer;
        ITfDocumentMgr *pDocMgrFocus = nullptr;
        ITfContext *pContext = nullptr;

        bool handedOffReplay = false;
        if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
        {
            if (SUCCEEDED(pDocMgrFocus->GetTop(&pContext)) && pContext)
            {
                _KEYSTROKE_STATE KeystrokeState;
                KeystrokeState.Category = CATEGORY_CANDIDATE;
                KeystrokeState.Function = FUNCTION_FINALIZE_CANDIDATELIST;
                pIME->_InvokeKeyHandler(pContext, request.code, request.wch, 0, KeystrokeState, request.requestId, {},
                                        0, request.compositionEpoch, request.focusToken, request.deferredReplayToken);
                handedOffReplay = true;
                pContext->Release();
            }
            pDocMgrFocus->Release();
        }
        if (!handedOffReplay)
        {
            pIME->_FailDeferredKey(request.deferredReplayToken, DeferredKeyFailureReason::AsyncPostFailed);
        }
        break;
    }
    case WM_AsyncPunctuationCommit: {
        CMetasequoiaIME::AsyncKeyRequest request;
        if (!pIME->_TakeAsyncKeyRequest(WM_AsyncPunctuationCommit, static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            pIME->_FailDeferredKey(request.deferredReplayToken, DeferredKeyFailureReason::Superseded);
            break;
        }
        PerfTimer timer;
        ITfDocumentMgr *pDocMgrFocus = nullptr;
        ITfContext *pContext = nullptr;
        const UINT code = request.code;
        const WCHAR wch = request.wch;

        bool handedOffReplay = false;
        if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
        {
            if (SUCCEEDED(pDocMgrFocus->GetTop(&pContext)) && pContext)
            {
                const bool useDirectPunctuationSession =
                    pIME->_candidateMode == CANDIDATE_NONE && !pIME->_IsComposing();
                if (useDirectPunctuationSession)
                {
                    pIME->_RequestDirectPunctuationEditSession(pContext, code, wch, request.requestId,
                                                               std::move(request.prefetchedText), request.focusToken,
                                                               request.compositionEpoch, request.deferredReplayToken);
                    handedOffReplay = true;
                }
                else
                {
                    _KEYSTROKE_STATE KeystrokeState;
                    KeystrokeState.Category = CATEGORY_COMPOSING;
                    KeystrokeState.Function = FUNCTION_PUNCTUATION;
                    pIME->_InvokeKeyHandler(pContext, code, wch, 0, KeystrokeState, request.requestId,
                                            std::move(request.prefetchedText), 0, request.compositionEpoch,
                                            request.focusToken, request.deferredReplayToken);
                    handedOffReplay = true;
                }
                pContext->Release();
            }
            pDocMgrFocus->Release();
        }
        if (!handedOffReplay)
        {
            pIME->_FailDeferredKey(request.deferredReplayToken, DeferredKeyFailureReason::AsyncPostFailed);
        }
        break;
    }
    case WM_AsyncCycleInputMode: {
        CMetasequoiaIME::AsyncKeyRequest request;
        if (!pIME->_TakeAsyncKeyRequest(WM_AsyncCycleInputMode, static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            pIME->_FailDeferredKey(request.deferredReplayToken, DeferredKeyFailureReason::Superseded);
            break;
        }
        FanyImeNamedpipeDataToTsf *receivedData = TryReadCommitReplyFromServerPipe(request.requestId);
        // The edit handler parses the payload once and rejects malformed text
        // through the same deferred-cycle failure policy.
        if (receivedData->msg_type != Global::DataFromServerMsgType::TrilingualCycle)
        {
            const DeferredKeyFailureReason reason = IsDeliveredServerRequestId(request.requestId)
                                                        ? DeferredKeyFailureReason::DeliveryAmbiguous
                                                        : DeferredKeyFailureReason::TransportBroken;
            if (request.deferredReplayToken != 0)
            {
                pIME->_FailDeferredKey(request.deferredReplayToken, reason);
            }
            else
            {
                pIME->_ResetSessionAfterFailure(DeferredKeyFailureKind::Transport);
            }
            break;
        }

        ITfDocumentMgr *documentMgr = nullptr;
        ITfContext *context = nullptr;
        bool handedOffReplay = false;
        if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&documentMgr)) && documentMgr)
        {
            if (SUCCEEDED(documentMgr->GetTop(&context)) && context)
            {
                _KEYSTROKE_STATE keyState = {};
                keyState.Category = CATEGORY_COMPOSING;
                keyState.Function = FUNCTION_CYCLE_INPUT_MODE;
                pIME->_InvokeKeyHandler(context, request.code, request.wch, 0, keyState, request.requestId,
                                        receivedData->candidate_string, 0, request.compositionEpoch, request.focusToken,
                                        request.deferredReplayToken);
                handedOffReplay = true;
                context->Release();
            }
            documentMgr->Release();
        }
        if (!handedOffReplay)
        {
            pIME->_FailDeferredKey(request.deferredReplayToken, DeferredKeyFailureReason::AsyncPostFailed);
        }
        break;
    }
    case WM_AsyncServerCandidateKey: {
        CMetasequoiaIME::AsyncKeyRequest request;
        if (!pIME->_TakeAsyncKeyRequest(WM_AsyncServerCandidateKey, static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            pIME->_FailDeferredKey(request.deferredReplayToken, DeferredKeyFailureReason::Superseded);
            break;
        }
        const UINT code = request.code;
        const WCHAR wch = request.wch;
        // Paging and highlight moves only need an acknowledgement: wait the
        // ordinary budget and treat a missing reply as a soft miss, with no
        // teardown and no queue clear (a late reply is cached by request id).
        // Every other candidate key may commit the highlighted candidate, so
        // it gets the commit reply budget and is never resent once delivered.
        const bool navigationOnly = IsCandidateNavigationOnlyKey(code);
        FanyImeNamedpipeDataToTsf *receivedData =
            navigationOnly ? TryReadDataFromServerPipeWithTimeout(request.requestId, /*abortTransportOnTimeout=*/false)
                           : TryReadCommitReplyFromServerPipe(request.requestId);
        if (receivedData->msg_type == Global::DataFromServerMsgType::TransportUnavailable)
        {
            // A transport failure is not text and must never be committed to
            // the application. A delivered key may already have committed on
            // the Server: it is dropped with the composition, never resent.
            const DeferredKeyFailureReason reason = IsDeliveredServerRequestId(request.requestId)
                                                        ? DeferredKeyFailureReason::DeliveryAmbiguous
                                                        : DeferredKeyFailureReason::TransportBroken;
            if (request.deferredReplayToken != 0)
            {
                pIME->_FailDeferredKey(request.deferredReplayToken, reason);
            }
            else
            {
                pIME->_ResetSessionAfterFailure(DeferredKeyFailureKind::Transport);
            }
            break;
        }
        if (navigationOnly)
        {
            // The Server never commits for these keys, so the reply (or the
            // soft miss standing in for it) carries nothing to apply.
            pIME->_CompleteDeferredKeyReplay(request.deferredReplayToken);
            break;
        }

        if (receivedData->msg_type == Global::DataFromServerMsgType::CommitExactText)
        {
            pIME->_PostAsyncKeyRequest(WM_AsyncPunctuationCommit, code, wch, request.requestId,
                                       receivedData->candidate_string, request.focusToken, request.compositionEpoch,
                                       request.deferredReplayToken);
        }
        else if (receivedData->msg_type == Global::DataFromServerMsgType::Normal)
        {
            if (wch == 0)
            {
                pIME->_CompleteDeferredKeyReplay(request.deferredReplayToken);
                break;
            }
            std::wstring commitText = receivedData->candidate_string;
            if (code == VK_DECIMAL)
            {
                commitText.append(L".");
            }
            else if (pIME->_pCompositionProcessorEngine)
            {
                const WCHAR preceding = commitText.empty() ? 0 : commitText.back();
                commitText.append(pIME->_ResolveSmartPunctuation(wch, preceding));
            }
            pIME->_PostAsyncKeyRequest(WM_AsyncPunctuationCommit, code, wch, request.requestId, std::move(commitText),
                                       request.focusToken, request.compositionEpoch, request.deferredReplayToken);
        }
        else
        {
            pIME->_CompleteDeferredKeyReplay(request.deferredReplayToken);
        }
        // Navigation responses are acknowledgements only. The Server owns and
        // refreshes candidate paging/selection state, so TSF must not apply the
        // same movement to its presenter a second time.
        break;
    }
    case WM_AsyncNumberCandidateCommit: {
        CMetasequoiaIME::AsyncKeyRequest request;
        if (!pIME->_TakeAsyncKeyRequest(WM_AsyncNumberCandidateCommit, static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            pIME->_FailDeferredKey(request.deferredReplayToken, DeferredKeyFailureReason::Superseded);
            break;
        }
        PerfTimer timer;
        ITfDocumentMgr *pDocMgrFocus = nullptr;
        ITfContext *pContext = nullptr;

        bool handedOffReplay = false;
        if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
        {
            if (SUCCEEDED(pDocMgrFocus->GetTop(&pContext)) && pContext)
            {
                _KEYSTROKE_STATE KeystrokeState;
                KeystrokeState.Category = CATEGORY_CANDIDATE;
                KeystrokeState.Function = FUNCTION_SELECT_BY_NUMBER;
                pIME->_InvokeKeyHandler(pContext, request.code, request.wch, 0, KeystrokeState, request.requestId, {},
                                        0, request.compositionEpoch, request.focusToken, request.deferredReplayToken);
                handedOffReplay = true;
                pContext->Release();
            }
            pDocMgrFocus->Release();
        }
        if (!handedOffReplay)
        {
            pIME->_FailDeferredKey(request.deferredReplayToken, DeferredKeyFailureReason::AsyncPostFailed);
        }
        break;
    }
    case WM_CleanupCandidatePresenter: {
        pIME->_DrainPendingCandidatePresenterCleanup();
        break;
    }
    case WM_RefreshLanguageBarTheme: {
        if (wParam != 0)
        {
            pIME->_RefreshLanguageBarThemeIcons();
            break;
        }
        SetTimer(hWnd, TIMER_REFRESH_LANG_BAR_THEME, REFRESH_LANG_BAR_THEME_DELAY_MS, nullptr);
        break;
    }
    case WM_ApplyPunctuationLock: {
        CCompositionProcessorEngine *engine = pIME->GetCompositionProcessorEngine();
        if (engine && Global::IsPunctuationLocked())
        {
            engine->SetPunctuationMode(pIME->_GetThreadMgr(), pIME->_GetClientId(), TRUE);
        }
        SendCurrentImeStatusSnapshot(pIME);
        break;
    }
    case WM_PairedPunctuationCaretMove: {
        const uint64_t focusToken = static_cast<uint64_t>(static_cast<uint32_t>(wParam)) |
                                    (static_cast<uint64_t>(static_cast<uint32_t>(lParam)) << 32);
        if (focusToken == 0 || focusToken != pIME->_pendingPairedCaretFocusToken)
        {
            // A newer request already superseded this one, or the move was
            // cancelled between the post and the dispatch.
            break;
        }

        pIME->_RunPairedPunctuationCaretMove();
        break;
    }
    case WM_RewriteSmartPunctuationViaSendInput: {
        const uint64_t focusToken = static_cast<uint64_t>(static_cast<uint32_t>(wParam)) |
                                    (static_cast<uint64_t>(static_cast<uint32_t>(lParam)) << 32);
        if (focusToken == 0 || focusToken != pIME->_pendingSmartPunctuationRewriteFocusToken)
        {
            // A newer request already superseded this one, or the rewrite was
            // cancelled between the post and the dispatch.
            break;
        }

        pIME->_RunSmartPunctuationSendInputRewrite();
        break;
    }
    case WM_SETTINGCHANGE: {
        // Registry may not be flushed yet when the broadcast arrives.
        if (lParam && _wcsicmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0)
        {
            SetTimer(hWnd, TIMER_REFRESH_LANG_BAR_THEME, REFRESH_LANG_BAR_THEME_DELAY_MS, nullptr);
        }
        break;
    }
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}
