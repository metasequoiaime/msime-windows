#include "Private.h"
#include "EditSession.h"
#include "GetTextExtentEditSession.h"
#include "TfTextLayoutSink.h"
#include "Ipc.h"
#include "FanyDefines.h"
#include "CaretAnchorPolicy.h"
#include <debugapi.h>
#include <fmt/xchar.h>

//+---------------------------------------------------------------------------
//
// ctor
//
//----------------------------------------------------------------------------

CGetTextExtentEditSession::CGetTextExtentEditSession(_In_ CMetasequoiaIME *pTextService, _In_ ITfContext *pContext,
                                                     _In_ ITfContextView *pContextView,
                                                     _In_ ITfRange *pRangeComposition,
                                                     _In_ CTfTextLayoutSink *pTfTextLayoutSink)
    : CEditSessionBase(pTextService, pContext)
{
    _pContextView = pContextView;
    _pRangeComposition = pRangeComposition;
    _pTfTextLayoutSink = pTfTextLayoutSink;
}

//+---------------------------------------------------------------------------
//
// ITfEditSession::DoEditSession
//
//----------------------------------------------------------------------------

STDAPI CGetTextExtentEditSession::DoEditSession(TfEditCookie ec)
{
    RECT rc = {0, 0, 0, 0};
    BOOL isClipped = TRUE;
    HRESULT hr = S_OK;

    hr = _pContextView->GetTextExt(ec, _pRangeComposition, &rc, &isClipped);

    if (SUCCEEDED(hr))
    {
        // Clipping can leave a zero-width caret with a usable position and height
        if (isClipped && !IsUsableCaretExtent(rc.left, rc.top, rc.right, rc.bottom))
        {
            Global::Point[0] = 0;
            Global::Point[1] = Global::INVALID_Y;
            return S_OK;
        }

        const POINT anchor = GetPhysicalTextAnchor(_pContextView, rc);
        Global::Point[0] = anchor.x;
        Global::Point[1] = anchor.y;
        // This is the per-keystroke caret source. Tell the sink the session now
        // has a usable anchor, so a later transient TS_E_NOLAYOUT can be answered
        // by keeping this position instead of parking the window off-screen.
        _pTfTextLayoutSink->_MarkAnchorValid();
        if (Global::current_process_name == Global::ZEN_BROWSER)
        {
            Global::firefox_like_cnt++;
            if (Global::firefox_like_cnt == 3)
            {
                _pTfTextLayoutSink->_LayoutChangeNotification(&rc);
            }
        }
        else
        {
            _pTfTextLayoutSink->_LayoutChangeNotification(&rc);
        }
    }

    return S_OK;
}
