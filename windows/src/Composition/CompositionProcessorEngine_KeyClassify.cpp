// Virtual-key classification: IsVirtualKeyNeed and IsVirtualKeyNeedForFreshComposition, with
// the keystroke, candidate and number-range tests they use.

#include "Private.h"
#include "MetasequoiaIME.h"
#include "CompositionProcessorEngine.h"
#include "TfInputProcessorProfile.h"
#include "Globals.h"
#include "FanyDefines.h"
#include "Compartment.h"
#include "LanguageBar.h"
#include "RegKey.h"
#include "define.h"
#include <msctf.h>
#include <string>
#include <fmt/xchar.h>
#include "Ipc.h"
#include "TemporaryJapaneseKeyPolicy.h"
#include "FanyUtils.h"
#include "FanyLog.h"
#include "EditSession.h"
#include "TfTextLayoutSink.h"
#include "../../../engine/contracts/date_time_input.h"
#include "../../../engine/contracts/direct_helpcode.h"
#include "../../../engine/contracts/mid_sentence_helpcode.h"
#include <new>

namespace
{
// 解析按键缓冲时哪种大写字母开段：句中辅助码或末尾辅助码（间接）的大写触发，Server 只会让其中一个生效。
FanyImeMidSentenceHelpcode::UppercaseTriggers HelpcodeUppercaseTriggers()
{
    return {Global::MidSentenceHelpcodeUppercaseEnabled.load(std::memory_order_relaxed),
            (Global::EndHelpcodeTriggers.load(std::memory_order_relaxed) & FanyImeEndHelpcode::kUppercase) != 0};
}

// 微软/搜狗/紫光双拼的 ';' 是 ing 韵母：光标前这一节（最后一个 ' 之后）是奇数键时它是编码键。
bool IsMicrosoftShuangpinIngKeyAt(UINT uCode, WCHAR wch, const WCHAR *buffer, DWORD_PTR length, DWORD_PTR caret)
{
    if (!Global::MicrosoftShuangpinEnabled.load(std::memory_order_relaxed) || uCode != VK_OEM_1 || wch != L';' ||
        buffer == nullptr || length == 0)
    {
        return false;
    }
    caret = min(caret, length);
    // 直接辅助码的辅码会打乱奇偶（uia 后面接 x; 时这一节是偶数键），改成只看前一个键是不是字母。
    if (Global::DirectHelpcodeEnabled.load(std::memory_order_relaxed))
    {
        return FanyImeDirectHelpcode::AcceptsSemicolonFinalAt(buffer, static_cast<std::size_t>(length),
                                                              static_cast<std::size_t>(caret));
    }
    // 大写触发开着、或缓冲里有末尾段时，段不算这一节的键；都没有时就是原来「最后一个 ' 之后数奇偶」的规则。
    return FanyImeMidSentenceHelpcode::AcceptsSemicolonFinalAt(
        buffer, static_cast<std::size_t>(length), static_cast<std::size_t>(caret), HelpcodeUppercaseTriggers());
}

// 双拼直接辅助码的 /：四码（两键音节 + 两位辅码）后面的终止键是编码键，否则仍按标点处理。Server
// 用同一条形状规则决定收不收（engine/contracts/direct_helpcode.h）。
bool IsDirectHelpcodeSlashKey(UINT uCode, WCHAR wch, const WCHAR *buffer, DWORD_PTR length, DWORD_PTR caret)
{
    return uCode == VK_OEM_2 && wch == L'/' && Global::DirectHelpcodeEnabled.load(std::memory_order_relaxed) &&
           Global::DirectHelpcodeSlashEnabled.load(std::memory_order_relaxed) && buffer != nullptr && length > 0 &&
           FanyImeDirectHelpcode::AcceptsSlashAt(buffer, static_cast<std::size_t>(length),
                                                 static_cast<std::size_t>(min(caret, length)));
}

// 双拼句中辅助码的触发键（反引号，或设置里勾了的分号）：开关开着，且光标前是一节完整的两键音节时
// 是编码键，否则仍按标点处理。分号先让给 ing 韵母。光标可以在句中（用箭头移回去补辅助码），只看
// 光标前的部分。Server 用同一条形状规则决定收不收（engine/contracts/mid_sentence_helpcode.h）。
bool IsMidSentenceHelpcodeMarkerKey(UINT uCode, WCHAR wch, const WCHAR *buffer, DWORD_PTR length, DWORD_PTR caret)
{
    const bool backtick =
        uCode == VK_OEM_3 && wch == L'`' && Global::MidSentenceHelpcodeEnabled.load(std::memory_order_relaxed);
    const bool semicolon = uCode == VK_OEM_1 && wch == L';' &&
                           Global::MidSentenceHelpcodeSemicolonEnabled.load(std::memory_order_relaxed) &&
                           !IsMicrosoftShuangpinIngKeyAt(uCode, wch, buffer, length, caret);
    return (backtick || semicolon) && buffer != nullptr && length > 0 &&
           FanyImeMidSentenceHelpcode::AcceptsMarkerAt(buffer, static_cast<std::size_t>(length),
                                                       static_cast<std::size_t>(min(caret, length)),
                                                       HelpcodeUppercaseTriggers());
}

// 双拼末尾辅助码（间接）的触发键（反引号或分号，Server 按设置和与句中辅助码的互斥发来的那几个）：光标在
// 缓冲末尾、前面是一节完整的两键音节时是编码键，缓冲里记成 kEndMarker，否则仍按标点处理。分号先让给 ing
// 韵母。Server 用同一条形状规则决定收不收（engine/contracts/mid_sentence_helpcode.h）。
bool IsEndHelpcodeMarkerKey(UINT uCode, WCHAR wch, const WCHAR *buffer, DWORD_PTR length, DWORD_PTR caret)
{
    const unsigned triggers = Global::EndHelpcodeTriggers.load(std::memory_order_relaxed);
    const bool backtick = uCode == VK_OEM_3 && wch == L'`' && (triggers & FanyImeEndHelpcode::kBacktick) != 0;
    const bool semicolon = uCode == VK_OEM_1 && wch == L';' && (triggers & FanyImeEndHelpcode::kSemicolon) != 0 &&
                           !IsMicrosoftShuangpinIngKeyAt(uCode, wch, buffer, length, caret);
    return (backtick || semicolon) && buffer != nullptr && length > 0 &&
           FanyImeMidSentenceHelpcode::AcceptsEndMarkerAt(buffer, static_cast<std::size_t>(length),
                                                          static_cast<std::size_t>(min(caret, length)),
                                                          HelpcodeUppercaseTriggers());
}

// 日语模式禁用 -/= 翻页：'-' 是长音符（ー）的输入键。空编码时也要起头组合，
// 候选框第一项是长音符 ー、第二项是普通连字符 '-'（候选由服务端提供）。
bool IsJapaneseKeyMode(const WCHAR *raw, DWORD_PTR length)
{
    return FanyImeTemporaryJapanese::IsMode(Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed),
                                            GetTemporaryJapanesePolicy(), raw, static_cast<std::size_t>(length));
}

// 直接辅助码下用引导键进入末尾辅助码（UsesDirectHelpcodeEndTrigger）：输入末尾形状合适的引导键（Server 发来的
// 反引号、分号），以及删掉它留下的末尾段开头的退格（Ctrl/Alt 组合不在这里，Ctrl+Backspace 仍按单元删）。两者
// 都由 Server 改写输入串并回 CompositionRestored；改写不了时 Server 照旧把引导键当标点回帧。发送时打上
// FanyImePipeFlags::DirectHelpcodeEndTriggerRewrite，Server 以这个标记为准（见 KeyEventSink.cpp）。分号先让给
// ing 韵母，日语模式不收。
bool IsDirectHelpcodeEndTriggerRewriteKeyAt(UINT uCode, WCHAR wch, const WCHAR *buffer, DWORD_PTR length,
                                            DWORD_PTR caret)
{
    if (buffer == nullptr || length == 0 || !UsesDirectHelpcodeEndTrigger() || IsJapaneseKeyMode(buffer, length))
    {
        return false;
    }
    const std::size_t size = static_cast<std::size_t>(length);
    const std::size_t at = static_cast<std::size_t>(min(caret, length));
    if (uCode == VK_BACK)
    {
        return FanyImeDirectHelpcode::IsEndTriggerBackspaceAt(buffer, size, at);
    }
    const unsigned keys = Global::DirectHelpcodeEndTriggerKeys.load(std::memory_order_relaxed);
    const bool backtick = uCode == VK_OEM_3 && wch == L'`' && (keys & FanyImeEndHelpcode::kBacktick) != 0;
    const bool semicolon = uCode == VK_OEM_1 && wch == L';' && (keys & FanyImeEndHelpcode::kSemicolon) != 0 &&
                           !IsMicrosoftShuangpinIngKeyAt(uCode, wch, buffer, length, caret);
    return (backtick || semicolon) && FanyImeDirectHelpcode::AcceptsEndTriggerAt(buffer, size, at);
}

bool IsJapaneseLongVowelKey(UINT uCode, WCHAR wch, const WCHAR *raw, DWORD_PTR length)
{
    return ClassifyJapaneseKey(uCode, wch, 0, Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed),
                               GetTemporaryJapanesePolicy(), raw, static_cast<std::size_t>(length), true)
               .Function == FUNCTION_INPUT;
}

// '_' '=' '+' 在日语模式下也不翻页，但它们没有假名写法，按标点上屏处理，
// 且只在已有编码时接管——空编码时仍然是普通标点，归应用程序。
bool IsJapaneseMinusEqualPunctuationKey(UINT uCode, WCHAR wch, BOOL fComposing, CANDIDATE_MODE candidateMode,
                                        DWORD_PTR keystrokeLength, const WCHAR *raw)
{
    return ClassifyJapaneseKey(uCode, wch, 0, Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed),
                               GetTemporaryJapanesePolicy(), raw, static_cast<std::size_t>(keystrokeLength),
                               fComposing || candidateMode != CANDIDATE_NONE)
               .Function == FUNCTION_PUNCTUATION;
}

bool IsCommitWithHighlightedCandidatePunctuationInCandidateMode(UINT uCode, WCHAR wch, CANDIDATE_MODE candidateMode,
                                                                const WCHAR *raw, DWORD_PTR keystrokeLength)
{
    if (candidateMode == CANDIDATE_NONE)
    {
        return false;
    }

    // Candidate paging keys must keep their navigation semantics even if the
    // corresponding character is also listed in CommitWithHighlightedCandPunc.
    switch (uCode)
    {
    case VK_PRIOR:
    case VK_NEXT:
    case VK_HOME:
    case VK_END:
    case VK_TAB:
        return false;
    case VK_OEM_MINUS:
    case VK_OEM_PLUS:
        // 日语模式下这两个键不翻页，交给下面的标点判定处理；但 '-' 走长音符输入，
        // 不是标点上屏。
        if (!IsJapaneseKeyMode(raw, keystrokeLength) || IsJapaneseLongVowelKey(uCode, wch, raw, keystrokeLength))
        {
            return false;
        }
        break;
    default:
        break;
    }

    return wch != 0 && Global::CommitWithHighlightedCandPunc.count(wch) > 0;
}

bool IsManualPinyinSeparatorInComposition(WCHAR wch, BOOL fComposing, CANDIDATE_MODE candidateMode,
                                          DWORD_PTR keystrokeLength)
{
    if (wch != L'\'')
    {
        return false;
    }
    if (keystrokeLength == 0)
    {
        return false;
    }
    return fComposing || candidateMode != CANDIDATE_NONE;
}
} // namespace

//////////////////////////////////////////////////////////////////////
//
//    CCompositionProcessorEngine
//
//////////////////////////////////////////////////////////////////////

BOOL CCompositionProcessorEngine::IsVirtualKeyNeedForFreshComposition(UINT uCode, _In_reads_(1) WCHAR *pwch,
                                                                      _Out_opt_ _KEYSTROKE_STATE *pKeyState)
{
    if (pKeyState)
    {
        pKeyState->Category = CATEGORY_NONE;
        pKeyState->Function = FUNCTION_NONE;
    }

    // Classify against an actually empty composition. This path is used while
    // the old focus session is still being cancelled, so none of its candidate
    // mode, wildcard flags, or virtual-key buffer may affect the first key in
    // the replacement session.
    if (IsManualPinyinSeparatorInComposition(pwch ? *pwch : 0, FALSE, CANDIDATE_NONE, 0))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }
    // 日语模式下 '-' 单独按也要起头组合，弹出候选框选长音符 ー 或普通 '-'。
    if (IsJapaneseLongVowelKey(uCode, pwch ? *pwch : 0, nullptr, 0))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }
    if (IsVirtualKeyKeystrokeComposition(uCode, pKeyState, FUNCTION_INPUT))
    {
        return TRUE;
    }
    if (pwch && IsWildcard() && IsWildcardChar(*pwch) && !IsDisableWildcardAtFirst())
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }
    return FALSE;
}

//+---------------------------------------------------------------------------
//
// CCompositionProcessorEngine::IsVirtualKeyNeed
//
// Test virtual key code need to the Composition Processor Engine.
// param
//     [in] uCode - Specify virtual key code.
//     [in/out] pwch       - char code
//     [in] fComposing     - Specified composing.
//     [in] fCandidateMode - Specified candidate mode.
//     [out] pKeyState     - Returns function regarding virtual key.
// returns
//     If engine need this virtual key code, returns true. Otherwise returns false.
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::IsVirtualKeyNeed( //
    UINT uCode,                                     //
    _In_reads_(1) WCHAR *pwch,                      //
    BOOL fComposing,                                //
    CANDIDATE_MODE candidateMode,                   //
    BOOL hasCandidateWithWildcard,                  //
    _Out_opt_ _KEYSTROKE_STATE *pKeyState           //
)
{
    if (pKeyState)
    {
        pKeyState->Category = CATEGORY_NONE;
        pKeyState->Function = FUNCTION_NONE;
    }

    if (candidateMode == CANDIDATE_ORIGINAL)
    {
        fComposing = FALSE;
    }

    if (IsManualPinyinSeparatorInComposition(pwch ? *pwch : 0, fComposing, candidateMode, _keystrokeBuffer.GetLength()))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }

    if (IsJapaneseLongVowelKey(uCode, pwch ? *pwch : 0, _keystrokeBuffer.Get(), _keystrokeBuffer.GetLength()))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }

    // T-mode: digits, '/' and ':' that still extend a date/time are input. The rest keep their usual
    // meaning below (digits select, '/' and ':' are punctuation); Shift+digit never matches here.
    // V-mode: digits and . + - * / ( ) after the V prefix are input, so they neither select, page nor commit
    // punctuation. Shift+8/9/0 give * ( ) and are input too; other Shift+digits still select.
    if (IsDateTimeInputKey(uCode, pwch ? *pwch : 0, _keystrokeBuffer.Get(), _keystrokeBuffer.GetLength(),
                           _caretPosition) ||
        IsVModeInputKey(pwch ? *pwch : 0, _keystrokeBuffer.Get(), _keystrokeBuffer.GetLength(), _caretPosition))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }
    // V-mode: bare digits are input, so Shift+1..9 selects. It must be decided here,
    // before the punctuation rule below would commit the highlighted candidate together with '!' '@' ...
    // Shift+8/9 produce '*' '(' and were already taken as input above.
    if (IsVModeShiftDigitSelectionKey(uCode, _keystrokeBuffer.Get(), _keystrokeBuffer.GetLength()))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_CANDIDATE;
            pKeyState->Function = FUNCTION_SELECT_BY_NUMBER;
        }
        return TRUE;
    }

    if (IsJapaneseMinusEqualPunctuationKey(uCode, pwch ? *pwch : 0, fComposing, candidateMode,
                                           _keystrokeBuffer.GetLength(), _keystrokeBuffer.Get()))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_PUNCTUATION;
        }
        return TRUE;
    }

    // The Server owns the configurable comma/period behavior. Always route
    // these keys through it while candidates are active; its response decides
    // whether the key navigates or commits the highlighted candidate with punctuation.
    const bool isCommaPeriodPagingKey = uCode == VK_OEM_COMMA || uCode == VK_OEM_PERIOD;
    const bool isBracketPagingKey = uCode == VK_OEM_4 || uCode == VK_OEM_6;
    const bool isMinusEqualPagingKey = (uCode == VK_OEM_MINUS || uCode == VK_OEM_PLUS) &&
                                       !IsJapaneseKeyMode(_keystrokeBuffer.Get(), _keystrokeBuffer.GetLength());
    if (candidateMode != CANDIDATE_NONE &&
        (isMinusEqualPagingKey || isCommaPeriodPagingKey || isBracketPagingKey || uCode == VK_TAB ||
         uCode == VK_PRIOR || uCode == VK_NEXT || uCode == VK_UP || uCode == VK_DOWN))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_CANDIDATE;
            pKeyState->Function = FUNCTION_SERVER_CANDIDATE_KEY;
        }
        return TRUE;
    }

    if (candidateMode != CANDIDATE_NONE && (uCode == VK_LEFT || uCode == VK_RIGHT))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            // 横排候选左右键选词开着时，移高亮还是移光标由 Server 定，TSF 只应用它的回复。
            if (UsesHorizontalArrowSelection())
            {
                pKeyState->Function = uCode == VK_LEFT ? FUNCTION_MOVE_LEFT_CANDIDATE : FUNCTION_MOVE_RIGHT_CANDIDATE;
            }
            else
            {
                pKeyState->Function = uCode == VK_LEFT ? FUNCTION_MOVE_LEFT : FUNCTION_MOVE_RIGHT;
            }
        }
        return TRUE;
    }

    if (IsMicrosoftShuangpinIngKeyAt(uCode, pwch ? *pwch : 0, _keystrokeBuffer.Get(), _keystrokeBuffer.GetLength(),
                                     _caretPosition))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }

    if (IsDirectHelpcodeEndTriggerRewriteKeyAt(uCode, pwch ? *pwch : 0, _keystrokeBuffer.Get(),
                                               _keystrokeBuffer.GetLength(), _caretPosition))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_SERVER_COMPOSITION_REWRITE;
        }
        return TRUE;
    }

    if (IsMidSentenceHelpcodeMarkerKey(uCode, pwch ? *pwch : 0, _keystrokeBuffer.Get(), _keystrokeBuffer.GetLength(),
                                       _caretPosition) ||
        IsEndHelpcodeMarkerKey(uCode, pwch ? *pwch : 0, _keystrokeBuffer.Get(), _keystrokeBuffer.GetLength(),
                               _caretPosition) ||
        IsDirectHelpcodeSlashKey(uCode, pwch ? *pwch : 0, _keystrokeBuffer.Get(), _keystrokeBuffer.GetLength(),
                                 _caretPosition))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }

    // 候选翻译展开键（裸 Backquote）：在辅助码判定之后、标点提交之前，保证辅助码优先。
    // 只在候选窗激活、有输入、非日语模式且翻译开关开着时吃键。
    if (candidateMode != CANDIDATE_NONE && _keystrokeBuffer.GetLength() > 0 &&
        !IsJapaneseKeyMode(_keystrokeBuffer.Get(), _keystrokeBuffer.GetLength()) &&
        FanyUtils::IsCandidateExpansionBareKey(uCode, pwch ? *pwch : 0))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_CANDIDATE;
            pKeyState->Function = FUNCTION_SERVER_CANDIDATE_KEY;
        }
        return TRUE;
    }

    if (IsCommitWithHighlightedCandidatePunctuationInCandidateMode(
            uCode, pwch ? *pwch : 0, candidateMode, _keystrokeBuffer.Get(), _keystrokeBuffer.GetLength()))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_PUNCTUATION;
        }
        return TRUE;
    }

    if (fComposing || candidateMode == CANDIDATE_INCREMENTAL || candidateMode == CANDIDATE_NONE)
    {
        if (IsVirtualKeyKeystrokeComposition(uCode, pKeyState, FUNCTION_NONE)) // 26 basic English chars
        {
            return TRUE;
        }
        else if ((IsWildcard() && IsWildcardChar(*pwch) && !IsDisableWildcardAtFirst()) ||
                 (IsWildcard() && IsWildcardChar(*pwch) && IsDisableWildcardAtFirst() && _keystrokeBuffer.GetLength()))
        {
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_COMPOSING;
                pKeyState->Function = FUNCTION_INPUT;
            }
            return TRUE;
        }
        else if (_hasWildcardIncludedInKeystrokeBuffer && uCode == VK_SPACE)
        {
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_COMPOSING;
                pKeyState->Function = FUNCTION_CONVERT_WILDCARD;
            }
            return TRUE;
        }
        if (Global::PureShiftKeyUp)
        {
            return TRUE;
        }
    }

    if (candidateMode == CANDIDATE_ORIGINAL)
    {
        BOOL isRetCode = TRUE;
        if (IsVirtualKeyKeystrokeCandidate(uCode, pKeyState, candidateMode, &isRetCode, &_KeystrokeCandidate))
        {
            return isRetCode;
        }

        if (hasCandidateWithWildcard)
        {
            if (IsVirtualKeyKeystrokeCandidate(uCode, pKeyState, candidateMode, &isRetCode,
                                               &_KeystrokeCandidateWildcard))
            {
                return isRetCode;
            }
        }

        // Candidate list could not handle key. We can try to restart the composition.
        if (IsVirtualKeyKeystrokeComposition(uCode, pKeyState, FUNCTION_INPUT))
        {
            if (candidateMode == CANDIDATE_ORIGINAL)
            {
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_FINALIZE_CANDIDATELIST_AND_INPUT;
                }
                return TRUE;
            }
        }
    }

    // CANDIDATE_INCREMENTAL should process Keystroke.Candidate virtual keys.
    else if (candidateMode == CANDIDATE_INCREMENTAL)
    {
        BOOL isRetCode = TRUE;
        if (IsVirtualKeyKeystrokeCandidate(uCode, pKeyState, candidateMode, &isRetCode, &_KeystrokeCandidate))
        {
            return isRetCode;
        }
    }

    if (!fComposing && candidateMode != CANDIDATE_ORIGINAL)
    {
        if (IsVirtualKeyKeystrokeComposition(uCode, pKeyState, FUNCTION_INPUT))
        {
            return TRUE;
        }
    }

    // System pre-defined keystroke
    if (fComposing)
    {
        if ((candidateMode != CANDIDATE_INCREMENTAL))
        {
            switch (uCode)
            {
            case VK_LEFT:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_LEFT;
                }
                return TRUE;
            case VK_RIGHT:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_RIGHT;
                }
                return TRUE;
            case VK_RETURN:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_FINALIZE_CANDIDATELIST;
                }
                return TRUE;
            case VK_ESCAPE:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_CANCEL;
                }
                return TRUE;
            case VK_BACK:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_BACKSPACE;
                }
                return TRUE;
            case VK_DELETE:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_DELETE;
                }
                return TRUE;

            case VK_UP:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_UP;
                }
                return TRUE;
            case VK_DOWN:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_DOWN;
                }
                return TRUE;
            case VK_PRIOR:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                }
                return TRUE;
            case VK_OEM_MINUS:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                }
                return TRUE;
            case VK_NEXT:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                }
                return TRUE;
            case VK_OEM_PLUS:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                }
                return TRUE;
            case VK_TAB:
                if (pKeyState)
                {
                    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
                    {
                        pKeyState->Category = CATEGORY_COMPOSING;
                        pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                    }
                    else
                    {
                        pKeyState->Category = CATEGORY_COMPOSING;
                        pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                    }
                }
                return TRUE;

            case VK_HOME:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_TOP;
                }
                return TRUE;
            case VK_END:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_BOTTOM;
                }
                return TRUE;

            case VK_SPACE:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_CONVERT;
                }
                return TRUE;
            }
        }
        else if (candidateMode == CANDIDATE_INCREMENTAL)
        {
            switch (uCode)
            {
                // VK_LEFT, VK_RIGHT - set *pIsEaten = FALSE for application could move caret left or right.
                // and for CUAS, invoke _HandleCompositionCancel() edit session due to ignore CUAS default key handler
                // for send out terminate composition
            case VK_LEFT:
            case VK_RIGHT: {
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_INVOKE_COMPOSITION_EDIT_SESSION;
                    pKeyState->Function = FUNCTION_CANCEL;
                }
            }
                return FALSE;

            case VK_RETURN:
                // Do something when user press return key
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_FINALIZE_CANDIDATELISTForVKReturn;
                }
                return TRUE;
            case VK_ESCAPE:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_CANCEL;
                }
                return TRUE;

                // VK_BACK - remove one char from reading string.
            case VK_BACK:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_BACKSPACE;
                }
                return TRUE;
            case VK_DELETE:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_DELETE;
                }
                return TRUE;

            case VK_UP:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_UP;
                }
                return TRUE;
            case VK_DOWN:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_DOWN;
                }
                return TRUE;
            case VK_PRIOR:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                }
                return TRUE;
            case VK_OEM_MINUS:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                }
                return TRUE;
            case VK_NEXT:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                }
                return TRUE;
            case VK_OEM_PLUS:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                }
                return TRUE;
            case VK_TAB:
                if (pKeyState)
                {
                    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
                    {
                        pKeyState->Category = CATEGORY_COMPOSING;
                        pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                    }
                    else
                    {
                        pKeyState->Category = CATEGORY_COMPOSING;
                        pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                    }
                }
                return TRUE;
            case VK_HOME:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_TOP;
                }
                return TRUE;
            case VK_END:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_BOTTOM;
                }
                return TRUE;

            case VK_SPACE: {
                if (candidateMode == CANDIDATE_INCREMENTAL)
                {
                    if (pKeyState)
                    {
                        pKeyState->Category = CATEGORY_CANDIDATE;
                        pKeyState->Function = FUNCTION_CONVERT;
                    }
                    return TRUE;
                }
                else
                {
                    if (pKeyState)
                    {
                        pKeyState->Category = CATEGORY_COMPOSING;
                        pKeyState->Function = FUNCTION_CONVERT;
                    }
                    return TRUE;
                }
            }
            }
        }
    }

    if (candidateMode == CANDIDATE_ORIGINAL)
    {
        switch (uCode)
        {
        case VK_UP:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_UP;
            }
            return TRUE;
        case VK_DOWN:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_DOWN;
            }
            return TRUE;
        case VK_PRIOR:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
            }
            return TRUE;
        case VK_OEM_MINUS:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
            }
            return TRUE;
        case VK_NEXT:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
            }
            return TRUE;
        case VK_OEM_PLUS:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
            }
            return TRUE;
        case VK_TAB:
            if (pKeyState)
            {
                if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                }
                else
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                }
            }
            return TRUE;
        case VK_HOME:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_PAGE_TOP;
            }
            return TRUE;
        case VK_END:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_PAGE_BOTTOM;
            }
            return TRUE;
        case VK_RETURN:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_FINALIZE_CANDIDATELIST;
            }
            return TRUE;
        case VK_SPACE:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_CONVERT;
            }
            return TRUE;
        case VK_BACK:
        case VK_DELETE:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_CANCEL;
            }
            return TRUE;

        case VK_ESCAPE:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_CANCEL;
            }
            return TRUE;
        }
    }

    //
    // Check whether the keystroke is number(for selecting candidate) and is in the range
    //
    if (IsKeystrokeRange(uCode, pKeyState, candidateMode))
    {
        return TRUE;
    }
    else if (pKeyState && pKeyState->Category != CATEGORY_NONE)
    {
        return FALSE;
    }

    if (*pwch && !IsVirtualKeyKeystrokeComposition(uCode, pKeyState, FUNCTION_NONE))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_INVOKE_COMPOSITION_EDIT_SESSION;
            pKeyState->Function = FUNCTION_FINALIZE_TEXTSTORE;
        }
        return FALSE;
    }

    return FALSE;
}

bool CCompositionProcessorEngine::IsMidSentenceHelpcodeTriggerKey(UINT uCode, WCHAR wch, const WCHAR *buffer,
                                                                  DWORD_PTR length, DWORD_PTR caret)
{
    return IsMidSentenceHelpcodeMarkerKey(uCode, wch, buffer, length, caret) ||
           IsEndHelpcodeMarkerKey(uCode, wch, buffer, length, caret);
}

bool CCompositionProcessorEngine::IsDirectHelpcodeInputKey(UINT uCode, WCHAR wch, const WCHAR *buffer, DWORD_PTR length,
                                                           DWORD_PTR caret)
{
    return IsDirectHelpcodeSlashKey(uCode, wch, buffer, length, caret) ||
           (Global::DirectHelpcodeEnabled.load(std::memory_order_relaxed) &&
            IsMicrosoftShuangpinIngKeyAt(uCode, wch, buffer, length, caret));
}

// Server 按同一条形状规则改输入串（engine/contracts/date_time_input.h）。看不到 T 模式开没开，只认开头的 T，
// Server 也一样不看，两边才不会分叉。小键盘数字也算：Server 收到时已经把它归一成主键盘数字。
bool CCompositionProcessorEngine::IsDateTimeInputKey(UINT uCode, WCHAR wch, const WCHAR *buffer, DWORD_PTR length,
                                                     DWORD_PTR caret)
{
    const bool digit_key = (uCode >= L'0' && uCode <= L'9') || (uCode >= VK_NUMPAD0 && uCode <= VK_NUMPAD9);
    const bool digit = digit_key && wch >= L'0' && wch <= L'9';
    const bool slash = uCode == VK_OEM_2 && wch == L'/';
    const bool colon = uCode == VK_OEM_1 && wch == L':';
    return (digit || slash || colon) && buffer != nullptr &&
           FanyImeDateTimeInput::AcceptsAt(buffer, static_cast<std::size_t>(length),
                                           static_cast<std::size_t>(min(caret, length)), wch);
}

// 只看字符不看键位：主键盘和小键盘的数字、运算符一样收，Server 也只看字符。前缀认哪个由 Server 的
// VModeChanged 决定（双拼和五笔只认 V，全拼 V、v 都认）。
bool CCompositionProcessorEngine::IsVModeShiftDigitSelectionKey(UINT uCode, const WCHAR *buffer, DWORD_PTR length)
{
    const bool shift_only = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 &&
                            (GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0 && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0;
    return shift_only && uCode >= L'1' && uCode <= L'9' &&
           FanyImeVModeInput::IsComposition(buffer, static_cast<std::size_t>(length),
                                            Global::VModeTrigger.load(std::memory_order_relaxed));
}

bool CCompositionProcessorEngine::IsVModeInputKey(WCHAR wch, const WCHAR *buffer, DWORD_PTR length, DWORD_PTR caret)
{
    return buffer != nullptr && FanyImeVModeInput::AcceptsAt(buffer, static_cast<std::size_t>(length),
                                                             static_cast<std::size_t>(min(caret, length)), wch,
                                                             Global::VModeTrigger.load(std::memory_order_relaxed));
}

bool CCompositionProcessorEngine::IsDirectHelpcodeEndTriggerRewriteKey(UINT uCode, WCHAR wch, const WCHAR *buffer,
                                                                       DWORD_PTR length, DWORD_PTR caret)
{
    return IsDirectHelpcodeEndTriggerRewriteKeyAt(uCode, wch, buffer, length, caret);
}

// 分号触发的句中辅助码段在按键缓冲里记成反引号，末尾辅助码（间接）的触发键记成 kEndMarker，与 Server 的
// raw 一致。在加入缓冲之前、按与吃键预判相同的状态判断。
WCHAR CCompositionProcessorEngine::NormalizeMidSentenceHelpcodeTrigger(WCHAR wch, const WCHAR *buffer, DWORD_PTR length,
                                                                       DWORD_PTR caret)
{
    const UINT uCode = wch == L'`' ? VK_OEM_3 : wch == L';' ? VK_OEM_1 : 0;
    if (uCode == 0)
    {
        return wch;
    }
    if (IsMidSentenceHelpcodeMarkerKey(uCode, wch, buffer, length, caret))
    {
        return static_cast<WCHAR>(FanyImeMidSentenceHelpcode::kMarker);
    }
    if (IsEndHelpcodeMarkerKey(uCode, wch, buffer, length, caret))
    {
        return static_cast<WCHAR>(FanyImeMidSentenceHelpcode::kEndMarker);
    }
    return wch;
}

//+---------------------------------------------------------------------------
//
// CCompositionProcessorEngine::IsVirtualKeyKeystrokeComposition
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::IsVirtualKeyKeystrokeComposition( //
    UINT uCode,                                                     //
    _Out_opt_ _KEYSTROKE_STATE *pKeyState,                          //
    KEYSTROKE_FUNCTION function                                     //
)
{
    if (pKeyState == nullptr)
    {
        return FALSE;
    }

    pKeyState->Category = CATEGORY_NONE;
    pKeyState->Function = FUNCTION_NONE;

    // 26 basic English characters
    for (UINT i = 0; i < _KeystrokeComposition.Count(); i++)
    {
        _KEYSTROKE *pKeystroke = nullptr;

        pKeystroke = _KeystrokeComposition.GetAt(i);

        if ((pKeystroke->VirtualKey == uCode) &&
            (Global::ModifiersValue == 36 || Global::ModifiersValue == 260 || Global::ModifiersValue == 292 ||
             Global::CheckModifiers(Global::ModifiersValue, pKeystroke->Modifiers)))
        {
            if (function == FUNCTION_NONE)
            {
                pKeyState->Category = CATEGORY_COMPOSING;
                pKeyState->Function = pKeystroke->Function;
                return TRUE;
            }
            else if (function == pKeystroke->Function)
            {
                pKeyState->Category = CATEGORY_COMPOSING;
                pKeyState->Function = pKeystroke->Function;
                return TRUE;
            }
        }
    }

    return FALSE;
}

//+---------------------------------------------------------------------------
//
// CCompositionProcessorEngine::IsVirtualKeyKeystrokeCandidate
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::IsVirtualKeyKeystrokeCandidate(
    UINT uCode, _In_ _KEYSTROKE_STATE *pKeyState, CANDIDATE_MODE /*candidateMode*/, _Out_ BOOL *pfRetCode,
    _In_ CMetasequoiaImeArray<_KEYSTROKE> *pKeystrokeMetric)
{
    if (pfRetCode == nullptr)
    {
        return FALSE;
    }
    *pfRetCode = FALSE;

    for (UINT i = 0; i < pKeystrokeMetric->Count(); i++)
    {
        _KEYSTROKE *pKeystroke = nullptr;

        pKeystroke = pKeystrokeMetric->GetAt(i);

        if ((pKeystroke->VirtualKey == uCode) && Global::CheckModifiers(Global::ModifiersValue, pKeystroke->Modifiers))
        {
            *pfRetCode = TRUE;
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;

                pKeyState->Function = pKeystroke->Function;
            }
            return TRUE;
        }
    }

    return FALSE;
}

//+---------------------------------------------------------------------------
//
// CCompositionProcessorEngine::IsKeyKeystrokeRange
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::IsKeystrokeRange(UINT uCode, _Out_ _KEYSTROKE_STATE *pKeyState,
                                                   CANDIDATE_MODE candidateMode)
{
    if (pKeyState == nullptr)
    {
        return FALSE;
    }

    pKeyState->Category = CATEGORY_NONE;
    pKeyState->Function = FUNCTION_NONE;

    if (_candidateListIndexRange.IsRange(uCode))
    {
        if (candidateMode != CANDIDATE_NONE)
        {
            pKeyState->Category = CATEGORY_CANDIDATE;
            pKeyState->Function = FUNCTION_SELECT_BY_NUMBER;
            return TRUE;
        }
        else if (GetVirtualKeyLength() > 0)
        {
            pKeyState->Category = CATEGORY_CANDIDATE;
            pKeyState->Function = FUNCTION_SELECT_BY_NUMBER;
            return TRUE;
        }
    }
    return FALSE;
}
