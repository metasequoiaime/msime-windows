#include "Private.h"
#include "Globals.h"
#include "MetasequoiaIME.h"
#include "CandidateListUIPresenter.h"
#include "CompositionProcessorEngine.h"
#include "KeyHandlerEditSession.h"
#include "KeyFocusRecovery.h"
#include "KeyRepeatGuard.h"
#include "stats_collector.h"
#include "stats_passthrough.h"
#include "CaretAnchorPolicy.h"
#include "Compartment.h"
#include "MetasequoiaIMEBaseStructure.h"
#include <debugapi.h>
#include <cwctype>
#include <string>
#include "Ipc.h"
#include "FanyUtils.h"
#include "FanyDefines.h"
#include "FanyLog.h"
#include "EditSession.h"
#include "TfTextLayoutSink.h"
#include "../Utils/PerfTimer.h"
#include <chrono>
#include "../../../engine/contracts/ipc_negotiation.h"
#include "KeyEventSinkInternal.h"

using namespace key_event_sink_detail;

namespace
{
class CKeyCaretAnchorEditSession : public CEditSessionBase
{
  public:
    CKeyCaretAnchorEditSession(CMetasequoiaIME *textService, ITfContext *context, int point[2], bool *resolved)
        : CEditSessionBase(textService, context), point_(point), resolved_(resolved)
    {
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        POINT anchor{};
        if (ResolveCollapsedSelectionAnchor(_pContext, ec, &anchor))
        {
            point_[0] = anchor.x;
            point_[1] = anchor.y;
            *resolved_ = true;
        }
        return S_OK;
    }

  private:
    int *point_;
    bool *resolved_;
};

bool ResolveKeyCaretAnchor(CMetasequoiaIME *textService, ITfContext *context, TfClientId clientId, int point[2])
{
    point[0] = 0;
    point[1] = Global::INVALID_Y;
    if (!context)
        return false;
    bool resolved = false;
    // TF_ES_SYNC either runs the session before returning or fails, so the
    // session never outlives the stack slots it writes to.
    auto *session = new (std::nothrow) CKeyCaretAnchorEditSession(textService, context, point, &resolved);
    if (!session)
        return false;
    HRESULT sessionResult = E_FAIL;
    const HRESULT requestResult =
        context->RequestEditSession(clientId, session, TF_ES_SYNC | TF_ES_READ, &sessionResult);
    session->Release();
    return SUCCEEDED(requestResult) && SUCCEEDED(sessionResult) && resolved;
}

} // namespace

CMetasequoiaIME::KeyDownDispatchResult CMetasequoiaIME::_DispatchKeyDown(
    _In_ ITfContext *pContext, WPARAM wParam, LPARAM lParam, _Out_ BOOL *pIsEaten, _In_opt_ const WCHAR *translatedWch,
    _In_opt_ const UINT *modifiersDown, _In_opt_ const _KEYSTROKE_STATE *prevalidatedKeyState, bool canDefer,
    uint64_t expectedFocusGeneration, uint64_t deferredReplayToken)
{
    if (pContext == nullptr || pIsEaten == nullptr)
    {
        return KeyDownDispatchResult::Complete;
    }

    if (translatedWch == nullptr)
    {
        Global::UpdateModifiers(wParam, lParam);
        _TrackModifierHotkeyArming(wParam, lParam, false);
        if (IsShiftVk(LOWORD(wParam)))
        {
            *pIsEaten = FALSE;
            return KeyDownDispatchResult::Complete;
        }
    }

    // Backspace hold guard (#347), ahead of the smart-punctuation probe and the
    // deferred branches: a repeat that follows a composition emptied by the
    // same hold is consumed locally — no shared memory, no IPC request, no edit
    // session. Replayed keys pass through here too, so the queued repeats are
    // swallowed as well.
    if (_ApplyBackspaceHoldGuard(wParam, lParam))
    {
        const WCHAR guardWch = ConvertVKey(VK_BACK);
        *pIsEaten = TRUE;
        _NoteKeyForSmartPunctuation(VK_BACK, guardWch, true, FUNCTION_BACKSPACE);
        DebugTsfIssue47(L"backspace-repeat-suppressed", FANY_IME_NO_REQUEST_ID, VK_BACK, guardWch, CATEGORY_COMPOSING,
                        FUNCTION_BACKSPACE, 1, _IsComposing(),
                        _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_OK,
                        deferredReplayToken);
        if (deferredReplayToken != 0)
        {
            _CompleteDeferredKeyReplay(deferredReplayToken);
        }
        return KeyDownDispatchResult::Complete;
    }

    _KEYSTROKE_STATE KeystrokeState = {};
    WCHAR wch = '\0';
    UINT code = 0;
    uint64_t requestId = FANY_IME_NO_REQUEST_ID;
    const UINT capturedModifiers = modifiersDown ? *modifiersDown : CaptureIpcModifiers();

    // Reversible smart punctuation is a local document edit: classify through
    // the same _IsKeyEaten the Test sink used, then run it directly instead of
    // queueing an IPC key or entering the deferred FIFO. This runs before the
    // barrier branches because the action does not depend on the Server.
    if (canDefer && translatedWch == nullptr && prevalidatedKeyState == nullptr)
    {
        _KEYSTROKE_STATE probeState = {};
        WCHAR probeWch = L'\0';
        UINT probeCode = 0;
        if (_IsKeyEaten(pContext, static_cast<UINT>(wParam), &probeCode, &probeWch, &probeState) &&
            (probeState.Function == FUNCTION_SMART_PUNCTUATION_CONVERT ||
             probeState.Function == FUNCTION_SMART_PUNCTUATION_REVERT))
        {
            *pIsEaten = TRUE;
            _RequestSmartPunctuationEditSession(pContext, probeWch, probeState.Function, expectedFocusGeneration);
            return KeyDownDispatchResult::Complete;
        }
    }

    if (canDefer && translatedWch == nullptr && prevalidatedKeyState == nullptr && !_HasDeferredKeyBarrier())
    {
        GUID hotkeyGuid = {};
        if (_MatchChordInputHotkey(wParam, &hotkeyGuid))
        {
            _shiftHotkeyArmed = false;
            _ctrlHotkeyArmed = false;
            if (expectedFocusGeneration == 0 || expectedFocusGeneration != _deferredKeyFocusGeneration)
            {
                *pIsEaten = TRUE;
                return KeyDownDispatchResult::Complete;
            }
            _QueueInputHotkey(pContext, hotkeyGuid, pIsEaten);
            return KeyDownDispatchResult::Complete;
        }
    }

    if (canDefer && _HasDeferredKeyBarrier())
    {
        if (!_DeferredKeyQueueHasCapacity() ||
            !_ClassifyDeferredKeyDown(pContext, wParam, lParam, translatedWch, &capturedModifiers, &wch, &code,
                                      &KeystrokeState))
        {
            // Mirror OnTestKeyDown: uneaten keys (esp. Backspace) must still
            // update smart-punctuation rejection state.
            if (code == 0 && wch == L'\0')
            {
                wch = translatedWch ? *translatedWch : ConvertVKey(static_cast<UINT>(wParam));
                code = VKeyFromVKPacketAndWchar(static_cast<UINT>(wParam), wch);
            }
            _NoteKeyForSmartPunctuation(code, wch, false, FUNCTION_NONE);
            *pIsEaten = FALSE;
            DebugTsfIssue47(L"keydown-deferred-rejected", FANY_IME_NO_REQUEST_ID, code, wch, KeystrokeState.Category,
                            KeystrokeState.Function, 0, _IsComposing(),
                            _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0,
                            HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
            return KeyDownDispatchResult::Complete;
        }
        if (expectedFocusGeneration == 0 || expectedFocusGeneration != _deferredKeyFocusGeneration)
        {
            *pIsEaten = TRUE;
            return KeyDownDispatchResult::Complete;
        }
        // Queued keys note on replay; note now too so a Backspace that is
        // somehow classified+queued still records rejection before drain.
        _NoteKeyForSmartPunctuation(code, wch, true, KeystrokeState.Function);
        *pIsEaten =
            _QueueDeferredKeyDown(pContext, wParam, lParam, wch, capturedModifiers, KeystrokeState) ? TRUE : FALSE;
        DebugTsfIssue47(*pIsEaten ? L"keydown-deferred-queued" : L"keydown-deferred-queue-failed",
                        FANY_IME_NO_REQUEST_ID, code, wch, KeystrokeState.Category, KeystrokeState.Function,
                        *pIsEaten ? 1 : 0, _IsComposing(),
                        _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0,
                        *pIsEaten ? S_OK : E_FAIL);
        if (*pIsEaten && _localSessionResetPending.load(std::memory_order_acquire))
        {
            const UINT resetToken = _localSessionResetToken.load(std::memory_order_acquire);
            _RequestLocalSessionReset(pContext, resetToken);
        }
        return KeyDownDispatchResult::Complete;
    }

    if (prevalidatedKeyState != nullptr)
    {
        KeystrokeState = *prevalidatedKeyState;
        wch = translatedWch ? *translatedWch : ConvertVKey(static_cast<UINT>(wParam));
        code = VKeyFromVKPacketAndWchar(static_cast<UINT>(wParam), wch);
        *pIsEaten = TRUE;
    }
    else
    {
        PerfTimer isKeyEatenTimer;
        *pIsEaten = _IsKeyEaten( //
            pContext,            //
            (UINT)wParam,        //
            &code,               //
            &wch,                //
            &KeystrokeState,     //
            translatedWch        //
        );
    }
    // Idempotent with the OnTestKeyDown call; replayed keys only pass here.
    _NoteKeyForSmartPunctuation(code, wch, *pIsEaten ? true : false, KeystrokeState.Function);

    DebugTsfIssue47(L"keydown-classified", FANY_IME_NO_REQUEST_ID, code, wch, KeystrokeState.Category,
                    KeystrokeState.Function, *pIsEaten ? 1 : 0, _IsComposing(),
                    _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_OK,
                    deferredReplayToken);

    // The probe above only covers the immediate path. Nothing further down
    // handles these two functions, so a key that reaches here still classified
    // as a smart-punctuation action (a replay, or a prevalidated key state)
    // would be eaten and silently dropped. Run the same local edit session.
    if (*pIsEaten && (KeystrokeState.Function == FUNCTION_SMART_PUNCTUATION_CONVERT ||
                      KeystrokeState.Function == FUNCTION_SMART_PUNCTUATION_REVERT))
    {
        _RequestSmartPunctuationEditSession(pContext, wch, KeystrokeState.Function, expectedFocusGeneration);
        if (deferredReplayToken != 0)
        {
            _CompleteDeferredKeyReplay(deferredReplayToken);
        }
        return KeyDownDispatchResult::Complete;
    }

    if (expectedFocusGeneration == 0 || expectedFocusGeneration != _deferredKeyFocusGeneration)
    {
        // A COM callback inside key classification changed the focused
        // topology. The old key must not enter the replacement Server epoch.
        *pIsEaten = TRUE;
        DebugTsfIssue47(L"keydown-focus-generation-changed", FANY_IME_NO_REQUEST_ID, code, wch, KeystrokeState.Category,
                        KeystrokeState.Function, 1, _IsComposing(),
                        _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_FALSE,
                        deferredReplayToken);
        return KeyDownDispatchResult::Complete;
    }

    const bool resetPending = _localSessionResetPending.load(std::memory_order_acquire);
    if (resetPending)
    {
        if (!canDefer)
        {
            DebugTsfIssue47(L"keydown-reset-superseded", FANY_IME_NO_REQUEST_ID, code, wch, KeystrokeState.Category,
                            KeystrokeState.Function, 1, _IsComposing(),
                            _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0,
                            S_FALSE, deferredReplayToken);
            return KeyDownDispatchResult::Superseded;
        }

        // The reset gate may have closed concurrently with normal
        // classification. Reclassify against the FIFO's future state before
        // retaining the key.
        if (!_DeferredKeyQueueHasCapacity() ||
            !_ClassifyDeferredKeyDown(pContext, wParam, lParam, translatedWch, &capturedModifiers, &wch, &code,
                                      &KeystrokeState) ||
            !_QueueDeferredKeyDown(pContext, wParam, lParam, wch, capturedModifiers, KeystrokeState))
        {
            *pIsEaten = FALSE;
        }
        const UINT resetToken = _localSessionResetToken.load(std::memory_order_acquire);
        DebugTsfIssue47(*pIsEaten ? L"keydown-reset-queued" : L"keydown-reset-queue-failed", FANY_IME_NO_REQUEST_ID,
                        code, wch, KeystrokeState.Category, KeystrokeState.Function, *pIsEaten ? 1 : 0, _IsComposing(),
                        _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0,
                        *pIsEaten ? S_OK : E_FAIL, resetToken);
        _RequestLocalSessionReset(pContext, resetToken);
        return KeyDownDispatchResult::Complete;
    }

    if (canDefer && *pIsEaten && (KeystrokeState.Category != CATEGORY_NONE || KeystrokeState.Function != FUNCTION_NONE))
    {
        // Give every IME-owned key a member-owned dispatch token before any
        // IPC write or asynchronous TSF edit session is started.  Consequently
        // a write success followed by a reply/edit failure takes the same
        // _FailDeferredKey path as a key that arrived behind a barrier.
        const bool healthyImmediateDispatch = _deferredKeyDowns.empty() && !_hasDeferredKeyInFlight;
        const bool queued = _QueueDeferredKeyDown(pContext, wParam, lParam, wch, capturedModifiers, KeystrokeState,
                                                  /*scheduleDrain=*/!healthyImmediateDispatch) != FALSE;
        *pIsEaten = queued ? TRUE : FALSE;
        DebugTsfIssue47(queued ? L"keydown-owned-queued" : L"keydown-owned-queue-failed", FANY_IME_NO_REQUEST_ID, code,
                        wch, KeystrokeState.Category, KeystrokeState.Function, queued ? 1 : 0, _IsComposing(),
                        _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0,
                        queued ? S_OK : E_FAIL, deferredReplayToken);
        if (queued && healthyImmediateDispatch)
        {
            // Healthy fast path: drain the FIFO synchronously inside
            // OnKeyDown instead of waiting for the posted
            // WM_DrainDeferredKeyDown.  In Excel, the first keydown is what
            // puts the selected cell into edit mode; the app pushes a new TSF
            // context during that same message, and OnPushContext clears the
            // deferred queue before the posted drain could run, swallowing
            // the first key.  Dispatching immediately restores the
            // reference-sample timing while retaining the dispatch token for
            // IPC/edit-session failures.  If the transport or focus session is
            // not ready, _DrainOneDeferredKeyDown leaves the key queued for the
            // ordinary asynchronous drain.
            _DrainOneDeferredKeyDown();
            // The drain is posted only when the synchronous one left the key
            // queued (no connected focus session, or a reset opened). A key it
            // dispatched is retired by its own completion, which schedules the
            // next drain, so a WM_DrainDeferredKeyDown posted up front would
            // only ever find the key in flight or gone.
            _ScheduleDeferredKeyDownDrain();
        }
        return KeyDownDispatchResult::Complete;
    }

    const bool isPunctuationKey = _pCompositionProcessorEngine && _pCompositionProcessorEngine->IsPunctuation(wch);
    const bool isNoOpForwardDelete =
        KeystrokeState.Function == FUNCTION_DELETE && _pCompositionProcessorEngine &&
        _pCompositionProcessorEngine->GetCaretPosition() >= _pCompositionProcessorEngine->GetVirtualKeyLength();

    Global::firefox_like_cnt = 0;

    /* Send key event to server process */
    if (*pIsEaten && !isNoOpForwardDelete)
    {
        // 检查是否应该跳过发送此键到服务器，由于长度限制。
        // 当达到限制时，我们只阻止字符输入键（FUNCTION_INPUT）。
        // 这允许功能键如 Backspace、Space、Enter 仍然工作。
        if (KeystrokeState.Function == FUNCTION_INPUT &&
            _pCompositionProcessorEngine->GetVirtualKeyLength() >= MAX_PINYIN_LENGTH)
        {
            // 这个键仍然被吃掉（以防止它到达应用程序），
            // 但我们不把它发送到 server 端，也不进一步处理它。
            DebugTsfIssue47(L"keydown-input-length-limit", FANY_IME_NO_REQUEST_ID, code, wch, KeystrokeState.Category,
                            KeystrokeState.Function, 1, _IsComposing(),
                            _pCompositionProcessorEngine->GetVirtualKeyLength(),
                            HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW), deferredReplayToken);
            return KeyDownDispatchResult::Complete;
        }

        if (expectedFocusGeneration != _deferredKeyFocusGeneration)
        {
            return KeyDownDispatchResult::Complete;
        }

        if (KeystrokeState.Function == FUNCTION_TOGGLE_CHARACTER_SET && !SupportsCharacterSetShortcut())
        {
            // The transport may have been replaced by an older Server after
            // TestKeyDown owned this key. Do not send it as ordinary input.
            return KeyDownDispatchResult::Complete;
        }

        Global::Keycode = code;
        Global::wch = wch;
        Global::ModifiersDown = capturedModifiers;

        // The character-set shortcut carries the caret anchor for its badge.
        // An unresolved anchor is sent explicitly as {0, INVALID_Y}; the packet
        // default point would otherwise read as a real screen position.
        int keyPoint[2] = {0, Global::INVALID_Y};
        const bool includeCaretAnchor =
            KeystrokeState.Function == FUNCTION_TOGGLE_CHARACTER_SET && SupportsCaretStateIndicator();
        if (includeCaretAnchor)
        {
            (void)ResolveKeyCaretAnchor(this, pContext, _tfClientId, keyPoint);
        }

        PerfTimer writeShmTimer;
        WriteDataToSharedMemory(Global::Keycode, wch, Global::ModifiersDown, includeCaretAnchor ? keyPoint : nullptr, 0,
                                L"", KeyEventPayloadWriteMask(includeCaretAnchor));

        PerfTimer sendKeyEventTimer;
        const KeyEventSendResult sendResult = SendKeyEventToUIProcess(&requestId);
        if (KeystrokeState.Function == FUNCTION_TOGGLE_CHARACTER_SET)
        {
            // No document edit or response is needed. In particular, an ambiguous
            // delivery must never replay a toggle after reconnecting.
            return KeyDownDispatchResult::Complete;
        }
        DebugTsfKeyLatency(L"main-pipe-send", requestId, sendKeyEventTimer.ElapsedMs(),
                           sendResult == KeyEventSendResult::Sent ? S_OK : E_FAIL);
        DebugTsfIssue47(sendResult == KeyEventSendResult::Sent ? L"keydown-sent" : L"keydown-send-failed", requestId,
                        code, wch, KeystrokeState.Category, KeystrokeState.Function, *pIsEaten ? 1 : 0, _IsComposing(),
                        _pCompositionProcessorEngine->GetVirtualKeyLength(),
                        sendResult == KeyEventSendResult::Sent ? S_OK : E_FAIL, deferredReplayToken);
        if (sendResult != KeyEventSendResult::Sent)
        {
            // DefinitelyNotSent or DeliveryAmbiguous: the key stays eaten and is
            // dropped. An ambiguous frame may already be on the Server, so it
            // is never written a second time; the transport reset clears both
            // sides and the user retypes.
            if (!canDefer)
            {
                return KeyDownDispatchResult::TransportFailed;
            }
            _ResetSessionAfterFailure(DeferredKeyFailureKind::Transport);
            return KeyDownDispatchResult::Complete;
        }

        if (KeystrokeState.Function == FUNCTION_SERVER_CANDIDATE_KEY && _msgWndHandle)
        {
            _PostAsyncKeyRequest(WM_AsyncServerCandidateKey, code, wch, requestId, {}, 0, 0, deferredReplayToken);
            return deferredReplayToken != 0 ? KeyDownDispatchResult::AwaitingCompletion
                                            : KeyDownDispatchResult::Complete;
        }

        if (code == VK_SPACE && KeystrokeState.Function == FUNCTION_CONVERT)
        {
            if (_msgWndHandle)
            {
                _PostAsyncKeyRequest(WM_AsyncFinalizeCandidate, code, wch, requestId, {}, 0, 0, deferredReplayToken);
                return deferredReplayToken != 0 ? KeyDownDispatchResult::AwaitingCompletion
                                                : KeyDownDispatchResult::Complete;
            }
        }

        if (KeystrokeState.Function == FUNCTION_PUNCTUATION && _msgWndHandle)
        {
            PerfTimer asyncPuncTimer;
            std::wstring punctuationCommitText;
            const bool shouldFinalizeHighlightedCandidateWithPunctuation =
                _candidateMode != CANDIDATE_NONE && _pCandidateListUIPresenter &&
                Global::CommitWithHighlightedCandPunc.count(wch) > 0;
            if (shouldFinalizeHighlightedCandidateWithPunctuation)
            {
                // Empty means the edit session must consume this request's
                // candidate reply and append the punctuation derived from wch
                // (including smart-punctuation against the candidate text).
                punctuationCommitText.clear();
            }
            else if (code == VK_DECIMAL)
            {
                // Numpad '.' stays ASCII, including after a candidate.
                punctuationCommitText = L".";
            }
            else if (CCompositionProcessorEngine::IsSmartAsciiPunctuationKey(wch) &&
                     Global::SmartPunctuationEnabled.load(std::memory_order_relaxed) &&
                     (Global::SmartPunctuationDirectDigitEnabled.load(std::memory_order_relaxed) ||
                      Global::SmartPunctuationDirectLetterEnabled.load(std::memory_order_relaxed)))
            {
                // Defer mapping until the edit session can inspect the
                // preceding document character (digits and/or letters →
                // ASCII). With both direct sub-switches off, ResolvePunctuation
                // would return the Chinese punctuation anyway, so the immediate
                // mapping below is equivalent and skips a pointless session.
                punctuationCommitText.clear();
            }
            else
            {
                const WCHAR *punctuation = _pCompositionProcessorEngine->GetPunctuation(wch);
                punctuationCommitText = punctuation ? punctuation : L"";
            }
            _PostAsyncKeyRequest(WM_AsyncPunctuationCommit, code, wch, requestId, std::move(punctuationCommitText), 0,
                                 0, deferredReplayToken);
            return deferredReplayToken != 0 ? KeyDownDispatchResult::AwaitingCompletion
                                            : KeyDownDispatchResult::Complete;
        }

        if (KeystrokeState.Function == FUNCTION_SELECT_BY_NUMBER && _msgWndHandle)
        {
            _PostAsyncKeyRequest(WM_AsyncNumberCandidateCommit, code, wch, requestId, {}, 0, 0, deferredReplayToken);
            return deferredReplayToken != 0 ? KeyDownDispatchResult::AwaitingCompletion
                                            : KeyDownDispatchResult::Complete;
        }
    }

    if (*pIsEaten)
    {
        bool needInvokeKeyHandler = true;
        /* Invoke key handler edit session */
        if (code == VK_ESCAPE)
        {
            KeystrokeState.Category = CATEGORY_COMPOSING;
        }

        /* Always eat THIRDPARTY_NEXTPAGE and THIRDPARTY_PREVPAGE
        keys, but don't always process them. */
        if ((wch == THIRDPARTY_NEXTPAGE) || (wch == THIRDPARTY_PREVPAGE))
        {
            needInvokeKeyHandler = !((KeystrokeState.Category == CATEGORY_NONE) && //
                                     (KeystrokeState.Function == FUNCTION_NONE));
        }
        if (needInvokeKeyHandler)
        {
            PerfTimer invokeTimer;
            _InvokeKeyHandler(pContext, code, wch, (DWORD)lParam, KeystrokeState, requestId, {}, 0, 0, 0,
                              deferredReplayToken);
            if (deferredReplayToken != 0)
            {
                return KeyDownDispatchResult::AwaitingCompletion;
            }
        }
    }
    else if (KeystrokeState.Category == CATEGORY_INVOKE_COMPOSITION_EDIT_SESSION)
    {
        // Invoke key handler edit session
        KeystrokeState.Category = CATEGORY_COMPOSING;
        PerfTimer invokeTimer;
        _InvokeKeyHandler(pContext, code, wch, (DWORD)lParam, KeystrokeState, FANY_IME_NO_REQUEST_ID);
    }

    if (isPunctuationKey)
    {
    }
    return KeyDownDispatchResult::Complete;
}
