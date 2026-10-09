#include "Private.h"
#include "KeyHandlerEditSession.h"
#include "EditSession.h"
#include "MetasequoiaIME.h"
#include "CompositionProcessorEngine.h"
#include "KeyStateCategory.h"
#include "Ipc.h"
#include "fmt/xchar.h"
#include "../Utils/PerfTimer.h"

//////////////////////////////////////////////////////////////////////
//
//    ITfEditSession
//        CEditSessionBase
// CKeyHandlerEditSession class
//
//////////////////////////////////////////////////////////////////////

//+---------------------------------------------------------------------------
//
// CKeyHandlerEditSession::DoEditSession
//
//----------------------------------------------------------------------------

STDAPI CKeyHandlerEditSession::DoEditSession(TfEditCookie ec)
{
    struct DeferredReplayCompletion
    {
        CMetasequoiaIME *textService;
        uint64_t token;
        bool applied = false;
        // Until the key handler runs, a return means the session was rejected
        // as belonging to an older focus token, composition epoch or reset.
        bool superseded = true;
        bool deliveryAmbiguous = false;
        bool transportBroken = false;
        bool projectionInvalidated = false;
        ~DeferredReplayCompletion()
        {
            if (textService && token != 0)
            {
                if (applied)
                {
                    textService->_CompleteDeferredKeyReplay(token);
                }
                else
                {
                    textService->_FailDeferredKey(token,
                                                  ClassifyEditSessionFailure(superseded, deliveryAmbiguous,
                                                                             transportBroken, projectionInvalidated));
                }
            }
        }
    } deferredReplayCompletion{_pTextService, _deferredReplayToken, false};

    const bool isLocalReset = _localResetToken != 0;
    if (isLocalReset)
    {
        if (!_pTextService->_IsLocalSessionResetCurrent(_localResetToken))
        {
            // A newer focus/reset token may supersede this granted session.
            // It must still retire its exact queue slot so Complete can queue
            // the newer token; returning without bookkeeping would gate every
            // subsequent key forever.
            _pTextService->_CompleteLocalSessionReset(_localResetToken);
            DebugTsfIssue47(L"edit-session-rejected-reset-token", _requestId, _uCode, _wch, _KeyState.Category,
                            _KeyState.Function, 1, _pTextService->_IsComposing(), 0, S_FALSE, _deferredReplayToken);
            return S_FALSE;
        }
    }
    else if (!_pTextService->_IsServerUnavailableFallbackActive() &&
             !_pTextService->_IsFocusSessionCurrent(_focusToken, _pContext))
    {
        DebugTsfIssue47(L"edit-session-rejected-focus", _requestId, _uCode, _wch, _KeyState.Category,
                        _KeyState.Function, 1, _pTextService->_IsComposing(), 0, S_FALSE, _deferredReplayToken);
        return S_FALSE;
    }
    if (!isLocalReset && _expectedCompositionEpoch != 0 &&
        !_pTextService->_IsCompositionEpochCurrent(_expectedCompositionEpoch))
    {
        DebugTsfIssue47(L"edit-session-rejected-epoch", _requestId, _uCode, _wch, _KeyState.Category,
                        _KeyState.Function, 1, _pTextService->_IsComposing(), 0, S_FALSE, _deferredReplayToken);
        return S_FALSE;
    }

    deferredReplayCompletion.superseded = false;
    HRESULT hResult = S_OK;
    PerfTimer doEditSessionTimer;
    LARGE_INTEGER freq, nowQpc;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&nowQpc);
    double queueElapsedMs =
        static_cast<double>(nowQpc.QuadPart - _requestStartQpc.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    DebugTsfKeyLatency(L"edit-session-queue", _requestId, queueElapsedMs, S_OK);

    CKeyStateCategoryFactory *pKeyStateCategoryFactory = CKeyStateCategoryFactory::Instance();
    CKeyStateCategory *pKeyStateCategory =
        pKeyStateCategoryFactory ? pKeyStateCategoryFactory->MakeKeyStateCategory(_KeyState.Category, _pTextService)
                                 : nullptr;

    if (pKeyStateCategory)
    {
        KeyHandlerEditSessionDTO keyHandlerEditSessioDTO(ec, _pContext, _uCode, _wch, _KeyState.Function, _requestId,
                                                         _prefetchedText);
        hResult = pKeyStateCategory->KeyStateHandler(_KeyState.Function, keyHandlerEditSessioDTO);
        deferredReplayCompletion.applied = hResult == S_OK;
        deferredReplayCompletion.deliveryAmbiguous = hResult == FANY_E_COMMIT_REPLY_AMBIGUOUS;
        deferredReplayCompletion.transportBroken = hResult == HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
        deferredReplayCompletion.projectionInvalidated = hResult == FANY_S_PROJECTION_INVALIDATED;

        pKeyStateCategory->Release();
    }
    if (pKeyStateCategoryFactory)
    {
        pKeyStateCategoryFactory->Release();
    }

    if (isLocalReset)
    {
        _pTextService->_CompleteLocalSessionReset(_localResetToken);
    }
    DebugTsfKeyLatency(L"edit-session-body", _requestId, doEditSessionTimer.ElapsedMs(), hResult);
    DebugTsfIssue47(L"edit-session-complete", _requestId, _uCode, _wch, _KeyState.Category, _KeyState.Function, 1,
                    _pTextService->_IsComposing(), 0, hResult, _deferredReplayToken);
    return hResult;
}
