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
#include "FanyUtils.h"
#include "FanyLog.h"
#include "EditSession.h"
#include "TfTextLayoutSink.h"
#include <new>

//////////////////////////////////////////////////////////////////////
//
// CMetasequoiaIME implementation.
//
//////////////////////////////////////////////////////////////////////

//+---------------------------------------------------------------------------
//
// _AddTextProcessorEngine
//
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_AddTextProcessorEngine()
{
    LANGID langid = 0;
    CLSID clsid = GUID_NULL;
    GUID guidProfile = GUID_NULL;

    // Get default profile.
    CTfInputProcessorProfile profile;

    if (FAILED(profile.CreateInstance()))
    {
        return FALSE;
    }

    if (FAILED(profile.GetCurrentLanguage(&langid)))
    {
        return FALSE;
    }

    if (FAILED(profile.GetDefaultLanguageProfile(langid, GUID_TFCAT_TIP_KEYBOARD, &clsid, &guidProfile)))
    {
        return FALSE;
    }

    // Is this already added?
    // Here is Idempotent Operation for this _AddTextProcessorEngine function.
    if (_pCompositionProcessorEngine != nullptr)
    {
        LANGID langidProfile = 0;
        GUID guidLanguageProfile = GUID_NULL;

        guidLanguageProfile = _pCompositionProcessorEngine->GetLanguageProfile(&langidProfile);
        if ((langid == langidProfile) && IsEqualGUID(guidProfile, guidLanguageProfile))
        {
            return TRUE;
        }
    }

    // Create composition processor engine
    if (_pCompositionProcessorEngine == nullptr)
    {
        _pCompositionProcessorEngine = new (std::nothrow) CCompositionProcessorEngine(this);
    }
    if (!_pCompositionProcessorEngine)
    {
        return FALSE;
    }

    // setup composition processor engine
    if (FALSE == _pCompositionProcessorEngine->SetupLanguageProfile(langid, guidProfile, _GetThreadMgr(),
                                                                    _GetClientId(), _IsSecureMode(), _IsComLess()))
    {
        return FALSE;
    }

    return TRUE;
}

//////////////////////////////////////////////////////////////////////
//
// CompositionProcessorEngine implementation.
//
//////////////////////////////////////////////////////////////////////

//+---------------------------------------------------------------------------
//
// ctor
//
//----------------------------------------------------------------------------

CCompositionProcessorEngine::CCompositionProcessorEngine(_In_ CMetasequoiaIME *pTextService)
{
    _langid = 0xffff;
    _guidProfile = GUID_NULL;
    _tfClientId = TF_CLIENTID_NULL;

    _pLanguageBar_IMEMode = nullptr;
    _pLanguageBar_DoubleSingleByte = nullptr;
    _pLanguageBar_Punctuation = nullptr;

    _pCompartmentConversion = nullptr;
    _pCompartmentKeyboardOpenEventSink = nullptr;
    _pCompartmentConversionEventSink = nullptr;
    _pCompartmentDoubleSingleByteEventSink = nullptr;
    _pCompartmentPunctuationEventSink = nullptr;
    _pOwnerThreadMgr = nullptr;
    _ownerMsgWndHandle = nullptr;
    _pTextService = pTextService;
    _keyboardOpen = FALSE;
    _keyboardOpenKnown = FALSE;
    _suppressKeyboardCloseCommit = FALSE;
    _defendConfiguredImeMode = FALSE;
    _hasPendingImeModeAfterCompositionCommit = FALSE;
    _pendingImeModeAfterCompositionCommit = FALSE;

    _hasWildcardIncludedInKeystrokeBuffer = FALSE;

    _isWildcard = FALSE;
    _isDisableWildcardAtFirst = FALSE;
    _isKeystrokeSort = FALSE;

    _candidateListPhraseModifier = 0;

    _candidateWndWidth = CAND_WIDTH;

    InitKeyStrokeTable();
}

//+---------------------------------------------------------------------------
//
// dtor
//
//----------------------------------------------------------------------------

CCompositionProcessorEngine::~CCompositionProcessorEngine()
{
    if (_pLanguageBar_IMEMode)
    {
        _pLanguageBar_IMEMode->CleanUp();
        _pLanguageBar_IMEMode->Release();
        _pLanguageBar_IMEMode = nullptr;
    }
    if (_pLanguageBar_DoubleSingleByte)
    {
        _pLanguageBar_DoubleSingleByte->CleanUp();
        _pLanguageBar_DoubleSingleByte->Release();
        _pLanguageBar_DoubleSingleByte = nullptr;
    }
    if (_pLanguageBar_Punctuation)
    {
        _pLanguageBar_Punctuation->CleanUp();
        _pLanguageBar_Punctuation->Release();
        _pLanguageBar_Punctuation = nullptr;
    }

    if (_pCompartmentConversion)
    {
        delete _pCompartmentConversion;
        _pCompartmentConversion = nullptr;
    }
    if (_pCompartmentKeyboardOpenEventSink)
    {
        _pCompartmentKeyboardOpenEventSink->_Unadvise();
        delete _pCompartmentKeyboardOpenEventSink;
        _pCompartmentKeyboardOpenEventSink = nullptr;
    }
    if (_pCompartmentConversionEventSink)
    {
        _pCompartmentConversionEventSink->_Unadvise();
        delete _pCompartmentConversionEventSink;
        _pCompartmentConversionEventSink = nullptr;
    }
    if (_pCompartmentDoubleSingleByteEventSink)
    {
        _pCompartmentDoubleSingleByteEventSink->_Unadvise();
        delete _pCompartmentDoubleSingleByteEventSink;
        _pCompartmentDoubleSingleByteEventSink = nullptr;
    }
    if (_pCompartmentPunctuationEventSink)
    {
        _pCompartmentPunctuationEventSink->_Unadvise();
        delete _pCompartmentPunctuationEventSink;
        _pCompartmentPunctuationEventSink = nullptr;
    }
    if (_pOwnerThreadMgr)
    {
        _pOwnerThreadMgr->Release();
        _pOwnerThreadMgr = nullptr;
    }
    _ownerMsgWndHandle = nullptr;
}

//+---------------------------------------------------------------------------
//
// SetupLanguageProfile
//
// Setup language profile for Composition Processor Engine.
// param
//     [in] LANGID langid = Specify language ID
//     [in] GUID guidLanguageProfile - Specify GUID language profile which GUID is as same as Text Service Framework
//     language profile. [in] ITfThreadMgr - pointer ITfThreadMgr. [in] tfClientId - TfClientId value. [in] isSecureMode
//     - secure mode
// returns
//     If setup succeeded, returns true. Otherwise returns false.
// N.B. For reverse conversion, ITfThreadMgr is NULL, TfClientId is 0 and isSecureMode is ignored.
//+---------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::SetupLanguageProfile(LANGID langid, REFGUID guidLanguageProfile,
                                                       _In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId,
                                                       BOOL isSecureMode, BOOL isComLessMode)
{
    BOOL ret = TRUE;
    if ((tfClientId == 0) && (pThreadMgr == nullptr))
    {
        ret = FALSE;
        goto Exit;
    }

    _isComLessMode = isComLessMode;
    _langid = langid;
    _guidProfile = guidLanguageProfile;
    _tfClientId = tfClientId;
    if (_pOwnerThreadMgr != pThreadMgr)
    {
        if (_pOwnerThreadMgr)
        {
            _pOwnerThreadMgr->Release();
        }
        _pOwnerThreadMgr = pThreadMgr;
        _pOwnerThreadMgr->AddRef();
    }
    _ownerMsgWndHandle = Global::msgWndHandle;

    SetupPreserved(pThreadMgr, tfClientId);
    InitializeMetasequoiaIMECompartment(pThreadMgr, tfClientId);
    SetupPunctuationPair();
    SetupLanguageBar(pThreadMgr, tfClientId, isSecureMode);
    SetupKeystroke();
    SetupConfiguration();

Exit:
    return ret;
}

//+---------------------------------------------------------------------------
//
// AddVirtualKey
// Add virtual key code to Composition Processor Engine for used to parse keystroke data.
// param
//     [in] uCode - Specify virtual key code.
// returns
//     State of Text Processor Engine.
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::AddVirtualKey(WCHAR wch)
{
    if (!wch)
    {
        return FALSE;
    }

    DWORD_PTR srgKeystrokeBufLen = _keystrokeBuffer.GetLength();
    _caretPosition = min(_caretPosition, srgKeystrokeBufLen);
    if (wch == L'\'' && ((_caretPosition > 0 && _keystrokeBuffer.Get()[_caretPosition - 1] == L'\'') ||
                         (_caretPosition < srgKeystrokeBufLen && _keystrokeBuffer.Get()[_caretPosition] == L'\'')))
    {
        return TRUE;
    }

    // Check if the keystroke buffer has reached the maximum length
    if (srgKeystrokeBufLen >= MAX_PINYIN_LENGTH)
    {
        return FALSE;
    }

    //
    // Insert at the logical composition caret.
    //
    PWCHAR pwch = new (std::nothrow) WCHAR[srgKeystrokeBufLen + 1];
    if (!pwch)
    {
        return FALSE;
    }

    memcpy(pwch, _keystrokeBuffer.Get(), _caretPosition * sizeof(WCHAR));
    pwch[_caretPosition] = wch;
    memcpy(pwch + _caretPosition + 1, _keystrokeBuffer.Get() + _caretPosition,
           (srgKeystrokeBufLen - _caretPosition) * sizeof(WCHAR));
    ++_caretPosition;

    if (_keystrokeBuffer.Get())
    {
        delete[] _keystrokeBuffer.Get();
    }

    _keystrokeBuffer.Set(pwch, srgKeystrokeBufLen + 1);

    std::wstring keyString(pwch, srgKeystrokeBufLen + 1);
    Global::PinyinString = keyString;

    return TRUE;
}

//+---------------------------------------------------------------------------
//
// RemoveVirtualKey
// Remove stored virtual key code.
// param
//     [in] dwIndex   - Specified index.
// returns
//     none.
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::RemoveVirtualKey(DWORD_PTR dwIndex)
{
    DWORD_PTR srgKeystrokeBufLen = _keystrokeBuffer.GetLength();
    if (dwIndex >= srgKeystrokeBufLen)
    {
        return;
    }

    if (dwIndex + 1 < srgKeystrokeBufLen)
    {
        // shift following eles left
        memmove((BYTE *)_keystrokeBuffer.Get() + (dwIndex * sizeof(WCHAR)),
                (BYTE *)_keystrokeBuffer.Get() + ((dwIndex + 1) * sizeof(WCHAR)),
                (srgKeystrokeBufLen - dwIndex - 1) * sizeof(WCHAR));
    }

    _keystrokeBuffer.Set(_keystrokeBuffer.Get(), srgKeystrokeBufLen - 1);
    if (_caretPosition > dwIndex)
    {
        --_caretPosition;
    }
    _caretPosition = min(_caretPosition, _keystrokeBuffer.GetLength());
}

BOOL CCompositionProcessorEngine::RemoveVirtualKeyBeforeCaret()
{
    if (_caretPosition == 0 || _keystrokeBuffer.GetLength() == 0)
    {
        return FALSE;
    }
    RemoveVirtualKey(_caretPosition - 1);
    return TRUE;
}

BOOL CCompositionProcessorEngine::RemoveVirtualKeyAtCaret()
{
    if (_caretPosition >= _keystrokeBuffer.GetLength())
    {
        return FALSE;
    }
    RemoveVirtualKey(_caretPosition);
    return TRUE;
}

BOOL CCompositionProcessorEngine::MoveCaret(int offset)
{
    const LONGLONG next = static_cast<LONGLONG>(_caretPosition) + offset;
    if (next < 0 || next > static_cast<LONGLONG>(_keystrokeBuffer.GetLength()))
    {
        return FALSE;
    }
    _caretPosition = static_cast<DWORD_PTR>(next);
    return TRUE;
}

void CCompositionProcessorEngine::SetRenderedPreedit(std::wstring preedit, size_t prefixLength,
                                                     std::vector<size_t> caretMap)
{
    _renderedPreedit = std::move(preedit);
    _renderedPreeditPrefixLength = min(prefixLength, _renderedPreedit.size());
    _renderedPreeditCaretMap = std::move(caretMap);
}

DWORD_PTR CCompositionProcessorEngine::GetRenderedCaretPosition() const
{
    if (!_renderedPreeditCaretMap.empty() && _renderedPreeditCaretMap.size() == _keystrokeBuffer.GetLength() + 1)
    {
        return min(_renderedPreeditCaretMap[min(_caretPosition, _keystrokeBuffer.GetLength())],
                   _renderedPreedit.size());
    }
    size_t lettersBeforeCaret = 0;
    for (DWORD_PTR i = 0; i < min(_caretPosition, _keystrokeBuffer.GetLength()); ++i)
    {
        if (_keystrokeBuffer.Get()[i] != L'\'')
        {
            ++lettersBeforeCaret;
        }
    }
    size_t displayPosition = _renderedPreeditPrefixLength;
    size_t seenLetters = 0;
    while (displayPosition < _renderedPreedit.size() && seenLetters < lettersBeforeCaret)
    {
        if (_renderedPreedit[displayPosition] != L'\'')
        {
            ++seenLetters;
        }
        ++displayPosition;
    }
    if (_caretPosition > 0 && _keystrokeBuffer.Get()[_caretPosition - 1] == L'\'')
    {
        while (displayPosition < _renderedPreedit.size() && _renderedPreedit[displayPosition] == L'\'')
        {
            ++displayPosition;
        }
    }
    return displayPosition;
}

//+---------------------------------------------------------------------------
//
// PurgeVirtualKey
// Purge stored virtual key code.
// param
//     none.
// returns
//     none.
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::PurgeVirtualKey()
{
    if (_keystrokeBuffer.Get())
    {
        delete[] _keystrokeBuffer.Get();
        _keystrokeBuffer.Set(NULL, 0);
    }
    _caretPosition = 0;
    _renderedPreedit.clear();
    _renderedPreeditPrefixLength = 0;
    _renderedPreeditCaretMap.clear();
}

WCHAR CCompositionProcessorEngine::GetVirtualKey(DWORD_PTR dwIndex)
{
    if (dwIndex < _keystrokeBuffer.GetLength())
    {
        return *(_keystrokeBuffer.Get() + dwIndex);
    }
    return 0;
}
//+---------------------------------------------------------------------------
//
// GetReadingStrings
// Retrieves string from Composition Processor Engine.
// param
//     [out] pReadingStrings - Specified returns pointer of CUnicodeString.
// returns
//     none
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::GetReadingStrings(_Inout_ CMetasequoiaImeArray<CStringRange> *pReadingStrings,
                                                    _Out_ BOOL *pIsWildcardIncluded)
{
    CStringRange oneKeystroke;

    _hasWildcardIncludedInKeystrokeBuffer = FALSE;

    if (pReadingStrings->Count() == 0 && _keystrokeBuffer.GetLength())
    {
        CStringRange *pNewString = nullptr;

        pNewString = pReadingStrings->Append();
        if (pNewString)
        {
            *pNewString = _keystrokeBuffer;
        }

        // V 模式的 * 是乘号不是通配符：标成通配符会让空格走通配符转换，算式就上不了屏。
        const bool v_mode = FanyImeVModeInput::IsComposition(_keystrokeBuffer.Get(), _keystrokeBuffer.GetLength(),
                                                             Global::VModeTrigger.load(std::memory_order_relaxed));
        for (DWORD index = 0; index < _keystrokeBuffer.GetLength() && !v_mode; index++)
        {
            oneKeystroke.Set(_keystrokeBuffer.Get() + index, 1);

            if (IsWildcard() && IsWildcardChar(*oneKeystroke.Get()))
            {
                _hasWildcardIncludedInKeystrokeBuffer = TRUE;
            }
        }
    }

    *pIsWildcardIncluded = _hasWildcardIncludedInKeystrokeBuffer;
}

//+---------------------------------------------------------------------------
//
// GetCandidateList
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::GetCandidateList(_Inout_ CMetasequoiaImeArray<CCandidateListItem> *pCandidateList,
                                                   BOOL isIncrementalWordSearch, BOOL isWildcardSearch)
{
    isIncrementalWordSearch;
    isWildcardSearch;

    //
    // Candidate generation now lives in the IPC server. TSF keeps a minimal
    // local mirror so selection/page bookkeeping still works.
    //
    const std::wstring keystrokeStr(_keystrokeBuffer.Get(), _keystrokeBuffer.GetLength());
    CCandidateListItem *pLI = nullptr;
    pLI = pCandidateList->Append();
    if (pLI)
    {
        pLI->_ItemString.Set(keystrokeStr.c_str(), keystrokeStr.size());
        pLI->_FindKeyCode.Set(keystrokeStr.c_str(), keystrokeStr.size());
    }
    for (UINT index = 0; index < pCandidateList->Count();)
    {
        CCandidateListItem *pItem = pCandidateList->GetAt(index);
        CStringRange startItemString;
        CStringRange endItemString;

        startItemString.Set(pItem->_ItemString.Get(), 1);
        endItemString.Set(pItem->_ItemString.Get() + pItem->_ItemString.GetLength() - 1, 1);

        index++;
    }
    return;
}

//+---------------------------------------------------------------------------
//
// SetupConfiguration
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::SetupConfiguration()
{
    _isWildcard = TRUE;
    _isDisableWildcardAtFirst = TRUE;
    _isKeystrokeSort = TRUE;
    _candidateWndWidth = CAND_WIDTH;

    SetInitialCandidateListRange();

    SetDefaultCandidateTextFont();

    return;
}

//+---------------------------------------------------------------------------
//
// SetupLanguageBar
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::SetupLanguageBar(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId,
                                                   BOOL isSecureMode)
{
    // Taskbar language-bar buttons are optional per the [language_bar] config; a hidden button is never
    // registered with ITfLangBarItemMgr and therefore never shown next to the input indicator. The master
    // `enabled` flag hides every button at once, and each per-button flag hides only its own button.
    const FanyUtils::LanguageBarVisibility langBar = FanyUtils::ReadConfiguredLanguageBarVisibility();

    DWORD dwEnable = 1;
    CreateLanguageBarButton(dwEnable, GUID_LBI_INPUTMODE, Global::LangbarImeModeDescription, Global::ImeModeDescription,
                            Global::ImeModeOnIcoIndex, Global::ImeModeOffIcoIndex, &_pLanguageBar_IMEMode,
                            isSecureMode);
    CreateLanguageBarButton(dwEnable, Global::MetasequoiaIMEGuidLangBarDoubleSingleByte,
                            Global::LangbarDoubleSingleByteDescription, Global::DoubleSingleByteDescription,
                            Global::DoubleSingleByteOnIcoIndex, Global::DoubleSingleByteOffIcoIndex,
                            &_pLanguageBar_DoubleSingleByte, isSecureMode);
    CreateLanguageBarButton(dwEnable, Global::MetasequoiaIMEGuidLangBarPunctuation,
                            Global::LangbarPunctuationDescription, Global::PunctuationDescription,
                            Global::PunctuationOnIcoIndex, Global::PunctuationOffIcoIndex, &_pLanguageBar_Punctuation,
                            isSecureMode);

    InitLanguageBar(_pLanguageBar_IMEMode, pThreadMgr, tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                    langBar.enabled && langBar.ime_mode);
    InitLanguageBar(_pLanguageBar_DoubleSingleByte, pThreadMgr, tfClientId,
                    Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte,
                    langBar.enabled && langBar.double_single_byte);
    InitLanguageBar(_pLanguageBar_Punctuation, pThreadMgr, tfClientId, Global::MetasequoiaIMEGuidCompartmentPunctuation,
                    langBar.enabled && langBar.punctuation);

    _pCompartmentConversion =
        new (std::nothrow) CCompartment(pThreadMgr, tfClientId, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
    _pCompartmentKeyboardOpenEventSink = new (std::nothrow) CCompartmentEventSink(CompartmentCallback, this);
    _pCompartmentConversionEventSink = new (std::nothrow) CCompartmentEventSink(CompartmentCallback, this);
    _pCompartmentDoubleSingleByteEventSink = new (std::nothrow) CCompartmentEventSink(CompartmentCallback, this);
    _pCompartmentPunctuationEventSink = new (std::nothrow) CCompartmentEventSink(CompartmentCallback, this);

    if (_pCompartmentKeyboardOpenEventSink)
    {
        _pCompartmentKeyboardOpenEventSink->_Advise(pThreadMgr, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    }
    if (_pCompartmentConversionEventSink)
    {
        _pCompartmentConversionEventSink->_Advise(pThreadMgr, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
    }
    if (_pCompartmentDoubleSingleByteEventSink)
    {
        _pCompartmentDoubleSingleByteEventSink->_Advise(pThreadMgr,
                                                        Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte);
    }
    if (_pCompartmentPunctuationEventSink)
    {
        _pCompartmentPunctuationEventSink->_Advise(pThreadMgr, Global::MetasequoiaIMEGuidCompartmentPunctuation);
    }

    return;
}

//+---------------------------------------------------------------------------
//
// CreateLanguageBarButton
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::CreateLanguageBarButton(
    DWORD dwEnable, GUID guidLangBar, _In_z_ LPCWSTR pwszDescriptionValue, _In_z_ LPCWSTR pwszTooltipValue,
    DWORD dwOnIconIndex, DWORD dwOffIconIndex, _Outptr_result_maybenull_ CLangBarItemButton **ppLangBarItemButton,
    BOOL isSecureMode)
{
    dwEnable;

    if (ppLangBarItemButton)
    {
        *ppLangBarItemButton = new (std::nothrow) CLangBarItemButton(
            guidLangBar, pwszDescriptionValue, pwszTooltipValue, dwOnIconIndex, dwOffIconIndex, isSecureMode);
    }

    return;
}

//+---------------------------------------------------------------------------
//
// InitLanguageBar
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::InitLanguageBar(_In_ CLangBarItemButton *pLangBarItemButton,
                                                  _In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId,
                                                  REFGUID guidCompartment, bool registerOnLanguageBar)
{
    if (pLangBarItemButton && registerOnLanguageBar)
    {
        if (pLangBarItemButton->_AddItem(pThreadMgr) == S_OK)
        {
            if (pLangBarItemButton->_RegisterCompartment(pThreadMgr, tfClientId, guidCompartment))
            {
                return TRUE;
            }
        }
    }
    return FALSE;
}

//+---------------------------------------------------------------------------
//
// CMetasequoiaIME::CreateInstance
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::CreateInstance(REFCLSID rclsid, REFIID riid, _Outptr_result_maybenull_ LPVOID *ppv,
                                        _Out_opt_ HINSTANCE *phInst, BOOL isComLessMode)
{
    HRESULT hr = S_OK;
    if (phInst == nullptr)
    {
        return E_INVALIDARG;
    }

    *phInst = nullptr;

    if (!isComLessMode)
    {
        hr = ::CoCreateInstance(rclsid, NULL, CLSCTX_INPROC_SERVER, riid, ppv);
    }
    else
    {
        hr = CMetasequoiaIME::ComLessCreateInstance(rclsid, riid, ppv, phInst);
    }

    return hr;
}

//+---------------------------------------------------------------------------
//
// CMetasequoiaIME::ComLessCreateInstance
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::ComLessCreateInstance(REFGUID rclsid, REFIID riid, _Outptr_result_maybenull_ void **ppv,
                                               _Out_opt_ HINSTANCE *phInst)
{
    HRESULT hr = S_OK;
    HINSTANCE metasequoiaIMEDllHandle = nullptr;
    WCHAR wchPath[MAX_PATH] = {'\0'};
    WCHAR szExpandedPath[MAX_PATH] = {'\0'};
    DWORD dwCnt = 0;
    *ppv = nullptr;

    hr = phInst ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
    {
        *phInst = nullptr;
        hr = CMetasequoiaIME::GetComModuleName(rclsid, wchPath, ARRAYSIZE(wchPath));
        if (SUCCEEDED(hr))
        {
            dwCnt = ExpandEnvironmentStringsW(wchPath, szExpandedPath, ARRAYSIZE(szExpandedPath));
            hr = (0 < dwCnt && dwCnt <= ARRAYSIZE(szExpandedPath)) ? S_OK : E_FAIL;
            if (SUCCEEDED(hr))
            {
                metasequoiaIMEDllHandle = LoadLibraryEx(szExpandedPath, NULL, 0);
                hr = metasequoiaIMEDllHandle ? S_OK : E_FAIL;
                if (SUCCEEDED(hr))
                {
                    *phInst = metasequoiaIMEDllHandle;
                    FARPROC pfn = GetProcAddress(metasequoiaIMEDllHandle, "DllGetClassObject");
                    hr = pfn ? S_OK : E_FAIL;
                    if (SUCCEEDED(hr))
                    {
                        IClassFactory *pClassFactory = nullptr;
                        hr = ((HRESULT(STDAPICALLTYPE *)(REFCLSID rclsid, REFIID riid, LPVOID * ppv))(pfn))(
                            rclsid, IID_IClassFactory, (void **)&pClassFactory);
                        if (SUCCEEDED(hr) && pClassFactory)
                        {
                            hr = pClassFactory->CreateInstance(NULL, riid, ppv);
                            pClassFactory->Release();
                        }
                    }
                }
            }
        }
    }

    if (!SUCCEEDED(hr) && phInst && *phInst)
    {
        FreeLibrary(*phInst);
        *phInst = 0;
    }
    return hr;
}

//+---------------------------------------------------------------------------
//
// CMetasequoiaIME::GetComModuleName
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::GetComModuleName(REFGUID rclsid, _Out_writes_(cchPath) WCHAR *wchPath, DWORD cchPath)
{
    HRESULT hr = S_OK;

    CRegKey key;
    WCHAR wchClsid[CLSID_STRLEN + 1];
    hr = CLSIDToString(rclsid, wchClsid) ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
    {
        WCHAR wchKey[MAX_PATH];
        hr = StringCchPrintfW(wchKey, ARRAYSIZE(wchKey), L"CLSID\\%s\\InProcServer32", wchClsid);
        if (SUCCEEDED(hr))
        {
            hr = (key.Open(HKEY_CLASSES_ROOT, wchKey, KEY_READ) == ERROR_SUCCESS) ? S_OK : E_FAIL;
            if (SUCCEEDED(hr))
            {
                WCHAR wszModel[MAX_PATH];
                ULONG cch = ARRAYSIZE(wszModel);
                hr = (key.QueryStringValue(L"ThreadingModel", wszModel, &cch) == ERROR_SUCCESS) ? S_OK : E_FAIL;
                if (SUCCEEDED(hr))
                {
                    if (CompareStringOrdinal(wszModel, -1, L"Apartment", -1, TRUE) == CSTR_EQUAL)
                    {
                        hr = (key.QueryStringValue(NULL, wchPath, &cchPath) == ERROR_SUCCESS) ? S_OK : E_FAIL;
                    }
                    else
                    {
                        hr = E_FAIL;
                    }
                }
            }
        }
    }

    return hr;
}

void CCompositionProcessorEngine::ShowAllLanguageBarIcons()
{
    SetLanguageBarStatus(TF_LBI_STATUS_HIDDEN, FALSE);
}

void CCompositionProcessorEngine::HideAllLanguageBarIcons()
{
    SetLanguageBarStatus(TF_LBI_STATUS_HIDDEN, TRUE);
}

void CCompositionProcessorEngine::SetInitialCandidateListRange()
{
    for (DWORD i = 1; i <= CANDWND_ITEM_CNT_PER_PAGE; i++)
    {
        DWORD *pNewIndexRange = nullptr;

        pNewIndexRange = _candidateListIndexRange.Append();
        if (pNewIndexRange != nullptr)
        {
            *pNewIndexRange = i;
        }
    }
}

void CCompositionProcessorEngine::SetDefaultCandidateTextFont()
{
    // Candidate Text Font
    if (Global::defaultlFontHandle == nullptr)
    {
        WCHAR fontName[50] = {'\0'};
        LoadString(Global::dllInstanceHandle, IDS_DEFAULT_FONT, fontName, 50);
        Global::defaultlFontHandle = CreateFont(-MulDiv(10, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72), 0, 0, 0,
                                                FW_MEDIUM, 0, 0, 0, 0, 0, 0, 0, 0, fontName);
        if (!Global::defaultlFontHandle)
        {
            LOGFONT lf;
            SystemParametersInfo(SPI_GETICONTITLELOGFONT, sizeof(LOGFONT), &lf, 0);
            // Fall back to the default GUI font on failure.
            Global::defaultlFontHandle = CreateFont(-MulDiv(10, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72), 0, 0, 0,
                                                    FW_MEDIUM, 0, 0, 0, 0, 0, 0, 0, 0, lf.lfFaceName);
        }
    }
}
