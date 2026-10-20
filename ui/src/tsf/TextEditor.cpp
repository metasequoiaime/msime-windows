#include <string>
#include <sstream>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include "TextEditor.h"
#include "../DebugLog.h"
#include "msimeui/Compat.h"

extern ITfThreadMgr *g_pThreadMgr;
extern TfClientId g_TfClientId;

namespace
{
std::wstring Utf8ToWide(const std::string &text)
{
    if (text.empty())
    {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::filesystem::path FindDictionaryPath()
{
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);

    std::vector<std::filesystem::path> roots;
    roots.emplace_back(std::filesystem::current_path());
    roots.emplace_back(std::filesystem::path(modulePath).parent_path());

    for (const auto &root : roots)
    {
        for (auto current = root; !current.empty(); current = current.parent_path())
        {
            const auto candidate = current / "assets" / "dict.txt";
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }

            if (current == current.root_path())
            {
                break;
            }
        }
    }

    return {};
}

struct WordDictionary
{
    std::unordered_set<std::wstring> entries;
    size_t maxWordLength = 1;
};

const WordDictionary &GetWordDictionary()
{
    static WordDictionary dictionary;
    static std::once_flag once;
    std::call_once(once, []() {
        const auto path = FindDictionaryPath();
        if (path.empty())
        {
            msimeui::DebugLog(
                "Word dictionary not found, Ctrl+Backspace/Delete will fall back to char/token boundaries");
            return;
        }

        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            msimeui::DebugLog("Failed to open word dictionary");
            return;
        }

        std::string line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            const std::wstring word = Utf8ToWide(line);
            if (word.empty())
            {
                continue;
            }

            dictionary.maxWordLength = std::max(dictionary.maxWordLength, word.size());
            dictionary.entries.insert(word);
        }

        MSIMEUI_DEBUG_LOG("Loaded word dictionary entries=" << dictionary.entries.size()
                                                            << " maxWordLength=" << dictionary.maxWordLength);
    });

    return dictionary;
}

bool IsAsciiWordChar(wchar_t ch)
{
    return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'_';
}

bool IsCjkLikeChar(wchar_t ch)
{
    return ch >= 0x2E80;
}

bool IsDeletablePunctuation(wchar_t ch)
{
    return !std::iswspace(ch) && !IsAsciiWordChar(ch) && !IsCjkLikeChar(ch);
}

UINT FindPreviousWordStart(std::wstring_view text, UINT caret)
{
    if (caret == 0 || text.empty())
    {
        return caret;
    }

    UINT cursor = caret;
    while (cursor > 0 && std::iswspace(text[cursor - 1]))
    {
        cursor--;
    }
    if (cursor == 0)
    {
        return 0;
    }

    const wchar_t previous = text[cursor - 1];
    if (IsAsciiWordChar(previous))
    {
        UINT start = cursor;
        while (start > 0 && IsAsciiWordChar(text[start - 1]))
        {
            start--;
        }
        return start;
    }

    if (IsDeletablePunctuation(previous))
    {
        UINT start = cursor;
        while (start > 0 && IsDeletablePunctuation(text[start - 1]))
        {
            start--;
        }
        return start;
    }

    const WordDictionary &dictionary = GetWordDictionary();
    const UINT earliest = cursor > dictionary.maxWordLength ? cursor - static_cast<UINT>(dictionary.maxWordLength) : 0;
    UINT bestStart = cursor - 1;
    size_t bestLength = 1;
    for (UINT start = earliest; start < cursor; ++start)
    {
        const size_t length = static_cast<size_t>(cursor - start);
        if (length <= bestLength)
        {
            continue;
        }

        if (dictionary.entries.find(std::wstring(text.substr(start, length))) != dictionary.entries.end())
        {
            bestStart = start;
            bestLength = length;
        }
    }
    return bestStart;
}

UINT FindNextWordEnd(std::wstring_view text, UINT caret)
{
    if (caret >= text.size())
    {
        return caret;
    }

    UINT cursor = caret;
    while (cursor < text.size() && std::iswspace(text[cursor]))
    {
        cursor++;
    }
    if (cursor >= text.size())
    {
        return static_cast<UINT>(text.size());
    }

    const wchar_t current = text[cursor];
    if (IsAsciiWordChar(current))
    {
        UINT end = cursor;
        while (end < text.size() && IsAsciiWordChar(text[end]))
        {
            end++;
        }
        return end;
    }

    if (IsDeletablePunctuation(current))
    {
        UINT end = cursor;
        while (end < text.size() && IsDeletablePunctuation(text[end]))
        {
            end++;
        }
        return end;
    }

    const WordDictionary &dictionary = GetWordDictionary();
    const UINT latest =
        std::min<UINT>(static_cast<UINT>(text.size()), cursor + static_cast<UINT>(dictionary.maxWordLength));
    UINT bestEnd = cursor + 1;
    size_t bestLength = 1;
    for (UINT end = cursor + 1; end <= latest; ++end)
    {
        const size_t length = static_cast<size_t>(end - cursor);
        if (length <= bestLength)
        {
            continue;
        }

        if (dictionary.entries.find(std::wstring(text.substr(cursor, length))) != dictionary.entries.end())
        {
            bestEnd = end;
            bestLength = length;
        }
    }
    return bestEnd;
}
} // namespace

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextEditor::MoveSelection(UINT nSelStart, UINT nSelEnd)
{
    UINT nTextLength = GetTextLength();
    if (nSelStart >= nTextLength)
        nSelStart = nTextLength;

    if (nSelEnd >= nTextLength)
        nSelEnd = nTextLength;

    _nSelStart = nSelStart;
    _nSelEnd = nSelEnd;
    _layout.ResetCaretBlink();
    _layout.EnsureCaretVisible(_nSelEnd);

    _pTextStore->OnSelectionChange();
}

void CTextEditor::SelectAll()
{
    MoveSelection(0, GetTextLength());
}

std::wstring CTextEditor::GetSelectedText() const
{
    const UINT selectionStart = min(_nSelStart, _nSelEnd);
    const UINT selectionEnd = max(_nSelStart, _nSelEnd);
    if (selectionStart == selectionEnd || !GetTextBuffer())
    {
        return {};
    }

    return std::wstring(GetTextBuffer() + selectionStart, GetTextBuffer() + selectionEnd);
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextEditor::MoveSelectionNext()
{
    UINT nTextLength = GetTextLength();
    if (_nSelStart == _nSelEnd && _nSelEnd < nTextLength)
        _nSelEnd++;

    _nSelStart = _nSelEnd;
    _layout.ResetCaretBlink();
    _pTextStore->OnSelectionChange();
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextEditor::MoveSelectionPrev()
{
    if (_nSelStart == _nSelEnd && _nSelStart > 0)
        _nSelStart--;

    _nSelEnd = _nSelStart;
    _layout.ResetCaretBlink();
    _pTextStore->OnSelectionChange();
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

BOOL CTextEditor::MoveSelectionAtPoint(POINT pt)
{
    const UINT nSel = _layout.InsertionIndexFromPoint(pt);
    MoveSelection(nSel, nSel);
    return TRUE;
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

BOOL CTextEditor::MoveSelectionUpDown(BOOL bUp)
{
    RECT rc;
    if (!_layout.RectFromCharPos(_nSelStart, &rc))
        return FALSE;

    POINT pt;
    pt.x = rc.left;
    if (bUp)
    {
        pt.y = rc.top - ((rc.bottom - rc.top) / 2);
        // rc is scrolled; the first line is the one above which nothing is left.
        const FLOAT scrollPixels =
            _layout.GetVerticalScroll() * static_cast<FLOAT>(msimeui::compat::GetWindowDpi(_hwnd)) / 96.0f;
        if (pt.y + static_cast<LONG>(scrollPixels) < 0)
            return FALSE;
    }
    else
    {
        pt.y = rc.bottom + ((rc.bottom - rc.top) / 2);
    }

    return MoveSelectionAtPoint(pt);
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

BOOL CTextEditor::MoveSelectionToLineFirstEnd(BOOL bFirst)
{
    BOOL bRet = FALSE;
    UINT nSel;

    if (bFirst)
    {
        nSel = _layout.FineFirstEndCharPosInLine(_nSelStart, TRUE);
    }
    else
    {
        nSel = _layout.FineFirstEndCharPosInLine(_nSelEnd, FALSE);
    }

    if (nSel != (UINT)-1)
    {
        MoveSelection(nSel, nSel);
        bRet = TRUE;
    }
    return bRet;
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

BOOL CTextEditor::InsertAtSelection(LPCWSTR psz)
{
    LONG lOldSelEnd = _nSelEnd;
    if (!RemoveText(_nSelStart, _nSelEnd - _nSelStart))
        return FALSE;

    if (!InsertText(_nSelStart, psz, lstrlen(psz)))
        return FALSE;

    _nSelStart += lstrlen(psz);
    _nSelEnd = _nSelStart;
    _layout.ResetCaretBlink();
    UpdateLayout();
    _layout.EnsureCaretVisible(_nSelEnd);

    _pTextStore->OnTextChange(_nSelStart, lOldSelEnd, _nSelEnd);
    _pTextStore->OnSelectionChange();
    return TRUE;
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

BOOL CTextEditor::DeleteAtSelection(BOOL fBack)
{
    // Check if we are in composition state
    if (_pTextStore->GetCurrentCompositionView() && GetTextLength() == 0)
    {
        TerminateCompositionString();
        return TRUE;
    }

    if (!fBack && (_nSelEnd < GetTextLength()))
    {
        if (!RemoveText(_nSelEnd, 1))
            return FALSE;

        _layout.ResetCaretBlink();
        UpdateLayout();
        _layout.EnsureCaretVisible(_nSelEnd);
        _pTextStore->OnTextChange(_nSelEnd, _nSelEnd + 1, _nSelEnd);
    }

    if (fBack && (_nSelStart > 0))
    {
        if (!RemoveText(_nSelStart - 1, 1))
            return FALSE;

        _nSelStart--;
        _nSelEnd = _nSelStart;
        _layout.ResetCaretBlink();
        UpdateLayout();
        _layout.EnsureCaretVisible(_nSelEnd);

        _pTextStore->OnTextChange(_nSelStart, _nSelStart + 1, _nSelStart);
        _pTextStore->OnSelectionChange();
    }

    return TRUE;
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

BOOL CTextEditor::DeleteSelection()
{
    ULONG nSelOldEnd = _nSelEnd;
    RemoveText(_nSelStart, _nSelEnd - _nSelStart);

    _nSelEnd = _nSelStart;
    _layout.ResetCaretBlink();
    UpdateLayout();
    _layout.EnsureCaretVisible(_nSelEnd);

    _pTextStore->OnTextChange(_nSelStart, nSelOldEnd, _nSelStart);
    _pTextStore->OnSelectionChange();

    return TRUE;
}

BOOL CTextEditor::DeletePreviousWord()
{
    if (_nSelStart != _nSelEnd)
    {
        return DeleteSelection();
    }

    if (_nSelStart == 0 || !GetTextBuffer())
    {
        return TRUE;
    }

    const std::wstring_view text(GetTextBuffer(), GetTextLength());
    const UINT deleteStart = FindPreviousWordStart(text, _nSelStart);

    if (deleteStart == _nSelStart)
    {
        return TRUE;
    }

    const ULONG oldSelectionEnd = _nSelEnd;
    if (!RemoveText(deleteStart, _nSelStart - deleteStart))
    {
        return FALSE;
    }

    _nSelStart = deleteStart;
    _nSelEnd = deleteStart;
    _layout.ResetCaretBlink();
    UpdateLayout();
    _layout.EnsureCaretVisible(_nSelEnd);

    _pTextStore->OnTextChange(deleteStart, oldSelectionEnd, deleteStart);
    _pTextStore->OnSelectionChange();
    return TRUE;
}

BOOL CTextEditor::DeleteNextWord()
{
    if (_nSelStart != _nSelEnd)
    {
        return DeleteSelection();
    }

    if (!GetTextBuffer() || _nSelEnd >= GetTextLength())
    {
        return TRUE;
    }

    const std::wstring_view text(GetTextBuffer(), GetTextLength());
    const UINT deleteEnd = FindNextWordEnd(text, _nSelEnd);
    if (deleteEnd == _nSelEnd)
    {
        return TRUE;
    }

    const ULONG oldSelectionEnd = _nSelEnd;
    if (!RemoveText(_nSelEnd, deleteEnd - _nSelEnd))
    {
        return FALSE;
    }

    _layout.ResetCaretBlink();
    UpdateLayout();
    _layout.EnsureCaretVisible(_nSelEnd);

    _pTextStore->OnTextChange(_nSelEnd, oldSelectionEnd + (deleteEnd - _nSelEnd), _nSelEnd);
    _pTextStore->OnSelectionChange();
    return TRUE;
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

BOOL CTextEditor::InitializeRenderResources(IDWriteFactory *pDWriteFactory)
{
    return _layout.Initialize(pDWriteFactory);
}

void CTextEditor::SetFont(const LOGFONT *plf)
{
    if (!plf)
    {
        return;
    }

    _lfCurrentFont = *plf;
    _fHasFont = TRUE;
    UpdateLayout();
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextEditor::Render(ID2D1RenderTarget *pRenderTarget)
{
    if (!_fHasFont)
    {
        return;
    }

    _layout.Render(pRenderTarget, DisplayText(), GetTextLength(), _nSelStart, _nSelEnd, _pCompositionRenderInfo,
                   _nCompositionRenderInfo);
}

const WCHAR *CTextEditor::DisplayText()
{
    if (!_maskCharacter)
    {
        return GetTextBuffer();
    }
    _maskedText.assign(GetTextLength(), _maskCharacter);
    return _maskedText.c_str();
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextEditor::UpdateLayout()
{
    if (!_hwnd || !_fHasFont)
    {
        return;
    }

    const FLOAT dpi = static_cast<FLOAT>(msimeui::compat::GetWindowDpi(_hwnd));
    const LONG width = max(_rcHost.right - _rcHost.left, 1L);
    const LONG height = max(_rcHost.bottom - _rcHost.top, 1L);
    const auto layout = [&](LONG gutter) {
        _layout.Layout(DisplayText(), GetTextLength(), &_lfCurrentFont, static_cast<FLOAT>(width),
                       static_cast<FLOAT>(height), dpi, dpi, static_cast<FLOAT>(_rcContentPadding.left),
                       static_cast<FLOAT>(_rcContentPadding.top), static_cast<FLOAT>(_rcContentPadding.right + gutter),
                       static_cast<FLOAT>(_rcContentPadding.bottom), _singleLine);
    };
    layout(0);
    if (!_singleLine && _scrollbarGutterPixels > 0 && _layout.GetMaxVerticalScroll() > 0.0f)
    {
        layout(_scrollbarGutterPixels);
    }
    const BOOL moved = GetTextLength() != _laidOutLength || _nSelEnd != _laidOutSelEnd;
    _laidOutLength = GetTextLength();
    _laidOutSelEnd = _nSelEnd;
    _layout.EnsureCaretVisible(_nSelEnd, moved);
}

void CTextEditor::NotifyLayoutChange()
{
    if (_pTextStore)
    {
        _pTextStore->OnLayoutChange();
    }
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

BOOL CTextEditor::InitTSF()
{

    _pTextStore = new CTextStore(this);
    if (!_pTextStore)
    {
        return FALSE;
    }

    if (FAILED(g_pThreadMgr->CreateDocumentMgr(&_pDocumentMgr)))
    {
        return FALSE;
    }

    if (FAILED(_pDocumentMgr->CreateContext(g_TfClientId, 0, (ITextStoreACP *)_pTextStore, &_pInputContext,
                                            &_ecTextStore)))
    {
        return FALSE;
    }

    if (FAILED(_pDocumentMgr->Push(_pInputContext)))
    {
        return FALSE;
    }

    _pTextEditSink = new CTextEditSink(this);
    if (!_pTextEditSink)
    {
        return FALSE;
    }

    _pTextEditSink->_Advise(_pInputContext);

    return TRUE;
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

BOOL CTextEditor::UninitTSF()
{
    if (_pTextEditSink)
    {
        _pTextEditSink->_Unadvise();
        _pTextEditSink->Release();
        _pTextEditSink = NULL;
    }

    if (_pDocumentMgr)
    {
        _pDocumentMgr->Pop(TF_POPF_ALL);
    }

    if (_pInputContext)
    {
        _pInputContext->Release();
        _pInputContext = NULL;
    }

    if (_pDocumentMgr)
    {
        _pDocumentMgr->Release();
        _pDocumentMgr = NULL;
    }

    if (_pTextStore)
    {
        _pTextStore->Release();
        _pTextStore = NULL;
    }

    return TRUE;
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextEditor::SetFocusDocumentMgr()
{
    if (_pDocumentMgr)
    {
        const HRESULT hr = g_pThreadMgr->SetFocus(_pDocumentMgr);

        MSIMEUI_DEBUG_LOG("CTextEditor::SetFocusDocumentMgr editor=" << this << " docMgr=" << _pDocumentMgr
                                                                     << " hwnd=" << GetWnd() << " hr=0x" << std::hex
                                                                     << static_cast<unsigned long>(hr));
    }
}

void CTextEditor::ClearFocusDocumentMgr()
{
    if (!_pDocumentMgr)
    {
        return;
    }

    const HRESULT hr = g_pThreadMgr->SetFocus(nullptr);

    MSIMEUI_DEBUG_LOG("CTextEditor::ClearFocusDocumentMgr editor=" << this << " docMgr=" << _pDocumentMgr
                                                                   << " hwnd=" << GetWnd() << " hr=0x" << std::hex
                                                                   << static_cast<unsigned long>(hr));
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextEditor::ClearCompositionRenderInfo()
{
    if (_pCompositionRenderInfo)
    {
        LocalFree(_pCompositionRenderInfo);
        _pCompositionRenderInfo = NULL;
        _nCompositionRenderInfo = 0;
    }
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

BOOL CTextEditor::AddCompositionRenderInfo(int nStart, int nEnd, TF_DISPLAYATTRIBUTE *pda)
{
    if (_pCompositionRenderInfo)
    {
        void *pvNew =
            LocalReAlloc(_pCompositionRenderInfo, (_nCompositionRenderInfo + 1) * sizeof(COMPOSITIONRENDERINFO),
                         LMEM_MOVEABLE | LMEM_ZEROINIT);
        if (!pvNew)
            return FALSE;

        _pCompositionRenderInfo = (COMPOSITIONRENDERINFO *)pvNew;
    }
    else
    {
        _pCompositionRenderInfo =
            (COMPOSITIONRENDERINFO *)LocalAlloc(LPTR, (_nCompositionRenderInfo + 1) * sizeof(COMPOSITIONRENDERINFO));
        if (!_pCompositionRenderInfo)
            return FALSE;
    }
    _pCompositionRenderInfo[_nCompositionRenderInfo].nStart = nStart;
    _pCompositionRenderInfo[_nCompositionRenderInfo].nEnd = nEnd;
    _pCompositionRenderInfo[_nCompositionRenderInfo].da = *pda;
    _nCompositionRenderInfo++;

    return TRUE;
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextEditor::TerminateCompositionString()
{
    if (_pTextStore->GetCurrentCompositionView())
    {
        ITfContextOwnerCompositionServices *pCompositionServices;
        if (_pInputContext->QueryInterface(IID_ITfContextOwnerCompositionServices, (void **)&pCompositionServices) ==
            S_OK)
        {
            pCompositionServices->TerminateComposition(_pTextStore->GetCurrentCompositionView());
            pCompositionServices->Release();
        }

        // Clear composition render info
        ClearCompositionRenderInfo();
        InvalidateRect();
    }
}

//----------------------------------------------------------------
//
//
//
//----------------------------------------------------------------

void CTextEditor::AleartMouseSink(POINT pt, DWORD dwBtnState, BOOL *pbEaten)
{
    UINT nSel = _layout.ExactCharPosFromPoint(pt);
    if (nSel == (UINT)-1)
    {
        return;
    }

    RECT rc;
    if (!_layout.RectFromCharPos(nSel, &rc))
    {
        return;
    }

    int nPos = (pt.x - rc.left) * 4 / (rc.right - rc.left) + 2;
    UINT uEdge = nSel + (nPos / 4);
    UINT uQuadrant = nPos % 4;

    _pTextStore->OnMouseEvent(uEdge, uQuadrant, dwBtnState, pbEaten);
}
