#pragma once

#include "MetasequoiaIME.h"
#include "sal.h"
#include "KeyHandlerEditSession.h"
#include "MetasequoiaIMEBaseStructure.h"
#include "Compartment.h"
#include "define.h"
#include <vector>

class CCompositionProcessorEngine
{
    friend class CMetasequoiaIME;

  public:
    enum class PreservedKeyAction
    {
        None,
        ToggleImeMode,
        ToggleDoubleSingleByteMode,
        TogglePunctuationMode
    };

    explicit CCompositionProcessorEngine(_In_ CMetasequoiaIME *pTextService);
    ~CCompositionProcessorEngine(void);

    BOOL SetupLanguageProfile(LANGID langid, REFGUID guidLanguageProfile, _In_ ITfThreadMgr *pThreadMgr,
                              TfClientId tfClientId, BOOL isSecureMode, BOOL isComLessMode);

    // Get language profile.
    GUID GetLanguageProfile(LANGID *plangid)
    {
        *plangid = _langid;
        return _guidProfile;
    }
    // Get locale
    LCID GetLocale()
    {
        return MAKELCID(_langid, SORT_DEFAULT);
    }

    BOOL IsVirtualKeyNeed(UINT uCode, _In_reads_(1) WCHAR *pwch, BOOL fComposing, CANDIDATE_MODE candidateMode,
                          BOOL hasCandidateWithWildcard, _Out_opt_ _KEYSTROKE_STATE *pKeyState);
    BOOL IsVirtualKeyNeedForFreshComposition(UINT uCode, _In_reads_(1) WCHAR *pwch,
                                             _Out_opt_ _KEYSTROKE_STATE *pKeyState);

    BOOL AddVirtualKey(WCHAR wch);
    // 句中辅助码触发键（反引号，或设置里勾了的分号）在 buffer 的 caret 处是不是编码键，规则见
    // engine/contracts/mid_sentence_helpcode.h。分号先让给 ing 韵母。
    static bool IsMidSentenceHelpcodeTriggerKey(UINT uCode, WCHAR wch, _In_reads_opt_(length) const WCHAR *buffer,
                                                DWORD_PTR length, DWORD_PTR caret);
    // 双拼直接辅助码开着时额外收的编码键：四码后的 /，以及不看奇偶的 ; 韵母。规则见
    // engine/contracts/direct_helpcode.h。
    static bool IsDirectHelpcodeInputKey(UINT uCode, WCHAR wch, _In_reads_opt_(length) const WCHAR *buffer,
                                         DWORD_PTR length, DWORD_PTR caret);
    // Shift+T 指定日期时间收的编码键：buffer 以 T 开头、插入后还能接成某种日期时间形状的数字、/ 和 :。
    // 规则见 engine/contracts/date_time_input.h。接不上的数字仍是选词键，Shift+数字始终选词。
    static bool IsDateTimeInputKey(UINT uCode, WCHAR wch, _In_reads_opt_(length) const WCHAR *buffer, DWORD_PTR length,
                                   DWORD_PTR caret);
    // V 模式（数字转中文、算式计算）收的编码键：V 后面的数字和 . + - * / ( )。规则见
    // engine/contracts/v_mode_input.h，前缀由 Server 的 VModeChanged 决定。
    static bool IsVModeInputKey(WCHAR wch, _In_reads_opt_(length) const WCHAR *buffer, DWORD_PTR length,
                                DWORD_PTR caret);
    // V 模式里数字键用来输入，选词改用 Shift+数字（同 U 模式）。放在 IsVModeInputKey 之后判断：Shift+8/9
    // 打出的 * ( 在那里已经当编码键收下了。
    static bool IsVModeShiftDigitSelectionKey(UINT uCode, _In_reads_opt_(length) const WCHAR *buffer, DWORD_PTR length);
    // 当句中辅助码触发键的分号在缓冲里记成反引号（与 Server 的 raw 一致），其余字符原样返回。
    static WCHAR NormalizeMidSentenceHelpcodeTrigger(WCHAR wch, _In_reads_opt_(length) const WCHAR *buffer,
                                                     DWORD_PTR length, DWORD_PTR caret);
    WCHAR NormalizeMidSentenceHelpcodeTrigger(WCHAR wch) const
    {
        return NormalizeMidSentenceHelpcodeTrigger(wch, _keystrokeBuffer.Get(), _keystrokeBuffer.GetLength(),
                                                   _caretPosition);
    }
    void RemoveVirtualKey(DWORD_PTR dwIndex);
    BOOL RemoveVirtualKeyBeforeCaret();
    BOOL RemoveVirtualKeyAtCaret();
    void PurgeVirtualKey();
    BOOL MoveCaret(int offset);
    DWORD_PTR GetCaretPosition() const
    {
        return _caretPosition;
    }
    // caretMap 是 Server 随预编辑带来的光标映射（engine/contracts/preedit_caret_map.h），为空或与
    // 按键缓冲长度对不上时按字母个数映射。
    void SetRenderedPreedit(std::wstring preedit, size_t prefixLength, std::vector<size_t> caretMap = {});
    DWORD_PTR GetRenderedCaretPosition() const;

    DWORD_PTR GetVirtualKeyLength()
    {
        return _keystrokeBuffer.GetLength();
    }
    CStringRange &GetKeystrokeBuffer()
    {
        return _keystrokeBuffer;
    };
    WCHAR GetVirtualKey(DWORD_PTR dwIndex);
    // Shift+U unicode input: composition buffer starts with 'U'.
    BOOL IsUnicodeModeComposition() const;

    void GetReadingStrings(                                          //
        _Inout_ CMetasequoiaImeArray<CStringRange> *pReadingStrings, //
        _Out_ BOOL *pIsWildcardIncluded                              //
    );
    void GetCandidateList(                                                //
        _Inout_ CMetasequoiaImeArray<CCandidateListItem> *pCandidateList, //
        BOOL isIncrementalWordSearch, BOOL isWildcardSearch               //
    );

    // Preserved key handler
    BOOL IsPreservedKeyEligible(REFGUID rguid);
    PreservedKeyAction GetPreservedKeyAction(REFGUID rguid) const;
    void OnPreservedKey(ITfContext *pContext, REFGUID rguid, _Out_ BOOL *pIsEaten, _In_ ITfThreadMgr *pThreadMgr,
                        TfClientId tfClientId, BOOL *pNeedToggleIMEMode, BOOL isPrevalidated = FALSE,
                        BOOL notifyServer = TRUE);

    // Toggle IME Mode
    void ToggleIMEMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    void SetIMEMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL bOpen);
    // Cycle replies require confirmed compartment state before the FIFO advances.
    HRESULT SetIMEModeForLanguageCycle(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL bOpen);
    BOOL CCompositionProcessorEngine::GetIMEMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    // Apply CN/EN compartment change deferred until after composition commit.
    // Closing KEYBOARD_OPENCLOSE before EndComposition makes CUAS/Win32 EDIT
    // finalize the same preedit twice (Chrome/TSF-only hosts are unaffected).
    void ApplyPendingImeModeAfterCompositionCommit(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    void SetPunctuationMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL bOpen);
    BOOL GetPunctuationMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    void SetDoubleSingleByteMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL bOpen);
    BOOL GetDoubleSingleByteMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);

    // Punctuation
    BOOL IsPunctuation(WCHAR wch);
    const WCHAR *GetPunctuation(WCHAR wch);
    // Smart punctuation (rime digit_separators-style): after ASCII letters/digits,
    // keep ',' '.' ':' as ASCII instead of mapping to Chinese punctuation.
    static BOOL IsSmartAsciiPunctuationKey(WCHAR wch);
    std::wstring ResolvePunctuation(WCHAR wch, WCHAR precedingChar);
    // Reversible smart punctuation mapping (Chinese form -> ASCII form). Each
    // symbol on its own maps; an auto-completed pair never arms the conversion.
    static bool IsSmartPunctuationChinese(WCHAR ch);
    static WCHAR GetSmartPunctuationAscii(WCHAR chinese);
    // Paired-punctuation auto-close emits both halves of a nest pair on the
    // opening key, so the closing key never reaches GetPunctuation to decrement
    // the depth. Undo the opening's increment here to keep the completed pair
    // net-zero; otherwise every auto-closed 《》 leaves the depth at 1 and the
    // next opening resolves to the inner 〈.
    void BalanceNestPairAfterAutoClose(WCHAR openingCode);

    BOOL IsDoubleSingleByte(WCHAR wch);
    BOOL IsWildcard()
    {
        return _isWildcard;
    }
    BOOL IsDisableWildcardAtFirst()
    {
        return _isDisableWildcardAtFirst;
    }
    BOOL IsWildcardChar(WCHAR wch)
    {
        return ((IsWildcardOneChar(wch) || IsWildcardAllChar(wch)) ? TRUE : FALSE);
    }
    BOOL IsWildcardOneChar(WCHAR wch)
    {
        return (wch == L'?' ? TRUE : FALSE);
    }
    BOOL IsWildcardAllChar(WCHAR wch)
    {
        return (wch == L'*' ? TRUE : FALSE);
    }
    BOOL IsKeystrokeSort()
    {
        return _isKeystrokeSort;
    }

    // Language bar control
    void SetLanguageBarStatus(DWORD status, BOOL isSet);

    void ConversionModeCompartmentUpdated(_In_ ITfThreadMgr *pThreadMgr);

    void ShowAllLanguageBarIcons();
    void HideAllLanguageBarIcons();
    void RefreshLanguageBarIcons();

    inline CCandidateRange *GetCandidateListIndexRange()
    {
        return &_candidateListIndexRange;
    }
    inline UINT GetCandidateListPhraseModifier()
    {
        return _candidateListPhraseModifier;
    }
    inline UINT GetCandidateWindowWidth()
    {
        return _candidateWndWidth;
    }

  private:
    void InitKeyStrokeTable();
    BOOL InitLanguageBar(_In_ CLangBarItemButton *pLanguageBar, _In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId,
                         REFGUID guidCompartment);

    struct _KEYSTROKE;
    BOOL IsVirtualKeyKeystrokeComposition(UINT uCode, _Out_opt_ _KEYSTROKE_STATE *pKeyState,
                                          KEYSTROKE_FUNCTION function);
    BOOL IsVirtualKeyKeystrokeCandidate(UINT uCode, _In_ _KEYSTROKE_STATE *pKeyState, CANDIDATE_MODE candidateMode,
                                        _Out_ BOOL *pfRetCode, _In_ CMetasequoiaImeArray<_KEYSTROKE> *pKeystrokeMetric);
    BOOL IsKeystrokeRange(UINT uCode, _Out_ _KEYSTROKE_STATE *pKeyState, CANDIDATE_MODE candidateMode);

    void SetupKeystroke();
    void SetupPreserved(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    void SetupConfiguration();
    void SetupLanguageBar(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL isSecureMode);
    void SetKeystrokeTable(_Inout_ CMetasequoiaImeArray<_KEYSTROKE> *pKeystroke);
    void SetupPunctuationPair();
    void CreateLanguageBarButton(DWORD dwEnable, GUID guidLangBar, _In_z_ LPCWSTR pwszDescriptionValue,
                                 _In_z_ LPCWSTR pwszTooltipValue, DWORD dwOnIconIndex, DWORD dwOffIconIndex,
                                 _Outptr_result_maybenull_ CLangBarItemButton **ppLangBarItemButton, BOOL isSecureMode);
    void SetInitialCandidateListRange();
    void SetDefaultCandidateTextFont();
    void InitializeMetasequoiaIMECompartment(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);

    class XPreservedKey;
    void SetPreservedKey(const CLSID clsid, TF_PRESERVEDKEY &tfPreservedKey, _In_z_ LPCWSTR pwszDescription,
                         _Out_ XPreservedKey *pXPreservedKey);
    BOOL InitPreservedKey(_In_ XPreservedKey *pXPreservedKey, _In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    BOOL CheckShiftKeyOnly(_In_ CMetasequoiaImeArray<TF_PRESERVEDKEY> *pTSFPreservedKeyTable);

    static HRESULT CompartmentCallback(_In_ void *pv, REFGUID guidCompartment);
    void PrivateCompartmentsUpdated(_In_ ITfThreadMgr *pThreadMgr);
    void KeyboardOpenCompartmentUpdated(_In_ ITfThreadMgr *pThreadMgr);
    HRESULT SetKeyboardOpenCompartment(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL isOpen);
    void SyncPunctuationWithImeMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL isOpen);
    void CommitCompositionOnExternalKeyboardClose();
    void ReleaseConfiguredImeModeDefense();
    // Requests the transient caret badge for a user action. Never call it for
    // programmatic compartment writes. trigger is a FanyImeCaretStateTrigger;
    // for anything but UserToggle the caller supplies the Caps Lock snapshot.
    void SendCaretStateSwitchEvent(UINT eventType, bool enabled, UINT trigger = 0, bool capsLockEnabled = false);

  private:
    struct _KEYSTROKE
    {
        UINT VirtualKey;
        UINT Modifiers;
        KEYSTROKE_FUNCTION Function;

        _KEYSTROKE()
        {
            VirtualKey = 0;
            Modifiers = 0;
            Function = FUNCTION_NONE;
        }
    };
    _KEYSTROKE _keystrokeTable[26];

    CStringRange _keystrokeBuffer;
    DWORD_PTR _caretPosition = 0;
    std::wstring _renderedPreedit;
    size_t _renderedPreeditPrefixLength = 0;
    std::vector<size_t> _renderedPreeditCaretMap;

    BOOL _hasWildcardIncludedInKeystrokeBuffer;

    LANGID _langid;
    GUID _guidProfile;
    TfClientId _tfClientId;

    CMetasequoiaImeArray<_KEYSTROKE> _KeystrokeComposition;
    CMetasequoiaImeArray<_KEYSTROKE> _KeystrokeCandidate;
    CMetasequoiaImeArray<_KEYSTROKE> _KeystrokeCandidateWildcard;
    CMetasequoiaImeArray<_KEYSTROKE> _KeystrokeCandidateSymbol;
    CMetasequoiaImeArray<_KEYSTROKE> _KeystrokeSymbol;

    // Preserved key data
    class XPreservedKey
    {
      public:
        XPreservedKey();
        ~XPreservedKey();
        BOOL UninitPreservedKey(_In_ ITfThreadMgr *pThreadMgr);

      public:
        CMetasequoiaImeArray<TF_PRESERVEDKEY> TSFPreservedKeyTable;
        GUID Guid;
        LPCWSTR Description;
    };

    XPreservedKey _PreservedKey_IMEMode;
    XPreservedKey _PreservedKey_IMEMode02;
    XPreservedKey _PreservedKey_IMEMode03;
    XPreservedKey _PreservedKey_EnglishInputMode;
    XPreservedKey _PreservedKey_DoubleSingleByte;
    XPreservedKey _PreservedKey_Punctuation;

    // Punctuation data
    CMetasequoiaImeArray<CPunctuationPair> _PunctuationPair;
    CMetasequoiaImeArray<CPunctuationNestPair> _PunctuationNestPair;

    // Language bar data
    CLangBarItemButton *_pLanguageBar_IMEMode;
    CLangBarItemButton *_pLanguageBar_DoubleSingleByte;
    CLangBarItemButton *_pLanguageBar_Punctuation;

    // Compartment
    CCompartment *_pCompartmentConversion;
    CCompartmentEventSink *_pCompartmentConversionEventSink;
    CCompartmentEventSink *_pCompartmentKeyboardOpenEventSink;
    CCompartmentEventSink *_pCompartmentDoubleSingleByteEventSink;
    CCompartmentEventSink *_pCompartmentPunctuationEventSink;
    ITfThreadMgr *_pOwnerThreadMgr;
    HWND _ownerMsgWndHandle;
    CMetasequoiaIME *_pTextService;
    BOOL _keyboardOpen;
    BOOL _keyboardOpenKnown;
    BOOL _suppressKeyboardCloseCommit;
    // After Activate applies input.default_ime_mode, Chromium (and Electron)
    // often rewrites TF_CONVERSIONMODE_* (NATIVE / SYMBOL / FULLSHAPE) while
    // focus is still on the non-editable shell. ConversionModeCompartmentUpdated
    // would then mirror those bits into OPENCLOSE and the punctuation /
    // fullwidth compartments. Defend by reasserting private state until the
    // user explicitly chooses a mode (Shift / langbar / FTB / preserved key).
    BOOL _defendConfiguredImeMode;
    BOOL _hasPendingImeModeAfterCompositionCommit;
    BOOL _pendingImeModeAfterCompositionCommit;

    // Configuration data.
    // Not bit-fields: BOOL is a signed int, so a one-bit field holds only -1 and 0 and every
    // `= TRUE` here stored -1 (C4463). Nothing compares these against TRUE today, but a
    // getter that returns -1 for "true" is a trap, and four ints cost nothing.
    BOOL _isWildcard;
    BOOL _isDisableWildcardAtFirst;
    BOOL _isKeystrokeSort;
    BOOL _isComLessMode;
    CCandidateRange _candidateListIndexRange;
    UINT _candidateListPhraseModifier;
    UINT _candidateWndWidth;

    static const int OUT_OF_FILE_INDEX = -1;
};
