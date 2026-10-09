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

void ClearReleasedShiftModifierState()
{
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
    {
        return;
    }

    Global::IsShiftKeyDownOnly = FALSE;
    Global::PureShiftKeyDown = FALSE;
    Global::PureShiftKeyUp = FALSE;
    Global::ModifiersValue &= ~(TF_MOD_SHIFT | TF_MOD_LSHIFT | TF_MOD_RSHIFT);
}
} // namespace

namespace key_event_sink_detail
{
// Ctrl+Backspace inside a composition deletes one input unit and Ctrl+Left /
// Ctrl+Right move one unit. These are the only Ctrl chords the IME claims:
// Shift, Alt and the Windows keys keep their host meaning, as do all three
// chords while no composition is active. The returned function is the one a
// claim site must classify the key as; FUNCTION_NONE means "host key".
KEYSTROKE_FUNCTION SegmentEditFunction(UINT code, UINT modifiers)
{
    if ((modifiers & 0b00000111u) != 0b00000010u || (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)
    {
        return FUNCTION_NONE;
    }
    switch (code)
    {
    case VK_BACK:
        return FUNCTION_BACKSPACE_SEGMENT;
    case VK_LEFT:
        return FUNCTION_MOVE_LEFT_SEGMENT;
    case VK_RIGHT:
        return FUNCTION_MOVE_RIGHT_SEGMENT;
    default:
        return FUNCTION_NONE;
    }
}

UINT CaptureIpcModifiers()
{
    UINT modifiers = 0;
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
        modifiers |= 0b00000001;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
        modifiers |= 0b00000010;
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
        modifiers |= 0b00000100;
    return modifiers;
}

bool IsEnglishInputModeToggle(UINT code, UINT modifiers)
{
    // Ctrl+Shift+E, without Alt.
    return code == 'E' && (modifiers & 0b00000111u) == 0b00000011u;
}

// Ctrl+Enter, without Shift/Alt/Windows: commit the translation shown to the right of the
// highlighted candidate. The Server decides what that is (one translation commits directly,
// several open a second candidate list), so this only has to reach it as a candidate key.
bool IsTranslationCommitShortcut(UINT code, UINT modifiers)
{
    return code == VK_RETURN && (modifiers & 0b00000111u) == 0b00000010u && (GetAsyncKeyState(VK_LWIN) & 0x8000) == 0 &&
           (GetAsyncKeyState(VK_RWIN) & 0x8000) == 0;
}

bool IsPinyinCommitShortcut(UINT code, UINT modifiers)
{
    return code == VK_RETURN && (modifiers & 0b00000111u) == 0b00000001u && (GetAsyncKeyState(VK_LWIN) & 0x8000) == 0 &&
           (GetAsyncKeyState(VK_RWIN) & 0x8000) == 0;
}

bool IsCharacterSetInputModeToggle(UINT code, UINT modifiers)
{
    return FanyImeProtocol::IsCharacterSetShortcut(code, modifiers) && (GetAsyncKeyState(VK_LWIN) & 0x8000) == 0 &&
           (GetAsyncKeyState(VK_RWIN) & 0x8000) == 0 && SupportsCharacterSetShortcut() &&
           FanyUtils::ReadConfiguredSwitchLanguageHotkeys().character_set_ctrl_shift_f;
}

bool IsTrilingualCycleEnabled()
{
    return Global::TrilingualCycleEnabled.load(std::memory_order_relaxed) &&
           (GetAsyncKeyState(VK_LWIN) & 0x8000) == 0 && (GetAsyncKeyState(VK_RWIN) & 0x8000) == 0 &&
           SupportsTrilingualCycle();
}

void PostOwnerMessageWithSyncFallback(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!window || !IsWindow(window))
    {
        return;
    }
    if (!PostMessage(window, message, wParam, lParam) &&
        GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId())
    {
        SendMessage(window, message, wParam, lParam);
    }
}

bool IsShiftVk(UINT code)
{
    return code == VK_SHIFT || code == VK_LSHIFT || code == VK_RSHIFT;
}
} // namespace key_event_sink_detail

//+---------------------------------------------------------------------------
//
// _IsCompositionActiveForKeyGuard
//
// The "real" composition liveness used by the synchronous key paths (the
// deferred classifier tests its projection instead). word_for_creating_word
// covers the intermediate state where the last selected segment's raw spelling
// still belongs to the Server and the composition would otherwise look empty.
//----------------------------------------------------------------------------

bool CMetasequoiaIME::_IsCompositionActiveForKeyGuard()
{
    if (_IsComposing() != FALSE)
    {
        return true;
    }
    if (_pCompositionProcessorEngine != nullptr && _pCompositionProcessorEngine->GetVirtualKeyLength() > 0)
    {
        return true;
    }
    return !GlobalIme::word_for_creating_word.empty();
}

//+---------------------------------------------------------------------------
//
// _ApplyBackspaceHoldGuard
//
// State transition for every VK_BACK key-down the sinks classify. A fresh
// press (no repeat bit) re-evaluates whether this hold began inside a
// composition; a repeat is claimed only when the hold did and the composition
// is already gone, so the key can never fall through to the host and delete
// document text (#347).
//----------------------------------------------------------------------------

bool CMetasequoiaIME::_ApplyBackspaceHoldGuard(WPARAM wParam, LPARAM lParam)
{
    if (static_cast<UINT>(wParam) != VK_BACK)
    {
        return false;
    }
    if (!IsAutoRepeat(lParam))
    {
        _backspaceHoldArmed = _IsCompositionActiveForKeyGuard();
        return false;
    }
    return ShouldSuppressBackspaceRepeat(_backspaceHoldArmed, _IsCompositionActiveForKeyGuard(), true);
}

void CMetasequoiaIME::_ApplyCapsLockKeyDownSideEffects(bool capsLockEnabled)
{
    Global::CapsLockEnabled.store(capsLockEnabled, std::memory_order_relaxed);
    _RequestLanguageBarCapsIconRefresh();
    if (_pCompositionProcessorEngine)
    {
        const bool imeOpen = _pCompositionProcessorEngine->GetIMEMode(_GetThreadMgr(), _GetClientId()) != FALSE;
        _pCompositionProcessorEngine->SendCaretStateSwitchEvent(
            FanyImePipeEventType::IMESwitch, imeOpen, FanyImeCaretStateTrigger::CapsLockEdge, capsLockEnabled);
    }
}

//+---------------------------------------------------------------------------
//
// _IsKeyEaten
//
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_IsKeyEaten(         //
    _In_ ITfContext *pContext,             //
    UINT codeIn,                           //
    _Out_ UINT *pCodeOut,                  //
    _Out_writes_(1) WCHAR *pwch,           //
    _Out_opt_ _KEYSTROKE_STATE *pKeyState, //
    _In_opt_ const WCHAR *translatedWch,   //
    bool freshCompositionState             //
)
{
    pContext;

    *pCodeOut = codeIn;

    BOOL isOpen = FALSE;
    CCompartment CompartmentKeyboardOpen(_pThreadMgr, _tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    CompartmentKeyboardOpen._GetCompartmentBOOL(isOpen);

    BOOL isDoubleSingleByte = FALSE;
    CCompartment CompartmentDoubleSingleByte(_pThreadMgr, _tfClientId,
                                             Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte);
    CompartmentDoubleSingleByte._GetCompartmentBOOL(isDoubleSingleByte);

    BOOL isPunctuation = FALSE;
    CCompartment CompartmentPunctuation(_pThreadMgr, _tfClientId, Global::MetasequoiaIMEGuidCompartmentPunctuation);
    CompartmentPunctuation._GetCompartmentBOOL(isPunctuation);

    if (pKeyState)
    {
        pKeyState->Category = CATEGORY_NONE;
        pKeyState->Function = FUNCTION_NONE;
    }
    if (pwch)
    {
        *pwch = L'\0';
    }

    // If the keyboard is disabled(e.g. no focused edit control), we don't eat keys.
    if (_IsKeyboardDisabled())
    {
        return FALSE;
    }

    //
    // Map virtual key to character code
    //
    BOOL isTouchKeyboardSpecialKeys = FALSE;
    WCHAR wch = translatedWch ? *translatedWch : ConvertVKey(codeIn);
    *pCodeOut = VKeyFromVKPacketAndWchar(codeIn, wch);
    if ((wch == THIRDPARTY_NEXTPAGE) || (wch == THIRDPARTY_PREVPAGE))
    {
        // We always eat the above softkeyboard special keys
        isTouchKeyboardSpecialKeys = TRUE;
        if (pwch)
        {
            *pwch = wch;
        }
    }

    // if the keyboard is closed, we don't eat keys, with the exception of the touch keyboard specials keys
    if (!isOpen && !isDoubleSingleByte && !isPunctuation)
    {
        return isTouchKeyboardSpecialKeys;
    }

    if (pwch)
    {
        *pwch = wch;
    }

    //
    // Get composition engine
    //
    CCompositionProcessorEngine *pCompositionProcessorEngine;
    pCompositionProcessorEngine = _pCompositionProcessorEngine;

    if (isOpen) // Chinese mode
    {
        const UINT shortcutModifiers = CaptureIpcModifiers();
        if (!_serverUnavailableFallbackActive && IsCharacterSetInputModeToggle(*pCodeOut, shortcutModifiers))
        {
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_COMPOSING;
                pKeyState->Function = FUNCTION_TOGGLE_CHARACTER_SET;
            }
            return TRUE;
        }
        // Keep the Chinese compartment open: this only toggles the
        // Server-owned English candidate sub-mode.
        if (IsEnglishInputModeToggle(*pCodeOut, shortcutModifiers))
        {
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_COMPOSING;
                pKeyState->Function = FUNCTION_CANCEL;
            }
            return TRUE;
        }

        // Ctrl+Enter commits a candidate translation. Shift+Enter commits the
        // Server's converted spelling, including compositions without candidates.
        if (!freshCompositionState &&
            ((_candidateMode != CANDIDATE_NONE && IsTranslationCommitShortcut(*pCodeOut, shortcutModifiers)) ||
             (!_serverUnavailableFallbackActive && pCompositionProcessorEngine->GetVirtualKeyLength() > 0 &&
              IsPinyinCommitShortcut(*pCodeOut, shortcutModifiers))))
        {
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_SERVER_CANDIDATE_KEY;
            }
            return TRUE;
        }

        // Other Ctrl/Alt/Windows combinations belong to the application.
        // IME-owned shortcuts are handled before this normal key classifier.
        if ((shortcutModifiers & 0b00000110u) != 0 || (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)
        {
            // Ctrl+Backspace / Ctrl+Left / Ctrl+Right inside a composition are
            // the one exception: they edit one input unit, whose length only
            // the Server can decide.
            const KEYSTROKE_FUNCTION segmentEdit = SegmentEditFunction(*pCodeOut, shortcutModifiers);
            if (segmentEdit != FUNCTION_NONE && _IsCompositionActiveForKeyGuard())
            {
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = segmentEdit;
                }
                return TRUE;
            }
            return isTouchKeyboardSpecialKeys;
        }

        const bool isComposing = freshCompositionState ? false : _IsComposing() != FALSE;
        const CANDIDATE_MODE candidateMode = freshCompositionState ? CANDIDATE_NONE : _candidateMode;
        const bool isCapsLockOn = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
        const bool isUppercaseAlphabet = (wch >= L'A' && wch <= L'Z') && (*pCodeOut >= L'A' && *pCodeOut <= L'Z');
        const bool isInputInProgress =
            !freshCompositionState &&
            (isComposing || (candidateMode != CANDIDATE_NONE) ||
             (pCompositionProcessorEngine && pCompositionProcessorEngine->GetVirtualKeyLength() > 0));

        // CapsLock ON + uppercase(没有按 Shift) alphabet:
        // - start of input: don't eat
        // - middle of input: eat
        if (isCapsLockOn && isUppercaseAlphabet && !isInputInProgress)
        {
            return isTouchKeyboardSpecialKeys;
        }

        //
        // The candidate or phrase list handles the keys through ITfKeyEventSink.
        //
        // eat only keys that CKeyHandlerEditSession can handles.
        //
        const BOOL ret =
            freshCompositionState
                ? pCompositionProcessorEngine->IsVirtualKeyNeedForFreshComposition(*pCodeOut, pwch, pKeyState)
                : pCompositionProcessorEngine->IsVirtualKeyNeed(*pCodeOut, pwch, isComposing, candidateMode,
                                                                _isCandidateWithWildcard, pKeyState);
        if (ret)
        {
            return TRUE;
        }

        // Reversible smart punctuation: a space right after a committed Chinese
        // punctuation converts it, and the punctuation key that produced an
        // ASCII conversion reverts it. The state already carries the focus and
        // foreground checks; here only the input modes are added, and the
        // classification must match _DispatchKeyDown's probe exactly so a
        // claimed key is never handed back.
        if (isPunctuation && !isDoubleSingleByte && !isComposing && candidateMode == CANDIDATE_NONE &&
            !Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed))
        {
            if (wch == L' ' && _CanInterceptSmartPunctuationConvert())
            {
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_SMART_PUNCTUATION_CONVERT;
                }
                return TRUE;
            }
            if (*pCodeOut != VK_DECIMAL && _CanInterceptSmartPunctuationRevert(wch))
            {
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_SMART_PUNCTUATION_REVERT;
                }
                return TRUE;
            }
        }
    }

    //
    // Punctuation
    //
    if (pCompositionProcessorEngine->IsPunctuation(wch))
    {
        const CANDIDATE_MODE candidateMode = freshCompositionState ? CANDIDATE_NONE : _candidateMode;
        if ((candidateMode == CANDIDATE_NONE || candidateMode == CANDIDATE_INCREMENTAL) && isPunctuation)
        {
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_COMPOSING;
                pKeyState->Function = FUNCTION_PUNCTUATION;
            }
            return TRUE;
        }
    }

    //
    // Double/Single byte
    //
    if (isDoubleSingleByte && pCompositionProcessorEngine->IsDoubleSingleByte(wch))
    {
        if ((freshCompositionState ? CANDIDATE_NONE : _candidateMode) == CANDIDATE_NONE)
        {
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_COMPOSING;
                pKeyState->Function = FUNCTION_DOUBLE_SINGLE_BYTE;
            }
            return TRUE;
        }
    }

    return isTouchKeyboardSpecialKeys;
}

//+---------------------------------------------------------------------------
//
// ConvertVKey
//
//----------------------------------------------------------------------------

WCHAR CMetasequoiaIME::ConvertVKey(UINT code)
{
    //
    // Map virtual key to scan code
    //
    UINT scanCode = 0;
    scanCode = MapVirtualKey(code, 0);

    //
    // Keyboard state
    //
    BYTE abKbdState[256] = {'\0'};
    if (!GetKeyboardState(abKbdState))
    {
        return 0;
    }

    //
    // Map virtual key to character code
    //
    WCHAR wch = '\0';
    if (ToUnicode(code, scanCode, abKbdState, &wch, 1, 0) == 1)
    {
        return wch;
    }

    return 0;
}

//+---------------------------------------------------------------------------
//
// _IsKeyboardDisabled
//
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_IsKeyboardDisabled()
{
    /* Steal from weasel: https://github.com/rime/weasel */
    ITfCompartmentMgr *pCompMgr = NULL;
    ITfDocumentMgr *pDocMgrFocus = NULL;
    ITfContext *pContext = NULL;
    BOOL fDisabled = FALSE;

    if ((_pThreadMgr->GetFocus(&pDocMgrFocus) != S_OK) || (pDocMgrFocus == NULL))
    {
        fDisabled = TRUE;
        goto Exit;
    }

    if ((pDocMgrFocus->GetTop(&pContext) != S_OK) || (pContext == NULL))
    {
        fDisabled = TRUE;
        goto Exit;
    }

    if (pContext->QueryInterface(IID_ITfCompartmentMgr, (void **)&pCompMgr) == S_OK)
    {
        ITfCompartment *pCompartmentDisabled;
        ITfCompartment *pCompartmentEmptyContext;

        /* Check GUID_COMPARTMENT_KEYBOARD_DISABLED */
        if (pCompMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_DISABLED, &pCompartmentDisabled) == S_OK)
        {
            VARIANT var;
            if (pCompartmentDisabled->GetValue(&var) == S_OK)
            {
                if (var.vt == VT_I4) // Even VT_EMPTY, GetValue() can succeed
                    fDisabled = (BOOL)var.lVal;
            }
            pCompartmentDisabled->Release();
        }

        /* Check GUID_COMPARTMENT_EMPTYCONTEXT */
        if (pCompMgr->GetCompartment(GUID_COMPARTMENT_EMPTYCONTEXT, &pCompartmentEmptyContext) == S_OK)
        {
            VARIANT var;
            if (pCompartmentEmptyContext->GetValue(&var) == S_OK)
            {
                if (var.vt == VT_I4) // Even VT_EMPTY, GetValue() can succeed
                    fDisabled = (BOOL)var.lVal;
            }
            pCompartmentEmptyContext->Release();
        }
        pCompMgr->Release();
    }

Exit:
    if (pContext)
        pContext->Release();
    if (pDocMgrFocus)
        pDocMgrFocus->Release();
    return fDisabled;
}

//+---------------------------------------------------------------------------
//
// ITfKeyEventSink::OnSetFocus
//
// Called by the system whenever this service gets the keystroke device focus.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnSetFocus(BOOL fForeground)
{
    // Activation can precede keystroke focus (Notepad TIP reload). TSF need
    // not send another document-focus callback before delivering keys.
    if (ShouldRecoverNamedpipeOnKeyFocus(fForeground != FALSE, Global::g_connected, IsNamedpipeFocusStateOwner(this)))
    {
        Global::g_connected = true;
        _workerCommitReady.store(false, std::memory_order_release);
        RequireNamedpipeFocusActivation();
        PostOwnerMessageWithSyncFallback(_msgWndHandle, WM_ConnectNamedpipe);
    }

    // A hold must never carry its guard into another input context.
    _backspaceHoldArmed = false;

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _NotePassthroughStatistics
//
// Counts one printable character that this tip hands back to the application.
// The three composition commit exits never see these keys -- the host inserts
// them -- so this is the only capture point for half-width digits, the symbols
// outside the punctuation table and English-mode letters (stats_passthrough.h).
// Observation only: the eaten result, the deferred queue and the edit path stay
// untouched, and every failure mode is a dropped count.
//----------------------------------------------------------------------------

void CMetasequoiaIME::_NotePassthroughStatistics(UINT virtualKey, WCHAR wch, bool keyboardKnownEnabled)
{
    if (!Global::StatisticsEnabled.load(std::memory_order_relaxed))
    {
        // With the switch off nothing is classified and no frame is written;
        // the de-duplication marker is left alone because nothing was counted.
        return;
    }

    const LONG messageTime = GetMessageTime();
    if (virtualKey != 0 && virtualKey == _passthroughStatsVirtualKey && messageTime == _passthroughStatsMessageTime)
    {
        // The system can query the same key event more than once (a host may
        // also call the keystroke manager directly); only the first pass counts.
        // The marker is consumed here: the probes for one event arrive back to
        // back, so anything later is a genuine second press that GetMessageTime
        // cannot separate from the first one inside the same tick.
        _passthroughStatsVirtualKey = 0;
        return;
    }

    if (wch == L'\0')
    {
        // The keyboard-closed early return in _IsKeyEaten leaves its out-char
        // blank even though the key reaches the application; widen it from the
        // layout here. Keys that genuinely produce no character keep the zero
        // and are dropped by the printable check below.
        wch = ConvertVKey(virtualKey);
    }

    // The same physical-state read _IsKeyEaten uses for application-owned
    // combinations. Shift deliberately does not participate: it is what makes
    // uppercase letters and the shifted symbol row their own characters.
    const UINT modifiers = CaptureIpcModifiers();
    const bool ctrlDown = (modifiers & 0b00000010u) != 0;
    const bool altDown = (modifiers & 0b00000100u) != 0;
    const bool winDown = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

    // A non-zero out-char from _IsKeyEaten passed that function's own
    // keyboard-disabled check, so the compartment query is only needed when the
    // caller could not prove the keyboard was live. _ClassifyDeferredKeyDown
    // fills its out-char before that check, so its two callers never set
    // keyboardKnownEnabled. Self-generated SendInput never reaches here:
    // OnTestKeyDown rejects it up front, and eaten is false by construction
    // because this only runs on the uneaten exits.
    const bool keyboardDisabled = !keyboardKnownEnabled && _IsKeyboardDisabled() != FALSE;
    const bool counted = MsimeStats::ShouldCountPassthroughChar(wch, /*eaten=*/false, /*selfGenerated=*/false,
                                                                keyboardDisabled, ctrlDown, altDown, winDown);
    if (!counted)
    {
        return;
    }

    _passthroughStatsVirtualKey = virtualKey;
    _passthroughStatsMessageTime = messageTime;
    MsimeStats::QueueStatisticsEvent(MsimeStats::ClassifyText(&wch, 1));
}

//+---------------------------------------------------------------------------
//
// ITfKeyEventSink::OnTestKeyDown
//
// Called by the system to query this service wants a potential keystroke.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnTestKeyDown(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pIsEaten)
{
    if (pContext == nullptr || pIsEaten == nullptr)
    {
        return E_INVALIDARG;
    }
    if (IsSelfGeneratedSendInputExtraInfo(static_cast<ULONG_PTR>(GetMessageExtraInfo())))
    {
        _capsLockTestKeyDownPending = false;
        *pIsEaten = FALSE;
        return S_OK;
    }
    const DWORD testKeyMessageTime = static_cast<DWORD>(GetMessageTime());
    const bool repeatedTestKeyDown =
        _capsLockTestKeyDownPending && _capsLockTestKeyDownMessageTime == testKeyMessageTime;
    _capsLockTestKeyDownPending = false;
    if (IsFreshCapsLockKeyDown(wParam, lParam))
    {
        // TestKeyDown observes the toggle before Windows applies this press.
        // It may run more than once for one key event; apply the edge once.
        if (!repeatedTestKeyDown)
        {
            const bool capsLockEnabled = ResultingCapsLockState(false, (GetKeyState(VK_CAPITAL) & 0x0001) != 0);
            _ApplyCapsLockKeyDownSideEffects(capsLockEnabled);
        }
        _capsLockTestKeyDownMessageTime = testKeyMessageTime;
        _capsLockTestKeyDownPending = true;
    }
    PerfTimer onTestKeyDownTimer;
    Global::UpdateModifiers(wParam, lParam);
    _TrackModifierHotkeyArming(wParam, lParam, false);
    if (IsShiftVk(LOWORD(wParam)))
    {
        *pIsEaten = FALSE;
        return S_OK;
    }

    // Backspace hold guard (#347). This sink sees every key-down, including the
    // ones later handed back to the application, so the arm state is refreshed
    // here as well as in _DispatchKeyDown. A suppressed repeat never reaches
    // the host; the pending smart-punctuation action still observes it as one
    // more key that invalidates the last conversion.
    if (_ApplyBackspaceHoldGuard(wParam, lParam))
    {
        const WCHAR guardWch = ConvertVKey(VK_BACK);
        *pIsEaten = TRUE;
        _NoteKeyForSmartPunctuation(VK_BACK, guardWch, true, FUNCTION_BACKSPACE);
        return S_OK;
    }

    if (_HasDeferredKeyBarrier())
    {
        _KEYSTROKE_STATE deferredState = {};
        WCHAR deferredWch = L'\0';
        UINT deferredCode = 0;
        if (!_DeferredKeyQueueHasCapacity())
        {
            // Still observe Backspace for smart-punctuation rejection. Uneaten
            // keys often never reach OnKeyDown, and this is the only sink that
            // always sees them.
            deferredWch = ConvertVKey(static_cast<UINT>(wParam));
            deferredCode = VKeyFromVKPacketAndWchar(static_cast<UINT>(wParam), deferredWch);
            _NoteKeyForSmartPunctuation(deferredCode, deferredWch, false, FUNCTION_NONE);
            // _ClassifyDeferredKeyDown is not reached on this exit and ConvertVKey
            // fills the char without checking the keyboard state.
            _NotePassthroughStatistics(static_cast<UINT>(wParam), deferredWch, false);
            *pIsEaten = FALSE;
            return S_OK;
        }

        // Reversible smart punctuation is a local action and must not wait for
        // the FIFO to drain. OnTestKeyDown is the sink that decides whether
        // OnKeyDown — and therefore the KeyDown probe — ever runs, so the same
        // _IsKeyEaten claim the probe replays has to be made here too. The
        // deferred classifier knows nothing about it (the request key would
        // otherwise be queued as an ordinary convert/punctuation key).
        {
            _KEYSTROKE_STATE smartState = {};
            WCHAR smartWch = L'\0';
            UINT smartCode = 0;
            if (_IsKeyEaten(pContext, static_cast<UINT>(wParam), &smartCode, &smartWch, &smartState) &&
                (smartState.Function == FUNCTION_SMART_PUNCTUATION_CONVERT ||
                 smartState.Function == FUNCTION_SMART_PUNCTUATION_REVERT))
            {
                *pIsEaten = TRUE;
                _NoteKeyForSmartPunctuation(smartCode, smartWch, true, smartState.Function);
                return S_OK;
            }
        }

        *pIsEaten = _ClassifyDeferredKeyDown(pContext, wParam, lParam, nullptr, nullptr, &deferredWch, &deferredCode,
                                             &deferredState)
                        ? TRUE
                        : FALSE;
        // Classify always fills code/wch before failing. Track rejection even
        // when the key is handed back to the app (typical for VK_BACK).
        _NoteKeyForSmartPunctuation(deferredCode, deferredWch, *pIsEaten ? true : false, deferredState.Function);
        if (!*pIsEaten)
        {
            // The deferred classifier fills its out-char before its own
            // keyboard-disabled check, and not every exit runs that check, so
            // the char proves nothing about the keyboard state.
            _NotePassthroughStatistics(static_cast<UINT>(wParam), deferredWch, false);
        }
        return S_OK;
    }

    GUID hotkeyGuid = {};
    if (_MatchChordInputHotkey(wParam, &hotkeyGuid))
    {
        *pIsEaten = TRUE;
        return S_OK;
    }

    _KEYSTROKE_STATE KeystrokeState;
    WCHAR wch = '\0';
    UINT code = 0;
    *pIsEaten = _IsKeyEaten(pContext, (UINT)wParam, &code, &wch, &KeystrokeState);

    // Every keydown reaches this sink, including the ones handed back to the
    // application (backspace with no composition), so the smart-punctuation
    // rejection state is tracked here rather than in the eaten-key path.
    _NoteKeyForSmartPunctuation(code, wch, *pIsEaten ? true : false, KeystrokeState.Function);

    if (!*pIsEaten)
    {
        // A half-width digit or a symbol outside the tables lands here: the tip
        // let it through, so the host will insert it outside every commit exit.
        _NotePassthroughStatistics(static_cast<UINT>(wParam), wch, wch != L'\0');
    }

    DebugTsfIssue47(L"test-keydown-classified", FANY_IME_NO_REQUEST_ID, code, wch, KeystrokeState.Category,
                    KeystrokeState.Function, *pIsEaten ? 1 : 0, _IsComposing(),
                    _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_OK);

    if (KeystrokeState.Category == CATEGORY_INVOKE_COMPOSITION_EDIT_SESSION)
    {
        //
        // Invoke key handler edit session
        //
        KeystrokeState.Category = CATEGORY_COMPOSING;

        _InvokeKeyHandler(pContext, code, wch, (DWORD)lParam, KeystrokeState, FANY_IME_NO_REQUEST_ID);
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfKeyEventSink::OnKeyDown
//
// Called by the system to offer this service a keystroke.
// on exit, the application will not handle the keystroke.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnKeyDown(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pIsEaten)
{
    if (pContext == nullptr || pIsEaten == nullptr)
    {
        return E_INVALIDARG;
    }
    if (IsSelfGeneratedSendInputExtraInfo(static_cast<ULONG_PTR>(GetMessageExtraInfo())))
    {
        _capsLockTestKeyDownPending = false;
        *pIsEaten = FALSE;
        return S_OK;
    }
    const bool matchingTestKeyDownHandled =
        _capsLockTestKeyDownPending && _capsLockTestKeyDownMessageTime == static_cast<DWORD>(GetMessageTime());
    _capsLockTestKeyDownPending = false;
    if (ShouldApplyCapsLockActualKeyDownSideEffects(matchingTestKeyDownHandled, wParam, lParam))
    {
        // Unlike TestKeyDown, the actual callback observes the resulting toggle state.
        const bool capsLockEnabled = ResultingCapsLockState(true, (GetKeyState(VK_CAPITAL) & 0x0001) != 0);
        _ApplyCapsLockKeyDownSideEffects(capsLockEnabled);
    }
    PerfTimer onKeyDownTimer;
    const uint64_t focusGeneration = _deferredKeyFocusGeneration;
    (void)_DispatchKeyDown(pContext, wParam, lParam, pIsEaten, nullptr, nullptr, nullptr, true, focusGeneration);
    DebugTsfKeyLatency(L"on-key-down", 0, onKeyDownTimer.ElapsedMs(), S_OK);
    return S_OK;
}

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
        if (KeystrokeState.Function == FUNCTION_CYCLE_INPUT_MODE && !SupportsTrilingualCycle())
        {
            // A reconnect may replace the peer after the language hotkey was queued.
            // Keep it eaten, but never send a cycle to an unnegotiated Server.
            // Later keys were projected into its destination, so discard that
            // queue along with the now-unusable projection.
            _ResetSessionAfterFailure(DeferredKeyFailureKind::Resync);
            return KeyDownDispatchResult::Complete;
        }

        Global::Keycode = code;
        Global::wch = wch;
        Global::ModifiersDown = capturedModifiers;
        if (KeystrokeState.Function == FUNCTION_CYCLE_INPUT_MODE)
        {
            // Mark the request explicitly and carry this host's own state: the
            // Server cycles from it, not from whichever host it last heard from.
            // Every key before this one has been applied, so it is current.
            Global::ModifiersDown |= FanyImeTrilingualInput::EncodeCycleRequest(
                _pCompositionProcessorEngine->GetIMEMode(_pThreadMgr, _tfClientId) != FALSE,
                Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed));
        }

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

        if (KeystrokeState.Function == FUNCTION_CYCLE_INPUT_MODE && _msgWndHandle)
        {
            _PostAsyncKeyRequest(WM_AsyncCycleInputMode, code, wch, requestId, {}, 0, 0, deferredReplayToken);
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

//+---------------------------------------------------------------------------
//
// ITfKeyEventSink::OnTestKeyUp
//
// Called by the system to query this service wants a potential keystroke.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnTestKeyUp(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pIsEaten)
{
    if (pContext == nullptr || pIsEaten == nullptr)
    {
        return E_INVALIDARG;
    }
    if (IsSelfGeneratedSendInputExtraInfo(static_cast<ULONG_PTR>(GetMessageExtraInfo())))
    {
        *pIsEaten = FALSE;
        return S_OK;
    }

    Global::UpdateModifiers(wParam, lParam);

    if (IsShiftVk(LOWORD(wParam)))
    {
        // TSF does not call OnKeyUp after a FALSE test result. Claim and
        // queue the bare-Shift toggle here while leaving its release visible.
        // Whichever of TestKeyUp/KeyUp a host offers first wins;
        // _MatchModifierReleaseHotkey disarms so the other one is a no-op.
        // mintty offers neither and falls back to the keyboard hook, which
        // _MarkBareShiftHandled() disarms.
        GUID hotkeyGuid = {};
        BOOL queued = FALSE;
        const bool toggled =
            _MatchModifierReleaseHotkey(wParam, &hotkeyGuid) && _QueueInputHotkey(pContext, hotkeyGuid, &queued);
        if (toggled)
        {
            _MarkBareShiftHandled();
        }
        DebugTsfIssue47(L"bare-shift-testkeyup", FANY_IME_NO_REQUEST_ID, LOWORD(wParam), L'\0', 0, 0, toggled ? 1 : 0,
                        _IsComposing(),
                        _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_OK);
        ClearReleasedShiftModifierState();
        *pIsEaten = FALSE;
        return S_OK;
    }

    if (_HasDeferredKeyBarrier())
    {
        // A matching deferred key-down may or may not have fit in the bounded
        // queue. Letting all key-ups through is harmless and guarantees the
        // application never observes a down without its release.
        *pIsEaten = FALSE;
        return S_OK;
    }

    GUID hotkeyGuid = {};
    if (_MatchModifierReleaseHotkey(wParam, &hotkeyGuid))
    {
        // A bare Ctrl release that toggles the input mode; Shift returned
        // above. Same rule as the Shift branch: the toggle does not need to
        // own the keystroke, and a host that keys off the release loses its
        // own shortcut when it is eaten (double-Ctrl is Run Anything in
        // JetBrains IDEs). Report the key as not eaten -- which also means
        // OnKeyUp will not be called, so queue the toggle here instead of
        // peeking.
        BOOL hotkeyQueued = FALSE;
        (void)_QueueInputHotkey(pContext, hotkeyGuid, &hotkeyQueued);
        *pIsEaten = FALSE;
        return S_OK;
    }

    _KEYSTROKE_STATE KeystrokeState = {};
    WCHAR wch = '\0';
    UINT code = 0;

    *pIsEaten = _IsKeyEaten(pContext, (UINT)wParam, &code, &wch, &KeystrokeState);

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfKeyEventSink::OnKeyUp
//
// Called by the system to offer this service a keystroke.  If *pIsEaten == TRUE
// on exit, the application will not handle the keystroke.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnKeyUp(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pIsEaten)
{
    if (pContext == nullptr || pIsEaten == nullptr)
    {
        return E_INVALIDARG;
    }
    if (IsSelfGeneratedSendInputExtraInfo(static_cast<ULONG_PTR>(GetMessageExtraInfo())))
    {
        *pIsEaten = FALSE;
        return S_OK;
    }
    Global::UpdateModifiers(wParam, lParam);

    if (IsShiftVk(LOWORD(wParam)))
    {
        // Defend against hosts that offer KeyUp without a preceding test.
        GUID hotkeyGuid = {};
        BOOL queued = FALSE;
        const bool toggled =
            _MatchModifierReleaseHotkey(wParam, &hotkeyGuid) && _QueueInputHotkey(pContext, hotkeyGuid, &queued);
        if (toggled)
        {
            _MarkBareShiftHandled();
        }
        DebugTsfIssue47(L"bare-shift-keyup", FANY_IME_NO_REQUEST_ID, LOWORD(wParam), L'\0', 0, 0, toggled ? 1 : 0,
                        _IsComposing(),
                        _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_OK);
        ClearReleasedShiftModifierState();
        *pIsEaten = FALSE;
        return S_OK;
    }

    if (_HasDeferredKeyBarrier())
    {
        *pIsEaten = FALSE;
        return S_OK;
    }

    GUID hotkeyGuid = {};
    if (_MatchModifierReleaseHotkey(wParam, &hotkeyGuid))
    {
        // Ctrl again; OnTestKeyUp normally ran the toggle and disarmed
        // already, so this only fires for hosts that call KeyUp without
        // TestKeyUp. Same rule as there: run the toggle, hand the release
        // back to the host.
        BOOL hotkeyQueued = FALSE;
        (void)_QueueInputHotkey(pContext, hotkeyGuid, &hotkeyQueued);
        *pIsEaten = FALSE;
        return S_OK;
    }

    _KEYSTROKE_STATE KeystrokeState = {};
    WCHAR wch = '\0';
    UINT code = 0;
    *pIsEaten = _IsKeyEaten(pContext, (UINT)wParam, &code, &wch, &KeystrokeState);

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfKeyEventSink::OnPreservedKey
//
// Called when a hotkey (registered by us, or by the system) is typed.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnPreservedKey(ITfContext *pContext, REFGUID rguid, BOOL *pIsEaten)
{
    if (pContext == nullptr || pIsEaten == nullptr)
    {
        return E_INVALIDARG;
    }
    // No TSF PreserveKey registrations remain for input-mode shortcuts.
    // Shift/Ctrl/Ctrl+Alt+Space/Ctrl+Shift+Space/Ctrl+./Ctrl+Shift+E are all
    // handled from ITfKeyEventSink.
    UNREFERENCED_PARAMETER(rguid);
    *pIsEaten = FALSE;
    return S_OK;
}

void CMetasequoiaIME::_DispatchPreservedKey(_In_ ITfContext *pContext, REFGUID preservedKey, _Out_ BOOL *pIsEaten,
                                            uint64_t expectedFocusGeneration, bool isPrevalidated,
                                            uint64_t deferredReplayToken)
{
    *pIsEaten = FALSE;
    if (pContext == nullptr || _pCompositionProcessorEngine == nullptr || expectedFocusGeneration == 0 ||
        expectedFocusGeneration != _deferredKeyFocusGeneration)
    {
        return;
    }

    BOOL pNeedToggleIMEMode = FALSE;

    _pCompositionProcessorEngine->OnPreservedKey(       //
        pContext,                                       //
        preservedKey,                                   //
        pIsEaten,                                       //
        _GetThreadMgr(),                                //
        _GetClientId(),                                 //
        &pNeedToggleIMEMode,                            //
        isPrevalidated ? TRUE : FALSE,                  //
        _serverUnavailableFallbackActive ? FALSE : TRUE //
    );

    if (pNeedToggleIMEMode && expectedFocusGeneration == _deferredKeyFocusGeneration)
    {
        // The preserved-key implementation also sends the Shift event to the
        // Server.  A failed/ambiguous send marks the local session dirty.  The
        // compartment toggle has already been applied and stays; the commit
        // phase is dropped and the transport reset cancels the composition.
        // The replacement epoch receives the authoritative status snapshot.
        if (deferredReplayToken != 0 && _localSessionResetPending.load(std::memory_order_acquire))
        {
            _FailDeferredKey(deferredReplayToken, DeferredKeyFailureReason::TransportBroken);
            return;
        }
        _KEYSTROKE_STATE KeystrokeState = {};
        WCHAR wch = '\0';
        UINT code = 0;
        KeystrokeState.Category = CATEGORY_COMPOSING;
        KeystrokeState.Function = FUNCTION_TOGGLE_IME_MODE;
        _InvokeKeyHandler(pContext, code, wch, (DWORD)0, KeystrokeState, FANY_IME_NO_REQUEST_ID, {}, 0, 0, 0,
                          deferredReplayToken);
    }
}

//+---------------------------------------------------------------------------
//
// _InitKeyEventSink
//
// Advise a keystroke sink.
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_InitKeyEventSink()
{
    ITfKeystrokeMgr *pKeystrokeMgr = nullptr;
    HRESULT hr = S_OK;

    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void **)&pKeystrokeMgr)))
    {
        return FALSE;
    }

    hr = pKeystrokeMgr->AdviseKeyEventSink(_tfClientId, (ITfKeyEventSink *)this, TRUE);

    pKeystrokeMgr->Release();

    return (hr == S_OK);
}

//+---------------------------------------------------------------------------
//
// _UninitKeyEventSink
//
// Unadvise a keystroke sink.  Assumes we have advised one already.
//----------------------------------------------------------------------------

void CMetasequoiaIME::_UninitKeyEventSink()
{
    ITfKeystrokeMgr *pKeystrokeMgr = nullptr;

    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void **)&pKeystrokeMgr)))
    {
        return;
    }

    pKeystrokeMgr->UnadviseKeyEventSink(_tfClientId);

    pKeystrokeMgr->Release();
}
