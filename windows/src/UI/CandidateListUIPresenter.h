#pragma once

#include "Private.h"
#include "MetasequoiaIME.h"
#include "CandidateSessionState.h"
#include "CompositionProcessorEngine.h"
#include "MetasequoiaIMEBaseStructure.h"
#include "KeyHandlerEditSession.h"
#include "TfTextLayoutSink.h"
#include "../../../engine/contracts/uiless_page.h"
#include <cstdint>
#include <string>

class CReadingLine;

enum CANDWND_ACTION
{
    CAND_ITEM_SELECT
};

//+---------------------------------------------------------------------------
//
// CCandidateListUIPresenter
//
// ITfCandidateListUIElement / ITfIntegratableCandidateListUIElement is used for
// UILess mode support
// ITfCandidateListUIElementBehavior sends the Selection behavior message to
// 3rd party IME.
//----------------------------------------------------------------------------

class CCandidateListUIPresenter : public CTfTextLayoutSink,
                                  public ITfCandidateListUIElementBehavior,
                                  public ITfIntegratableCandidateListUIElement
{
  public:
    CCandidateListUIPresenter(_In_ CMetasequoiaIME *pTextService, KEYSTROKE_CATEGORY Category,
                              _In_ CCandidateRange *pIndexRange, BOOL hideWindow);
    virtual ~CCandidateListUIPresenter();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, _Outptr_ void **ppvObj);
    STDMETHODIMP_(ULONG) AddRef(void);
    STDMETHODIMP_(ULONG) Release(void);

    // ITfUIElement
    STDMETHODIMP GetDescription(BSTR *pbstr);
    STDMETHODIMP GetGUID(GUID *pguid);
    STDMETHODIMP Show(BOOL showCandidateWindow);
    STDMETHODIMP IsShown(BOOL *pIsShow);

    // ITfCandidateListUIElement
    STDMETHODIMP GetUpdatedFlags(DWORD *pdwFlags);
    STDMETHODIMP GetDocumentMgr(ITfDocumentMgr **ppdim);
    STDMETHODIMP GetCount(UINT *pCandidateCount);
    STDMETHODIMP GetSelection(UINT *pSelectedCandidateIndex);
    STDMETHODIMP GetString(UINT uIndex, BSTR *pbstr);
    STDMETHODIMP GetPageIndex(UINT *pIndex, UINT uSize, UINT *puPageCnt);
    STDMETHODIMP SetPageIndex(UINT *pIndex, UINT uPageCnt);
    STDMETHODIMP GetCurrentPage(UINT *puPage);

    // ITfCandidateListUIElementBehavior methods
    STDMETHODIMP SetSelection(UINT nIndex);
    STDMETHODIMP Finalize(void);
    STDMETHODIMP Abort(void);

    // ITfIntegratableCandidateListUIElement
    STDMETHODIMP SetIntegrationStyle(GUID guidIntegrationStyle);
    STDMETHODIMP GetSelectionStyle(_Out_ TfIntegratableCandidateListSelectionStyle *ptfSelectionStyle);
    STDMETHODIMP OnKeyDown(_In_ WPARAM wParam, _In_ LPARAM lParam, _Out_ BOOL *pIsEaten);
    STDMETHODIMP ShowCandidateNumbers(_Out_ BOOL *pIsShow);
    STDMETHODIMP FinalizeExactCompositionString();

    virtual HRESULT _StartCandidateList(TfClientId tfClientId, _In_ ITfDocumentMgr *pDocumentMgr,
                                        _In_ ITfContext *pContextDocument, TfEditCookie ec,
                                        _In_ ITfRange *pRangeComposition, UINT wndWidth);
    void _EndCandidateList();
    void _PrepareForAsyncCleanup();
    BOOL _IsAsyncCleanupPending() const
    {
        return _asyncCleanupPending;
    }

    void _NotifyUI();
    void _SetText(_In_ CMetasequoiaImeArray<CCandidateListItem> *pCandidateList, BOOL isAddFindKeyCode);
    void _ClearList();
    void _ApplyUiLessCandidatePage(_In_ const std::wstring &page, int selectedIndex = 0);
    // Applies a Server page unless an older generation than the one shown; returns whether it was applied.
    bool _ApplyUiLessPayload(_In_ const FanyImeUiLessPage::Payload &payload);
    void _NotifyUiLessHost();
    bool _ConsumeUiLessCompositionReply(uint64_t requestId);
    UINT _GetCount() const
    {
        return _candidateState.GetCount();
    }

    DWORD_PTR _GetSelectedCandidateString(_Outptr_result_maybenull_ const WCHAR **ppwchCandidateString);
    BOOL _SetSelectionInPage(int nPos)
    {
        return _candidateState.SetSelectionInPage(nPos);
    }

    BOOL _MoveSelection(_In_ int offSet);
    BOOL _SetSelection(_In_ int selectedIndex);
    BOOL _MovePage(_In_ int offSet);

    void _MoveWindowToTextExt();

    // CTfTextLayoutSink
    virtual VOID _LayoutChangeNotification(_In_ RECT *lpRect);
    virtual VOID _LayoutDestroyNotification();

    // Event for ITfThreadFocusSink
    virtual HRESULT OnSetThreadFocus();
    virtual HRESULT OnKillThreadFocus();

    void AdviseUIChangedByArrowKey(_In_ KEYSTROKE_FUNCTION arrowKey);

    BOOL _IsShowMode() const
    {
        return _isShowMode;
    }

  private:
    virtual HRESULT CALLBACK _CandidateChangeNotification(_In_ enum CANDWND_ACTION action);

    HRESULT _UpdateUIElement();

    HRESULT BeginUIElement();
    HRESULT EndUIElement();

    void AddCandidateToCandidateListUI(_In_ CMetasequoiaImeArray<CCandidateListItem> *pCandidateList,
                                       BOOL isAddFindKeyCode);

    void SetPageIndexWithScrollInfo(_In_ CMetasequoiaImeArray<CCandidateListItem> *pCandidateList);
    void WriteCandidateUiPayload(_In_ UINT writeFlag);
    void BeginCandidateUiSession();
    void UpdateCandidateUiSession();
    void MoveCandidateUiSession();
    void _RecordSentCaretPoint(bool sent);
    void EndCandidateUiSession();
    void _LoadUiLessCandidatesFromSharedMemory();
    void _RequestCancelComposition();
    void _ReplaceCandidateListFromPage(_In_ const std::wstring &page);

  protected:
    BOOL _isShowMode;
    BOOL _hideWindow;
    BOOL _candidateWindowVisible;

  private:
    CCandidateSessionState _candidateState;
    CCandidateRange *_pIndexRange;
    KEYSTROKE_CATEGORY _Category;
    DWORD _updatedFlags;
    DWORD _uiElementId;
    CMetasequoiaIME *_pTextService;
    LONG _refCount;
    BOOL _candidateUiSessionActive;
    // Caret last delivered to Server by Show/Move. Some hosts (WeChat) raise a
    // dozen identical layout changes per key; resending each one floods Main.
    bool _hasSentCaretPoint = false;
    int _sentCaretPoint[2] = {};
    BOOL _asyncCleanupPending;
    std::wstring _lastUiLessCandidatePage;
    // Generation of the Server page shown to a UI-less host. The worker push and the key reply arrive on
    // different pipes, so a page may land after a newer one.
    std::uint64_t _lastUiLessGeneration = 0;
    // Shared memory only holds a plain word list once the Server treats this session as UI-less; before that it is
    // the Server's own HTML page (helpcodes, translation spans), which a host would show as garbage.
    bool _serverUiLessPageSeen = false;
};
