// Input-mode hotkeys: the mintty bare-Shift keyboard hook, bare-modifier arming, chord and
// modifier-release matching, and queueing a matched hotkey as a deferred preserved key.

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
constexpr auto kModifierHotkeyToggleLimit = std::chrono::milliseconds(500);

bool IsControlVk(UINT code)
{
    return code == VK_CONTROL || code == VK_LCONTROL || code == VK_RCONTROL;
}

bool IsAltVk(UINT code)
{
    return code == VK_MENU || code == VK_LMENU || code == VK_RMENU;
}

bool IsWinVk(UINT code)
{
    return code == VK_LWIN || code == VK_RWIN;
}

bool IsOtherKeyboardKeyDown()
{
    // Mouse buttons occupy the first virtual-key values.  Start at Backspace
    // so holding a mouse button does not turn a bare Shift into a chord.
    for (UINT code = VK_BACK; code <= 0xfe; ++code)
    {
        if (!IsShiftVk(code) && (GetAsyncKeyState(code) & 0x8000) != 0)
        {
            return true;
        }
    }
    return false;
}

// Global::IsShiftKeyDownOnly and friends are derived from GetKeyState(), which
// reports the modifier state as of the message the calling thread last pulled
// from its queue.  Hosts that call into ITfKeystrokeMgr outside that dispatch
// -- Word is the known offender -- can therefore hand us a Shift event whose
// GetKeyState() snapshot still describes the previous keystroke.  Bare-modifier
// arming must not depend on that; GetAsyncKeyState() reads the real keyboard.
bool IsOnlyModifierPhysicallyDown(UINT keptDownVk)
{
    static const UINT kModifiers[] = {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN};
    for (const UINT modifier : kModifiers)
    {
        if (modifier == keptDownVk)
        {
            continue;
        }
        if ((GetAsyncKeyState(modifier) & 0x8000) != 0)
        {
            return false;
        }
    }
    return (GetAsyncKeyState(keptDownVk) & 0x8000) != 0;
}

} // namespace

void CMetasequoiaIME::_InitBareShiftKeyboardHook()
{
    // Restricted to mintty on purpose.  mintty routes composition over the
    // legacy IMM bridge and never offers a bare modifier key-up to
    // ITfKeyEventSink at all, so there is no sink event to work with.  Hosts
    // that merely report a stale GetKeyState() alongside the release (Word) are
    // handled in the sink instead -- see _MatchModifierReleaseHotkey.  Neither
    // Weasel nor WindInput installs a keyboard hook for this, and windows/
    // AGENTS.md asks that hooks in the injected DLL stay the exception.
    if (_bareShiftHook != nullptr || _bareShiftHookOwner != nullptr ||
        CompareStringOrdinal(Global::current_process_name.c_str(), -1, L"mintty.exe", -1, TRUE) != CSTR_EQUAL)
    {
        return;
    }

    _bareShiftHookOwner = this;
    _bareShiftHook = SetWindowsHookExW(WH_KEYBOARD, _BareShiftKeyboardHookProc, nullptr, GetCurrentThreadId());
    if (_bareShiftHook == nullptr)
    {
        _bareShiftHookOwner = nullptr;
    }
}

void CMetasequoiaIME::_UninitBareShiftKeyboardHook()
{
    HHOOK hook = _bareShiftHook;
    _bareShiftHook = nullptr;
    if (_bareShiftHookOwner == this)
    {
        _bareShiftHookOwner = nullptr;
    }
    if (hook != nullptr)
    {
        UnhookWindowsHookEx(hook);
    }

    _bareShiftDownMask = 0;
    _bareShiftArmed = false;
    _bareShiftFocusGeneration = 0;
    _bareShiftExpireTick = 0;
}

LRESULT CALLBACK CMetasequoiaIME::_BareShiftKeyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    CMetasequoiaIME *owner = _bareShiftHookOwner;
    if (code == HC_ACTION && owner != nullptr &&
        !IsSelfGeneratedSendInputExtraInfo(static_cast<ULONG_PTR>(GetMessageExtraInfo())))
    {
        const UINT virtualKey = static_cast<UINT>(wParam);
        const bool isShift = IsShiftVk(virtualKey);
        const bool isKeyUp = (static_cast<ULONG_PTR>(lParam) & 0x80000000u) != 0;
        const bool wasDown = (static_cast<ULONG_PTR>(lParam) & 0x40000000u) != 0;

        if (isShift)
        {
            const UINT scanCode = (static_cast<UINT>(lParam) >> 16) & 0x00ffu;
            const BYTE shiftBit = scanCode == 0x36 ? 0x02 : 0x01;
            if (!isKeyUp)
            {
                if (!wasDown)
                {
                    if (owner->_bareShiftDownMask == 0)
                    {
                        owner->_bareShiftArmed = !IsOtherKeyboardKeyDown();
                        ++owner->_bareShiftSequence;
                        if (owner->_bareShiftSequence == 0)
                        {
                            ++owner->_bareShiftSequence;
                        }
                        owner->_bareShiftFocusGeneration = owner->_deferredKeyFocusGeneration;
                        owner->_bareShiftExpireTick =
                            GetTickCount64() + static_cast<ULONGLONG>(kModifierHotkeyToggleLimit.count());
                    }
                    owner->_bareShiftDownMask |= shiftBit;
                }
            }
            else
            {
                owner->_bareShiftDownMask &= static_cast<BYTE>(~shiftBit);
                if (owner->_bareShiftDownMask == 0)
                {
                    const bool shouldPost = owner->_bareShiftArmed && GetTickCount64() < owner->_bareShiftExpireTick;
                    owner->_bareShiftArmed = false;
                    if (shouldPost && owner->_msgWndHandle != nullptr)
                    {
                        PostMessage(owner->_msgWndHandle, WM_BareShiftRelease, owner->_bareShiftSequence, 0);
                    }
                }
            }
        }
        else if (!isKeyUp)
        {
            owner->_bareShiftArmed = false;
        }
    }

    return CallNextHookEx(owner ? owner->_bareShiftHook : nullptr, code, wParam, lParam);
}

void CMetasequoiaIME::_MarkBareShiftHandled()
{
    if (_bareShiftHook != nullptr)
    {
        _bareShiftHandledSequence = _bareShiftSequence;
    }
}

void CMetasequoiaIME::_HandleHookedBareShiftRelease(UINT sequence)
{
    if (_bareShiftHook == nullptr || sequence == 0 || sequence != _bareShiftSequence ||
        sequence == _bareShiftHandledSequence || _bareShiftFocusGeneration != _deferredKeyFocusGeneration ||
        !Global::g_connected || _pThreadMgr == nullptr || _pCompositionProcessorEngine == nullptr ||
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 || !FanyUtils::ReadConfiguredSwitchLanguageHotkeys().shift)
    {
        return;
    }

    BOOL hasThreadFocus = FALSE;
    if (FAILED(_pThreadMgr->IsThreadFocus(&hasThreadFocus)) || !hasThreadFocus)
    {
        return;
    }

    ITfDocumentMgr *documentMgr = nullptr;
    ITfContext *context = nullptr;
    if (FAILED(_pThreadMgr->GetFocus(&documentMgr)) || documentMgr == nullptr)
    {
        return;
    }
    const HRESULT getTopResult = documentMgr->GetTop(&context);
    documentMgr->Release();
    if (FAILED(getTopResult) || context == nullptr)
    {
        return;
    }

    BOOL eaten = FALSE;
    const bool queued = _QueueInputHotkey(context, Global::MetasequoiaIMEGuidImeModePreserveKey, &eaten);
    DebugTsfIssue47(L"bare-shift-hook-release", FANY_IME_NO_REQUEST_ID, VK_SHIFT, L'\0', 0, 0, queued ? 1 : 0,
                    _IsComposing(),
                    _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_OK);
    if (queued)
    {
        _bareShiftHandledSequence = sequence;
        _shiftHotkeyArmed = false;
        _ctrlHotkeyArmed = false;
        Global::IsShiftKeyDownOnly = FALSE;
        Global::PureShiftKeyDown = FALSE;
        Global::PureShiftKeyUp = FALSE;
        Global::ModifiersValue &= ~(TF_MOD_SHIFT | TF_MOD_LSHIFT | TF_MOD_RSHIFT);
    }
    context->Release();
}

void CMetasequoiaIME::_TrackModifierHotkeyArming(WPARAM wParam, LPARAM lParam, bool isKeyUp)
{
    if (isKeyUp)
    {
        return;
    }

    const UINT code = LOWORD(wParam);
    const bool isShift = IsShiftVk(code);
    const bool isCtrl = IsControlVk(code);
    const bool isOtherModifier = IsAltVk(code) || IsWinVk(code);

    if (!isShift && !isCtrl && !isOtherModifier)
    {
        _shiftHotkeyArmed = false;
        _ctrlHotkeyArmed = false;
        return;
    }

    // Ignore auto-repeat; only the first down can arm a bare-modifier toggle.
    if ((lParam & 0x40000000) != 0)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (isShift && IsOnlyModifierPhysicallyDown(VK_SHIFT))
    {
        _shiftHotkeyArmed = true;
        _ctrlHotkeyArmed = false;
        _modifierHotkeyExpire = now + kModifierHotkeyToggleLimit;
        return;
    }
    if (isCtrl && IsOnlyModifierPhysicallyDown(VK_CONTROL))
    {
        _ctrlHotkeyArmed = true;
        _shiftHotkeyArmed = false;
        _modifierHotkeyExpire = now + kModifierHotkeyToggleLimit;
        return;
    }

    _shiftHotkeyArmed = false;
    _ctrlHotkeyArmed = false;
}

bool CMetasequoiaIME::_MatchChordInputHotkey(WPARAM wParam, _Out_ GUID *hotkeyGuid) const
{
    if (hotkeyGuid == nullptr)
    {
        return false;
    }

    const UINT code = LOWORD(wParam);
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const auto hotkeys = FanyUtils::ReadConfiguredSwitchLanguageHotkeys();

    if (code == VK_SPACE && ctrl && alt && !shift && hotkeys.ctrl_alt_space)
    {
        *hotkeyGuid = Global::MetasequoiaIMEGuidImeModePreserveKey02;
        return true;
    }
    if (code == VK_SPACE && ctrl && shift && !alt)
    {
        *hotkeyGuid = Global::MetasequoiaIMEGuidDoubleSingleBytePreserveKey;
        return true;
    }
    if (code == VK_OEM_PERIOD && ctrl && !shift && !alt)
    {
        *hotkeyGuid = Global::MetasequoiaIMEGuidPunctuationPreserveKey;
        return true;
    }
    return false;
}

bool CMetasequoiaIME::_MatchModifierReleaseHotkey(WPARAM wParam, _Out_ GUID *hotkeyGuid)
{
    if (hotkeyGuid == nullptr)
    {
        return false;
    }

    const UINT code = LOWORD(wParam);
    const auto now = std::chrono::steady_clock::now();
    const auto hotkeys = FanyUtils::ReadConfiguredSwitchLanguageHotkeys();

    // _shiftHotkeyArmed already encodes "this Shift went down alone and nothing
    // else has been pressed since" -- _TrackModifierHotkeyArming disarms on any
    // other key-down.  Matching on the released virtual key plus that latch is
    // enough, and unlike Global::PureShiftKeyUp it does not require the host's
    // GetKeyState() snapshot to have caught up with the release.
    if (IsShiftVk(code) && _shiftHotkeyArmed)
    {
        const bool fire = now < _modifierHotkeyExpire && hotkeys.shift;
        _shiftHotkeyArmed = false;
        _ctrlHotkeyArmed = false;
        if (fire)
        {
            *hotkeyGuid = Global::MetasequoiaIMEGuidImeModePreserveKey;
            return true;
        }
        return false;
    }

    if (IsControlVk(code) && _ctrlHotkeyArmed)
    {
        const bool fire = now < _modifierHotkeyExpire && hotkeys.ctrl;
        _shiftHotkeyArmed = false;
        _ctrlHotkeyArmed = false;
        if (fire)
        {
            *hotkeyGuid = Global::MetasequoiaIMEGuidImeModePreserveKey03;
            return true;
        }
        return false;
    }

    return false;
}

bool CMetasequoiaIME::_QueueInputHotkey(_In_ ITfContext *pContext, REFGUID hotkeyGuid, _Out_ BOOL *pIsEaten)
{
    if (pIsEaten == nullptr)
    {
        return false;
    }
    *pIsEaten = FALSE;
    if (pContext == nullptr || !_DeferredKeyQueueHasCapacity())
    {
        return false;
    }

    if (_pCompositionProcessorEngine != nullptr &&
        _pCompositionProcessorEngine->GetPreservedKeyAction(hotkeyGuid) ==
            CCompositionProcessorEngine::PreservedKeyAction::ToggleImeMode &&
        !_serverUnavailableFallbackActive && IsTrilingualCycleEnabled())
    {
        // Keep the existing configured language-hotkey detection (bare Shift
        // by default), but replace its binary toggle with the Server-owned
        // cycle. As in OnPreservedKey, a matched language action is sent as
        // VK_SHIFT; raw modifier key-downs never enter this path.
        _KEYSTROKE_STATE cycleState = {};
        cycleState.Category = CATEGORY_COMPOSING;
        cycleState.Function = FUNCTION_CYCLE_INPUT_MODE;
        *pIsEaten = _QueueDeferredKeyDown(pContext, VK_SHIFT, 0, L'\0', 0, cycleState) ? TRUE : FALSE;
    }
    else
    {
        *pIsEaten = _QueueDeferredPreservedKey(pContext, hotkeyGuid) ? TRUE : FALSE;
    }
    if (*pIsEaten && _localSessionResetPending.load(std::memory_order_acquire))
    {
        const UINT resetToken = _localSessionResetToken.load(std::memory_order_acquire);
        _RequestLocalSessionReset(pContext, resetToken);
    }
    return *pIsEaten != FALSE;
}
