// Edit sessions requested from the key path (direct punctuation, smart punctuation, deferred
// application text), the pending Server candidate slot, and the async key request table.

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
// Messages whose handler waits for a committing reply and re-queues its request
// when the reply is not in yet (see _PollCommitReply). Only a request whose
// wait has started (commitReplyDeadlineTick != 0) is re-dispatched by the
// timer; one still in the posted queue runs from its own message.
bool IsCommitReplyPollMessage(UINT message)
{
    return message == WM_AsyncServerCandidateKey || message == WM_AsyncCycleInputMode ||
           message == WM_AsyncFinalizeCandidate || message == WM_AsyncNumberCandidateCommit ||
           message == WM_AsyncPunctuationCommit;
}

class CPunctuationCommitEditSession : public CEditSessionBase
{
  public:
    CPunctuationCommitEditSession(CMetasequoiaIME *pTextService, ITfContext *pContext, UINT code, WCHAR wch,
                                  uint64_t requestId, LARGE_INTEGER requestStartQpc, std::wstring prefetchedText,
                                  uint64_t focusToken, uint64_t compositionEpoch, uint64_t deferredReplayToken)
        : CEditSessionBase(pTextService, pContext), _code(code), _wch(wch), _requestId(requestId),
          _requestStartQpc(requestStartQpc), _prefetchedText(std::move(prefetchedText)), _focusToken(focusToken),
          _compositionEpoch(compositionEpoch), _deferredReplayToken(deferredReplayToken)
    {
        if (_requestStartQpc.QuadPart == 0)
        {
            QueryPerformanceCounter(&_requestStartQpc);
        }
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        struct Completion
        {
            CMetasequoiaIME *textService;
            uint64_t token;
            bool applied = false;
            bool superseded = true;
            HRESULT result = S_OK;
            ~Completion()
            {
                if (textService && token != 0)
                {
                    if (applied)
                    {
                        textService->_CompleteDeferredKeyReplay(token);
                    }
                    else
                    {
                        textService->_FailDeferredKey(
                            token, ClassifyEditSessionFailure(superseded, result == FANY_E_COMMIT_REPLY_AMBIGUOUS,
                                                              result == HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE)));
                    }
                }
            }
        } completion{_pTextService, _deferredReplayToken, false};

        if (!_pTextService->_IsFocusSessionCurrent(_focusToken, _pContext) ||
            !_pTextService->_IsCompositionEpochCurrent(_compositionEpoch))
        {
            return S_FALSE;
        }
        completion.superseded = false;
        HRESULT hr =
            _pTextService->_HandleCompositionPunctuation(ec, _pContext, _code, _wch, _requestId, _prefetchedText);
        completion.applied = hr == S_OK;
        completion.result = hr;
        return hr;
    }

  private:
    UINT _code;
    WCHAR _wch;
    uint64_t _requestId;
    LARGE_INTEGER _requestStartQpc;
    std::wstring _prefetchedText;
    uint64_t _focusToken;
    uint64_t _compositionEpoch;
    uint64_t _deferredReplayToken;
};

class CSmartPunctuationEditSession : public CEditSessionBase
{
  public:
    CSmartPunctuationEditSession(CMetasequoiaIME *pTextService, ITfContext *pContext, WCHAR wch,
                                 KEYSTROKE_FUNCTION function, uint64_t focusToken)
        : CEditSessionBase(pTextService, pContext), _wch(wch), _function(function), _focusToken(focusToken)
    {
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        // The document/state may already belong to another focus session by
        // the time the request is serviced; the key was claimed synchronously,
        // so there is nothing to hand back to the host here.
        if (!_pTextService->_IsFocusSessionCurrent(_focusToken, _pContext))
        {
            return S_FALSE;
        }
        return _pTextService->_ExecuteSmartPunctuationAction(ec, _pContext, _wch, _function);
    }

  private:
    WCHAR _wch;
    KEYSTROKE_FUNCTION _function;
    uint64_t _focusToken;
};

// Secondary session for the case where CSmartPunctuationEditSession could not
// be serviced at all. It only writes the key's own character (space or Chinese
// punctuation) so a synchronously claimed key is not silently lost, and it
// validates the focus session itself instead of reusing the primary request
// path (which would recurse into the same failure).
class CSmartPunctuationFallbackEditSession : public CEditSessionBase
{
  public:
    CSmartPunctuationFallbackEditSession(CMetasequoiaIME *pTextService, ITfContext *pContext, WCHAR wch,
                                         KEYSTROKE_FUNCTION function, uint64_t focusToken)
        : CEditSessionBase(pTextService, pContext), _wch(wch), _function(function), _focusToken(focusToken)
    {
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        if (!_pTextService->_IsFocusSessionCurrent(_focusToken, _pContext))
        {
            return S_FALSE;
        }
        return _pTextService->_ExecuteSmartPunctuationFallback(ec, _pContext, _wch, _function);
    }

  private:
    WCHAR _wch;
    KEYSTROKE_FUNCTION _function;
    uint64_t _focusToken;
};

class CDeferredApplicationTextEditSession : public CEditSessionBase
{
  public:
    CDeferredApplicationTextEditSession(CMetasequoiaIME *pTextService, ITfContext *pContext, WCHAR wch,
                                        uint64_t focusToken, uint64_t focusGeneration, uint64_t replayToken)
        : CEditSessionBase(pTextService, pContext), _wch(wch), _focusToken(focusToken),
          _focusGeneration(focusGeneration), _replayToken(replayToken)
    {
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        const bool replayCurrent =
            _pTextService->_IsDeferredKeyReplayCurrent(_replayToken, _focusGeneration, _pContext);
        const bool fallbackActive = _pTextService->_IsServerUnavailableFallbackActive();
        const bool focusCurrent = _pTextService->_IsFocusSessionCurrent(_focusToken, _pContext);
        if (!replayCurrent || (!fallbackActive && !focusCurrent))
        {
            _pTextService->_FailDeferredKey(_replayToken, DeferredKeyFailureReason::Superseded);
            return S_FALSE;
        }

        TF_SELECTION selection = {};
        ULONG fetched = 0;
        bool textApplied = false;
        HRESULT hr = _pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
        if (SUCCEEDED(hr) && fetched == 1 && selection.range)
        {
            hr = selection.range->SetText(ec, 0, &_wch, 1);
            if (SUCCEEDED(hr))
            {
                // SetText is the irreversible operation.  A subsequent
                // selection-placement failure must not retry the character,
                // otherwise the application receives it twice.
                textApplied = true;
                selection.range->Collapse(ec, TF_ANCHOR_END);
                hr = _pContext->SetSelection(ec, 1, &selection);
            }
            selection.range->Release();
        }
        else if (SUCCEEDED(hr))
        {
            hr = E_FAIL;
        }

        if (textApplied)
        {
            _pTextService->_CompleteDeferredKeyReplay(_replayToken);
        }
        else
        {
            _pTextService->_FailDeferredKey(_replayToken, DeferredKeyFailureReason::HostEditRejected);
        }
        return hr;
    }

  private:
    WCHAR _wch;
    uint64_t _focusToken;
    uint64_t _focusGeneration;
    uint64_t _replayToken;
};

} // namespace

HRESULT CMetasequoiaIME::_RequestDeferredApplicationTextEditSession(_In_ ITfContext *pContext, WCHAR wch,
                                                                    uint64_t expectedFocusToken,
                                                                    uint64_t expectedFocusGeneration,
                                                                    uint64_t deferredReplayToken)
{
    if (pContext == nullptr || wch == L'\0' || deferredReplayToken == 0)
    {
        _CompleteDeferredKeyReplay(deferredReplayToken);
        return E_INVALIDARG;
    }

    CDeferredApplicationTextEditSession *editSession = new (std::nothrow) CDeferredApplicationTextEditSession(
        this, pContext, wch, expectedFocusToken, expectedFocusGeneration, deferredReplayToken);
    if (editSession == nullptr)
    {
        _FailDeferredKey(deferredReplayToken, DeferredKeyFailureReason::EditSessionRequestFailed);
        return E_OUTOFMEMORY;
    }

    HRESULT editSessionHr = E_FAIL;
    const HRESULT requestHr =
        pContext->RequestEditSession(_tfClientId, editSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &editSessionHr);
    editSession->Release();
    if (FAILED(requestHr) || FAILED(editSessionHr))
    {
        _FailDeferredKey(deferredReplayToken, DeferredKeyFailureReason::EditSessionRequestFailed);
    }
    return FAILED(requestHr) ? requestHr : editSessionHr;
}

HRESULT CMetasequoiaIME::_RequestDirectPunctuationEditSession(_In_ ITfContext *pContext, UINT code, WCHAR wch,
                                                              uint64_t requestId, std::wstring prefetchedText,
                                                              uint64_t expectedFocusToken,
                                                              uint64_t expectedCompositionEpoch,
                                                              uint64_t deferredReplayToken)
{
    if (pContext == nullptr)
    {
        return E_INVALIDARG;
    }

    LARGE_INTEGER requestStartQpc = {};
    QueryPerformanceCounter(&requestStartQpc);

    CPunctuationCommitEditSession *pEditSession = new (std::nothrow) CPunctuationCommitEditSession(
        this, pContext, code, wch, requestId, requestStartQpc, std::move(prefetchedText),
        expectedFocusToken != 0 ? expectedFocusToken : _CaptureFocusSessionToken(),
        expectedCompositionEpoch != 0 ? expectedCompositionEpoch : _CaptureCompositionEpoch(), deferredReplayToken);
    if (pEditSession == nullptr)
    {
        _FailDeferredKey(deferredReplayToken, DeferredKeyFailureReason::EditSessionRequestFailed);
        return E_OUTOFMEMORY;
    }

    HRESULT editSessionHr = E_FAIL;
    PerfTimer requestTimer;
    HRESULT requestHr =
        pContext->RequestEditSession(_tfClientId, pEditSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &editSessionHr);
    pEditSession->Release();
    if (FAILED(requestHr) || FAILED(editSessionHr))
    {
        _FailDeferredKey(deferredReplayToken, DeferredKeyFailureReason::EditSessionRequestFailed);
    }
    return FAILED(requestHr) ? requestHr : editSessionHr;
}

HRESULT CMetasequoiaIME::_RequestSmartPunctuationEditSession(_In_ ITfContext *pContext, WCHAR wch,
                                                             KEYSTROKE_FUNCTION function,
                                                             uint64_t expectedFocusGeneration)
{
    if (pContext == nullptr)
    {
        return E_INVALIDARG;
    }
    // The key belongs to a focus topology that is already gone. It was claimed
    // by the Test sink, so drop it rather than edit the new topology.
    if (expectedFocusGeneration == 0 || expectedFocusGeneration != _deferredKeyFocusGeneration)
    {
        return S_FALSE;
    }

    CSmartPunctuationEditSession *pEditSession =
        new (std::nothrow) CSmartPunctuationEditSession(this, pContext, wch, function, _CaptureFocusSessionToken());
    if (pEditSession == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    HRESULT editSessionHr = E_FAIL;
    const HRESULT requestHr =
        pContext->RequestEditSession(_tfClientId, pEditSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &editSessionHr);
    pEditSession->Release();
    const HRESULT hr = FAILED(requestHr) ? requestHr : editSessionHr;
    if (FAILED(hr))
    {
        // The key was claimed synchronously and can no longer be handed back to
        // the host, so run a minimal second session that writes the key's own
        // character. If even that fails the key is visibly lost — a host that
        // refuses every write leaves no other option.
        HRESULT fallbackHr = E_OUTOFMEMORY;
        CSmartPunctuationFallbackEditSession *pFallbackSession = new (std::nothrow)
            CSmartPunctuationFallbackEditSession(this, pContext, wch, function, _CaptureFocusSessionToken());
        if (pFallbackSession != nullptr)
        {
            HRESULT fallbackEditSessionHr = E_FAIL;
            const HRESULT fallbackRequestHr = pContext->RequestEditSession(
                _tfClientId, pFallbackSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &fallbackEditSessionHr);
            pFallbackSession->Release();
            fallbackHr = FAILED(fallbackRequestHr) ? fallbackRequestHr : fallbackEditSessionHr;
        }
        if (FAILED(fallbackHr))
        {
            DebugTsfIssue47(L"smart-punctuation-fallback", FANY_IME_NO_REQUEST_ID, 0, wch, 0,
                            static_cast<UINT>(function), 1, _IsComposing(),
                            _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0,
                            fallbackHr);
        }
    }
    return hr;
}

void CMetasequoiaIME::_QueuePendingServerCandidate(UINT msgType, _In_z_ const WCHAR *pCandidateString)
{
    std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
    _hasPendingServerCandidate = true;
    _pendingServerCandidateMsgType = msgType;
    _pendingServerCandidateString = pCandidateString ? pCandidateString : L"";
}

bool CMetasequoiaIME::_TakePendingServerCandidate(_Out_ UINT *pMsgType, _Out_ std::wstring *pCandidateString)
{
    if (pMsgType == nullptr || pCandidateString == nullptr)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
    if (!_hasPendingServerCandidate)
    {
        return false;
    }

    *pMsgType = _pendingServerCandidateMsgType;
    pCandidateString->swap(_pendingServerCandidateString);
    _hasPendingServerCandidate = false;
    _pendingServerCandidateMsgType = Global::DataFromServerMsgType::OutofRange;
    return true;
}

bool CMetasequoiaIME::_PostAsyncKeyRequest(UINT message, UINT code, WCHAR wch, uint64_t requestId,
                                           std::wstring prefetchedText, uint64_t expectedFocusToken,
                                           uint64_t expectedCompositionEpoch, uint64_t deferredReplayToken)
{
    switch (message)
    {
    case WM_AsyncServerCandidateKey:
    case WM_AsyncCycleInputMode:
    case WM_AsyncFinalizeCandidate:
    case WM_AsyncPunctuationCommit:
    case WM_AsyncNumberCandidateCommit:
        break;
    default:
        _FailDeferredKey(deferredReplayToken, DeferredKeyFailureReason::AsyncPostFailed);
        return false;
    }

    const DeferredKeyFailureKind resetKind =
        message == WM_AsyncCycleInputMode ? DeferredKeyFailureKind::Transport : DeferredKeyFailureKind::Resync;
    constexpr size_t maxPendingAsyncKeys = 64;
    const uint64_t focusToken = expectedFocusToken != 0 ? expectedFocusToken : _CaptureFocusSessionToken();
    const uint64_t compositionEpoch =
        expectedCompositionEpoch != 0 ? expectedCompositionEpoch : _CaptureCompositionEpoch();
    const HWND ownerWindow = _msgWndHandle;
    if (focusToken == 0 || !ownerWindow || !IsWindow(ownerWindow))
    {
        if (deferredReplayToken != 0)
        {
            _FailDeferredKey(deferredReplayToken, DeferredKeyFailureReason::AsyncPostFailed);
        }
        else
        {
            _ResetSessionAfterFailure(resetKind);
        }
        return false;
    }

    UINT token = 0;
    {
        std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
        if (_pendingAsyncKeyMessages.size() < maxPendingAsyncKeys)
        {
            do
            {
                token = NextWindowMessageToken();
            } while (token == 0 || _pendingAsyncKeyMessages.count(token) != 0);
            _pendingAsyncKeyMessages.emplace(token, AsyncKeyRequest{message, code, wch, requestId, focusToken,
                                                                    compositionEpoch, deferredReplayToken,
                                                                    std::move(prefetchedText)});
        }
    }
    if (token == 0)
    {
        if (deferredReplayToken != 0)
        {
            _FailDeferredKey(deferredReplayToken, DeferredKeyFailureReason::AsyncPostFailed);
        }
        else
        {
            _ResetSessionAfterFailure(resetKind);
        }
        return false;
    }
    if (!PostMessage(ownerWindow, message, static_cast<WPARAM>(token), 0))
    {
        {
            std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
            _pendingAsyncKeyMessages.erase(token);
        }
        if (deferredReplayToken != 0)
        {
            _FailDeferredKey(deferredReplayToken, DeferredKeyFailureReason::AsyncPostFailed);
        }
        else
        {
            _ResetSessionAfterFailure(resetKind);
        }
        return false;
    }
    return true;
}

bool CMetasequoiaIME::_TakeAsyncKeyRequest(UINT message, UINT token, _Out_ AsyncKeyRequest &request)
{
    std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
    const auto entry = _pendingAsyncKeyMessages.find(token);
    if (entry == _pendingAsyncKeyMessages.end() || entry->second.message != message)
    {
        return false;
    }
    request = std::move(entry->second);
    _pendingAsyncKeyMessages.erase(entry);
    return true;
}

bool CMetasequoiaIME::_RequeueAsyncKeyRequest(UINT token, UINT message, AsyncKeyRequest request)
{
    const HWND ownerWindow = _msgWndHandle;
    if (token == 0 || !ownerWindow || !IsWindow(ownerWindow))
    {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
        _pendingAsyncKeyMessages[token] = std::move(request);
    }
    // Wake on a timer instead of re-posting the message: a posted message keeps
    // the thread queue non-empty for the whole wait, which defers every WM_PAINT
    // and WM_TIMER the host is waiting on. A timer yields the same slice and
    // still lets the host repaint (see TIMER_COMMIT_REPLY_POLL).
    if (SetTimer(ownerWindow, TIMER_COMMIT_REPLY_POLL, COMMIT_REPLY_POLL_INTERVAL_MS, nullptr) != 0)
    {
        return true;
    }
    // No timer slot left: the posted message is the pre-timer behaviour and
    // only costs the host its repaints, so it stays as the fallback.
    if (PostMessage(ownerWindow, message, static_cast<WPARAM>(token), 0))
    {
        return true;
    }
    std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
    _pendingAsyncKeyMessages.erase(token);
    return false;
}

void CMetasequoiaIME::_DispatchPendingCommitReplyPolls()
{
    const HWND ownerWindow = _msgWndHandle;
    if (!ownerWindow || !IsWindow(ownerWindow))
    {
        return;
    }

    struct Poll
    {
        uint64_t requestId;
        UINT token;
        UINT message;
    };
    std::vector<Poll> polls;
    {
        std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
        for (const auto &entry : _pendingAsyncKeyMessages)
        {
            // A request still in the posted queue has not started waiting and
            // runs from its own message; dispatching it here would run it ahead
            // of the messages posted before it.
            if (IsCommitReplyPollMessage(entry.second.message) && entry.second.commitReplyDeadlineTick != 0)
            {
                polls.push_back({entry.second.requestId, entry.first, entry.second.message});
            }
        }
    }
    // Commits settle in the order they were sent, not in token order.
    std::sort(polls.begin(), polls.end(), [](const Poll &a, const Poll &b) { return a.requestId < b.requestId; });

    // Synchronous dispatch: the timer pass already yielded to the host, and
    // returning with an empty queue is what lets the system generate the next
    // WM_PAINT. Every handler either settles its request or re-arms the timer.
    // Nothing is held while dispatching: the handlers take the same mutex.
    bool stillPolling = false;
    for (const Poll &poll : polls)
    {
        SendMessage(ownerWindow, poll.message, static_cast<WPARAM>(poll.token), 0);
        std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
        const auto entry = _pendingAsyncKeyMessages.find(poll.token);
        if (entry != _pendingAsyncKeyMessages.end() && entry->second.commitReplyDeadlineTick != 0)
        {
            // Still waiting: the newer commits stay behind it.
            stillPolling = true;
            break;
        }
    }
    if (!stillPolling)
    {
        // Self-limiting: with nothing left to poll the timer would otherwise
        // wake this thread once a tick for the life of the message window.
        KillTimer(ownerWindow, TIMER_COMMIT_REPLY_POLL);
    }
}

bool CMetasequoiaIME::_PollCommitReply(UINT message, UINT token, _Inout_ AsyncKeyRequest &request,
                                       _Outptr_result_maybenull_ FanyImeNamedpipeDataToTsf **receivedData)
{
    *receivedData = nullptr;
    if (request.commitReplyDeadlineTick == 0)
    {
        request.commitReplyDeadlineTick = GetTickCount64() + FANY_IME_COMMIT_REPLY_TIMEOUT_MS;
    }

    // An older commit that is still waiting settles first. The Server answers in
    // send order, so its reply is ahead of this one and this one cannot be
    // applied before it; the older key's deadline is also the earlier one.
    bool behindOlderCommit = false;
    {
        std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
        for (const auto &entry : _pendingAsyncKeyMessages)
        {
            if (IsCommitReplyPollMessage(entry.second.message) && entry.second.commitReplyDeadlineTick != 0 &&
                entry.second.requestId < request.requestId)
            {
                behindOlderCommit = true;
                break;
            }
        }
    }

    // Past its own deadline the slice reads anyway, so this key can never wait
    // longer than its window.
    if (!behindOlderCommit || GetTickCount64() >= request.commitReplyDeadlineTick)
    {
        FanyImeNamedpipeDataToTsf *reply = nullptr;
        const CommitReplyWaitStatus status =
            TryReadCommitReplySlice(request.requestId, request.commitReplyDeadlineTick, &reply);
        if (status != CommitReplyWaitStatus::NotReady)
        {
            *receivedData = reply;
            return true;
        }
    }

    // Yield instead of blocking: keep the request pending and wake on the poll
    // timer, so the host message pump runs until the next slice. The fresh
    // dispatch revalidates the token, the focus token and the composition
    // epoch before touching state.
    const uint64_t requestId = request.requestId;
    const uint64_t deferredReplayToken = request.deferredReplayToken;
    if (_RequeueAsyncKeyRequest(token, message, std::move(request)))
    {
        return false;
    }

    // The request was delivered but can no longer be re-posted, so its reply may
    // never be observed. That is the same "delivered but unanswered" state the
    // transport branch reports: drop the key and never resend it.
    _FailUnansweredCommitKey(requestId, deferredReplayToken);
    return false;
}

bool CMetasequoiaIME::_FetchCommitReplyForEditSession(UINT message, UINT token, AsyncKeyRequest &request)
{
    FanyImeNamedpipeDataToTsf *receivedData = nullptr;
    if (!_PollCommitReply(message, token, request, &receivedData))
    {
        return false;
    }
    if (receivedData->msg_type == Global::DataFromServerMsgType::TransportUnavailable)
    {
        // Settled here rather than handed to the session: the transport is
        // already being reset, which may clear the reply cache before the
        // session runs, and a session that misses the cache would block the
        // host again and tear down the freshly reconnected pipe.
        _FailUnansweredCommitKey(request.requestId, request.deferredReplayToken);
        return false;
    }
    // The session that applies the key must not wait for the same reply again.
    CacheCommitReplyForEditSession(request.requestId, *receivedData);
    return true;
}

void CMetasequoiaIME::_FailUnansweredCommitKey(uint64_t requestId, uint64_t deferredReplayToken)
{
    const DeferredKeyFailureReason reason = IsDeliveredServerRequestId(requestId)
                                                ? DeferredKeyFailureReason::DeliveryAmbiguous
                                                : DeferredKeyFailureReason::TransportBroken;
    if (deferredReplayToken != 0)
    {
        _FailDeferredKey(deferredReplayToken, reason);
    }
    else
    {
        _ResetSessionAfterFailure(DeferredKeyFailureKind::Transport);
    }
}

void CMetasequoiaIME::_FailSupersededAsyncKeyRequest(const AsyncKeyRequest &request)
{
    // A key that yielded waiting for its commit reply was delivered: the Server
    // may already have committed while the host ended the composition. Dropping
    // it as stale would leave the Server ahead of TSF. A focus change already
    // discarded everything the key belonged to, so that one stays stale.
    if (request.commitReplyDeadlineTick != 0 && _IsFocusSessionCurrent(request.focusToken))
    {
        _FailUnansweredCommitKey(request.requestId, request.deferredReplayToken);
        return;
    }
    _FailDeferredKey(request.deferredReplayToken, DeferredKeyFailureReason::Superseded);
}

void CMetasequoiaIME::_ClearAsyncKeyRequests()
{
    std::vector<uint64_t> deferredReplayTokens;
    {
        std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
        for (const auto &entry : _pendingAsyncKeyMessages)
        {
            if (entry.second.deferredReplayToken != 0)
            {
                deferredReplayTokens.push_back(entry.second.deferredReplayToken);
            }
        }
        _pendingAsyncKeyMessages.clear();
    }
    for (uint64_t replayToken : deferredReplayTokens)
    {
        _FailDeferredKey(replayToken, DeferredKeyFailureReason::Superseded);
    }
}

void CMetasequoiaIME::_ClearPendingIpcRequests()
{
    _ClearAsyncKeyRequests();
    ResetNamedpipeReplyState();

    std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
    _pendingServerCommitMessages.clear();
    _pendingWorkerSwitchMessages.clear();
    _pendingServerCandidateString.clear();
    _hasPendingServerCandidate = false;
    _pendingServerCandidateMsgType = Global::DataFromServerMsgType::OutofRange;
}
