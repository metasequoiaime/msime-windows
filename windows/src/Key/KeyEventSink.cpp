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
#include "VimMode.h"

using namespace key_event_sink_detail;

namespace
{
void ApplyVimModeEscape(CCompositionProcessorEngine *engine, ITfThreadMgr *threadMgr, TfClientId clientId, WPARAM key,
                        LPARAM keyFlags, bool eaten, bool compositionActive, bool deferredKeysPending)
{
    const bool winDown = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
    if (!VimMode::ShouldSwitchToEnglish(static_cast<UINT>(key), eaten, IsAutoRepeat(keyFlags), CaptureIpcModifiers(),
                                        winDown, compositionActive, deferredKeysPending) ||
        !engine || !engine->GetIMEMode(threadMgr, clientId) || !FanyUtils::ReadConfiguredVimMode())
    {
        return;
    }
    // This runs even when TestKeyDown returns FALSE: TSF then omits OnKeyDown.
    engine->SetIMEMode(threadMgr, clientId, FALSE);
    engine->SetPunctuationMode(threadMgr, clientId, FALSE);
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

    if (wParam == VK_ESCAPE && !_IsKeyboardDisabled())
    {
        ApplyVimModeEscape(_pCompositionProcessorEngine, _pThreadMgr, _tfClientId, wParam, lParam, *pIsEaten != FALSE,
                           _IsCompositionActiveForKeyGuard() || _candidateMode != CANDIDATE_NONE,
                           _HasDeferredKeyBarrier());
    }

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
    // Some hosts offer the actual callback without a preceding test.
    if (wParam == VK_ESCAPE && !_IsKeyboardDisabled())
    {
        ApplyVimModeEscape(_pCompositionProcessorEngine, _pThreadMgr, _tfClientId, wParam, lParam, *pIsEaten != FALSE,
                           _IsCompositionActiveForKeyGuard() || _candidateMode != CANDIDATE_NONE,
                           _HasDeferredKeyBarrier());
    }
    DebugTsfKeyLatency(L"on-key-down", 0, onKeyDownTimer.ElapsedMs(), S_OK);
    return S_OK;
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
