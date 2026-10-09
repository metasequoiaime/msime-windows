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
