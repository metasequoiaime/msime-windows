#include "TextInputCtrl.h"
#include "commdlg.h"
#include "TsfTextServices.h"

#include "msimeui/Compat.h"

#include <algorithm>

using std::max;
using std::min;

#define TEXTINPUTCTRL_CLASSNAME TEXT("TextInputCtrlClassName")
#define TIMERID_CARET 0x100

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

ATOM CTextInputCtrl::RegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEX wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = (WNDPROC)s_WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = NULL;
    wcex.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wcex.hbrBackground = NULL;
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = TEXTINPUTCTRL_CLASSNAME;
    wcex.hIconSm = NULL;

    return RegisterClassEx(&wcex);
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

HWND CTextInputCtrl::Create(HWND hwndParent)
{
    _hwnd =
        CreateWindow(TEXTINPUTCTRL_CLASSNAME, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwndParent, NULL, g_hInst, this);

    return _hwnd;
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

LRESULT CALLBACK CTextInputCtrl::s_WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT ps;
    CTextInputCtrl *ptic;
    switch (message)
    {
    case WM_CREATE:
        SetThis(hwnd, ((CREATESTRUCT *)lParam)->lpCreateParams);
        SetTimer(hwnd, TIMERID_CARET, GetCaretBlinkTime(), NULL);
        GetThis(hwnd)->OnCreate(hwnd, wParam, lParam);
        break;

    case WM_DESTROY:
        ptic = GetThis(hwnd);
        if (ptic)
        {
            ptic->OnDestroy();
        }
        break;

    case WM_SETFOCUS:
        ptic = GetThis(hwnd);
        if (ptic)
        {
            ptic->OnSetFocus(wParam, lParam);
        }
        break;

    case WM_PAINT:
        ptic = GetThis(hwnd);
        BeginPaint(hwnd, &ps);
        if (ptic)
            ptic->OnPaint();
        EndPaint(hwnd, &ps);
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        ptic = GetThis(hwnd);
        if (ptic)
            ptic->OnSize();
        break;

    case WM_KEYDOWN:
        ptic = GetThis(hwnd);
        if (ptic)
            ptic->OnKeyDown(wParam, lParam);
        break;

    case WM_LBUTTONDOWN:
        ptic = GetThis(hwnd);
        if (ptic)
            ptic->OnLButtonDown(wParam, lParam);
        break;

    case WM_LBUTTONUP:
        ptic = GetThis(hwnd);
        if (ptic)
            ptic->OnLButtonUp(wParam, lParam);
        break;

    case WM_RBUTTONDOWN:
        ptic = GetThis(hwnd);
        if (ptic)
            ptic->OnRButtonDown(wParam, lParam);
        break;

    case WM_RBUTTONUP:
        ptic = GetThis(hwnd);
        if (ptic)
            ptic->OnRButtonUp(wParam, lParam);
        break;

    case WM_MOUSEMOVE:
        ptic = GetThis(hwnd);
        if (ptic)
            ptic->OnMouseMove(wParam, lParam);
        break;

    case WM_IME_COMPOSITION:
        if (lParam & GCS_RESULTSTR)
        {
            HIMC himc = ImmGetContext(hwnd);

            if (himc)
            {
                LONG nSize = ImmGetCompositionString(himc, GCS_RESULTSTR, NULL, 0);
                if (nSize)
                {
                    LPWSTR psz = (LPWSTR)LocalAlloc(LPTR, nSize + sizeof(WCHAR));
                    if (psz)
                    {
                        ImmGetCompositionString(himc, GCS_RESULTSTR, psz, nSize);
                        LocalFree(psz);
                    }
                }
            }
            ImmReleaseContext(hwnd, himc);

            //
            // If we don't want to receive WM_IME_CHAR or WM_CHAR with
            // this result string, we should not call DefWindowProc()
            // with GCS_RESULTSTR, GCS_RESULTREADSTR, GCS_RESULTCLAUSE and
            // GCS_RESULTREADCLAUSE flags.
            //
            // lParam &= ~(GCS_RESULTCLAUSE |
            //             GCS_RESULTREADCLAUSE |
            //             GCS_RESULTREADSTR |
            //             GCS_RESULTSTR);
            // if (!lParam)
            //     break;
            //
        }
        return DefWindowProc(hwnd, message, wParam, lParam);

    case WM_IME_CHAR:
        //
        // wParam is a character of the result string.
        // if we don't want to receive WM_CHAR message for this character,
        // we should not call DefWindowProc().
        //
        return DefWindowProc(hwnd, message, wParam, lParam);

    case WM_CHAR:
        //
        // wParam is a character of the result string.
        //

        switch ((WCHAR)wParam)
        {
        case 0x08:
        case 0x0a:
        case 0x7f:
            return 0;
        default:
            break;
        }

        ptic = GetThis(hwnd);
        if (ptic)
        {
            WCHAR wc[2];
            wc[0] = (WCHAR)wParam;
            wc[1] = L'\0';
            ptic->_editor.InsertAtSelection(wc);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case WM_TIMER:
        if (wParam == TIMERID_CARET)
        {
            ptic = GetThis(hwnd);
            if (ptic)
            {
                ptic->_editor.BlinkCaret();
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }

        break;

    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

BOOL CTextInputCtrl::CreateDeviceResources()
{
    if (_pRenderTarget)
    {
        return TRUE;
    }

    RECT rc = {};
    GetClientRect(_hwnd, &rc);

    if (!_pD2DFactory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &_pD2DFactory)))
    {
        return FALSE;
    }

    if (!_pDWriteFactory && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                       reinterpret_cast<IUnknown **>(&_pDWriteFactory))))
    {
        return FALSE;
    }

    if (!_editor.InitializeRenderResources(_pDWriteFactory))
    {
        return FALSE;
    }

    const D2D1_SIZE_U size = D2D1::SizeU(max(rc.right, 1L), max(rc.bottom, 1L));
    if (FAILED(_pD2DFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(_hwnd, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY), &_pRenderTarget)))
    {
        return FALSE;
    }

    const FLOAT dpi = static_cast<FLOAT>(msimeui::compat::GetWindowDpi(_hwnd));
    _pRenderTarget->SetDpi(dpi, dpi);

    return TRUE;
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextInputCtrl::DiscardDeviceResources()
{
    if (_pRenderTarget)
    {
        _pRenderTarget->Release();
        _pRenderTarget = NULL;
    }

    if (_pDWriteFactory)
    {
        _pDWriteFactory->Release();
        _pDWriteFactory = NULL;
    }

    if (_pD2DFactory)
    {
        _pD2DFactory->Release();
        _pD2DFactory = NULL;
    }
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextInputCtrl::OnPaint()
{
    if (!CreateDeviceResources())
    {
        return;
    }

    const FLOAT dpi = static_cast<FLOAT>(msimeui::compat::GetWindowDpi(_hwnd));
    _pRenderTarget->SetDpi(dpi, dpi);
    _pRenderTarget->BeginDraw();
    _pRenderTarget->Clear(D2D1::ColorF(D2D1::ColorF::White));
    _editor.Render(_pRenderTarget);
    const HRESULT hr = _pRenderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        DiscardDeviceResources();
    }
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextInputCtrl::OnSize()
{
    if (_pRenderTarget)
    {
        RECT rc = {};
        GetClientRect(_hwnd, &rc);
        const FLOAT dpi = static_cast<FLOAT>(msimeui::compat::GetWindowDpi(_hwnd));
        _pRenderTarget->SetDpi(dpi, dpi);
        _pRenderTarget->Resize(D2D1::SizeU(max(rc.right, 1L), max(rc.bottom, 1L)));
    }

    _editor.UpdateLayout();
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextInputCtrl::OnKeyDown(WPARAM wParam, LPARAM lParam)
{
    const bool ctrlPressed = (GetKeyState(VK_CONTROL) & 0x80) != 0;
    UINT nSelStart;
    UINT nSelEnd;
    switch (0xff & wParam)
    {
    case VK_LEFT:
        if (GetKeyState(VK_SHIFT) & 0x80)
        {
            nSelStart = _editor.GetSelectionStart();
            nSelEnd = _editor.GetSelectionEnd();
            if (nSelStart > 0)
            {
                _editor.MoveSelection(nSelStart - 1, nSelEnd);
            }
        }
        else
        {
            _editor.MoveSelectionPrev();
        }
        InvalidateRect(_hwnd, NULL, FALSE);
        break;

    case VK_RIGHT:
        if (GetKeyState(VK_SHIFT) & 0x80)
        {
            nSelStart = _editor.GetSelectionStart();
            nSelEnd = _editor.GetSelectionEnd();
            _editor.MoveSelection(nSelStart, nSelEnd + 1);
        }
        else
        {
            _editor.MoveSelectionNext();
        }
        InvalidateRect(_hwnd, NULL, FALSE);
        break;

    case VK_UP:
        _editor.MoveSelectionUpDown(TRUE);
        InvalidateRect(_hwnd, NULL, FALSE);
        break;

    case VK_DOWN:
        _editor.MoveSelectionUpDown(FALSE);
        InvalidateRect(_hwnd, NULL, FALSE);
        break;

    case VK_HOME:
        _editor.MoveSelectionToLineFirstEnd(TRUE);
        InvalidateRect(_hwnd, NULL, FALSE);
        break;

    case VK_END:
        _editor.MoveSelectionToLineFirstEnd(FALSE);
        InvalidateRect(_hwnd, NULL, FALSE);
        break;

    case VK_DELETE:
        nSelStart = _editor.GetSelectionStart();
        nSelEnd = _editor.GetSelectionEnd();
        if (nSelStart == nSelEnd)
        {
            if (ctrlPressed)
            {
                _editor.DeleteNextWord();
            }
            else
            {
                _editor.DeleteAtSelection(FALSE);
            }
        }
        else
        {
            _editor.DeleteSelection();
        }
        InvalidateRect(_hwnd, NULL, FALSE);
        break;

    case VK_BACK:
        nSelStart = _editor.GetSelectionStart();
        nSelEnd = _editor.GetSelectionEnd();
        if (nSelStart == nSelEnd)
        {
            if (ctrlPressed)
            {
                _editor.DeletePreviousWord();
            }
            else
            {
                _editor.DeleteAtSelection(TRUE);
            }
        }
        else
        {
            _editor.DeleteSelection();
        }
        InvalidateRect(_hwnd, NULL, FALSE);
        break;
    }
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextInputCtrl::OnCreate(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
    if (!InitializeTsfTextServices(g_hInst))
    {
        return;
    }

    _editor.SetWnd(hwnd);
    _editor.SetFont(&_lfCurrentFont);
    _editor.InitTSF();
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextInputCtrl::OnDestroy()
{
    _editor.UninitTSF();
    DiscardDeviceResources();
    UninitializeTsfTextServices();
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextInputCtrl::OnSetFocus(WPARAM wParam, LPARAM lParam)
{
    _editor.SetFocusDocumentMgr();
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextInputCtrl::OnLButtonDown(WPARAM wParam, LPARAM lParam)
{
    SetFocus(_hwnd);

    if (AleartMouseSink(lParam))
    {
        return;
    }

    _editor.TerminateCompositionString();

    POINT pt;
    _uSelDragStart = (UINT)-1;
    pt.x = GET_X_LPARAM(lParam);
    pt.y = GET_Y_LPARAM(lParam);
    if (_editor.MoveSelectionAtPoint(pt))
    {
        InvalidateRect(_hwnd, NULL, FALSE);
        _uSelDragStart = _editor.GetSelectionStart();
    }
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextInputCtrl::OnLButtonUp(WPARAM wParam, LPARAM lParam)
{
    if (AleartMouseSink(lParam))
    {
        return;
    }

    UINT nSelStart = _editor.GetSelectionStart();
    UINT nSelEnd = _editor.GetSelectionEnd();
    POINT pt;
    pt.x = GET_X_LPARAM(lParam);
    pt.y = GET_Y_LPARAM(lParam);
    if (_editor.MoveSelectionAtPoint(pt))
    {
        UINT nNewSelStart = _editor.GetSelectionStart();
        UINT nNewSelEnd = _editor.GetSelectionEnd();
        _editor.MoveSelection(min(nSelStart, nNewSelStart), max(nSelEnd, nNewSelEnd));
        InvalidateRect(_hwnd, NULL, FALSE);
    }
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextInputCtrl::OnRButtonDown(WPARAM wParam, LPARAM lParam)
{
    if (AleartMouseSink(lParam))
    {
        return;
    }
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextInputCtrl::OnRButtonUp(WPARAM wParam, LPARAM lParam)
{
    if (AleartMouseSink(lParam))
    {
        return;
    }
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextInputCtrl::OnMouseMove(WPARAM wParam, LPARAM lParam)
{
    if (AleartMouseSink(lParam))
    {
        return;
    }

    if (wParam & MK_LBUTTON)
    {
        POINT pt;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        if (_editor.MoveSelectionAtPoint(pt))
        {
            UINT nNewSelStart = _editor.GetSelectionStart();
            UINT nNewSelEnd = _editor.GetSelectionEnd();
            _editor.MoveSelection(min(_uSelDragStart, nNewSelStart), max(_uSelDragStart, nNewSelEnd));
            InvalidateRect(_hwnd, NULL, FALSE);
        }
    }
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextInputCtrl::SetFont(HWND hwndParent)
{
    CHOOSEFONT cf;
    LOGFONT lf = _lfCurrentFont;

    cf.lStructSize = sizeof(CHOOSEFONT);
    cf.hwndOwner = hwndParent;
    cf.lpLogFont = &lf;
    cf.Flags = CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS;
    cf.rgbColors = RGB(0, 0, 0);
    cf.lCustData = 0;
    cf.lpfnHook = NULL;
    cf.lpTemplateName = NULL;
    cf.hInstance = NULL;
    cf.lpszStyle = NULL;
    cf.nFontType = SCREEN_FONTTYPE;
    cf.nSizeMin = 0;
    cf.nSizeMax = 0;

    if (ChooseFont(&cf))
    {
        _lfCurrentFont = lf;
        _editor.SetFont(&_lfCurrentFont);
        InvalidateRect(_hwnd, NULL, FALSE);
    }
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

BOOL CTextInputCtrl::AleartMouseSink(LPARAM lParam)
{
    POINT pt;
    pt.x = GET_X_LPARAM(lParam);
    pt.y = GET_Y_LPARAM(lParam);

    BOOL bEaten = FALSE;
    DWORD dwBtnState = 0;

    if (GetKeyState(VK_LBUTTON) & 0x80)
        dwBtnState = MK_LBUTTON;
    else if (GetKeyState(VK_RBUTTON) & 0x80)
        dwBtnState = MK_RBUTTON;

    _editor.AleartMouseSink(pt, dwBtnState, &bEaten);
    return bEaten;
}
