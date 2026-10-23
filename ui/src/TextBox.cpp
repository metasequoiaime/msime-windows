#include "msimeui/Controls.h"

#include "DebugLog.h"
#include "msimeui/DeviceResources.h"
#include "msimeui/Theme.h"
#include "msimeui/Window.h"

#include "TextEditor.h"
#include "TsfTextServices.h"

#include <algorithm>
#include <cmath>
#include <commdlg.h>
#include <wrl/client.h>

namespace msimeui
{
using Microsoft::WRL::ComPtr;
using std::max;
using std::min;

namespace
{
constexpr UINT_PTR kCaretTimerId = 1;

RectF InsetRectF(const RectF &rect, float inset)
{
    const float width = std::max(rect.width - inset * 2.0f, 0.0f);
    const float height = std::max(rect.height - inset * 2.0f, 0.0f);
    return {rect.x + inset, rect.y + inset, width, height};
}

bool PointInRect(const RectF &rect, const PointF &point)
{
    return point.x >= rect.x && point.x < (rect.x + rect.width) && point.y >= rect.y &&
           point.y < (rect.y + rect.height);
}

bool PointInRoundedRect(const RectF &rect, float radius, const PointF &point)
{
    if (!PointInRect(rect, point))
    {
        return false;
    }

    const float clampedRadius = std::min(radius, std::min(rect.width, rect.height) * 0.5f);
    if (clampedRadius <= 0.0f)
    {
        return true;
    }

    const float left = rect.x;
    const float top = rect.y;
    const float right = rect.x + rect.width;
    const float bottom = rect.y + rect.height;

    if ((point.x >= left + clampedRadius && point.x < right - clampedRadius) ||
        (point.y >= top + clampedRadius && point.y < bottom - clampedRadius))
    {
        return true;
    }

    const float centerX = point.x < left + clampedRadius ? left + clampedRadius : right - clampedRadius;
    const float centerY = point.y < top + clampedRadius ? top + clampedRadius : bottom - clampedRadius;
    const float dx = point.x - centerX;
    const float dy = point.y - centerY;
    return (dx * dx + dy * dy) <= (clampedRadius * clampedRadius);
}

bool IsCtrlPressed()
{
    return (GetKeyState(VK_CONTROL) & 0x80) != 0;
}

bool CopyTextToClipboard(HWND hwnd, const std::wstring &text)
{
    if (!hwnd || !OpenClipboard(hwnd))
    {
        return false;
    }

    const auto closeClipboard = []() { CloseClipboard(); };

    EmptyClipboard();

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!global)
    {
        closeClipboard();
        return false;
    }

    void *buffer = GlobalLock(global);
    if (!buffer)
    {
        GlobalFree(global);
        closeClipboard();
        return false;
    }

    memcpy(buffer, text.c_str(), bytes);
    GlobalUnlock(global);

    if (!SetClipboardData(CF_UNICODETEXT, global))
    {
        GlobalFree(global);
        closeClipboard();
        return false;
    }

    closeClipboard();
    return true;
}

std::wstring ReadClipboardText(HWND hwnd)
{
    if (!hwnd || !OpenClipboard(hwnd))
    {
        return {};
    }

    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data)
    {
        CloseClipboard();
        return {};
    }

    const wchar_t *buffer = static_cast<const wchar_t *>(GlobalLock(data));
    if (!buffer)
    {
        CloseClipboard();
        return {};
    }

    std::wstring text(buffer);
    GlobalUnlock(data);
    CloseClipboard();
    return text;
}
} // namespace

TextBox::TextBox(float height, std::wstring placeholder)
    : preferredHeight_(height), placeholder_(std::move(placeholder))
{
    editor_ = new CTextEditor();
    editor_->SetTextChangedCallback([this]() {
        if (onTextChanged_ && !suppressTextChanged_ && !editor_->IsComposing())
        {
            const auto handler = onTextChanged_;
            handler(GetText());
        }
        InvalidateVisual();
    });

    HFONT defaultFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    GetObject(defaultFont, sizeof(LOGFONT), &font_);
    font_.lfHeight = -18;
    wcscpy_s(font_.lfFaceName, LF_FACESIZE, ThemeManager::GetCurrent().textInputFontFamily.c_str());
}

TextBox::~TextBox()
{
    if (tsfInitialized_)
    {
        editor_->UninitTSF();
        UninitializeTsfTextServices();
    }

    delete editor_;
    editor_ = nullptr;
}

void TextBox::SetAppearance(Appearance appearance)
{
    appearance_ = std::move(appearance);
    ApplyFont();
    RefreshEditorLayout();
    InvalidateVisual();
}

void TextBox::SetText(const std::wstring &text)
{
    if (!editor_)
    {
        return;
    }
    if (!tsfInitialized_)
    {
        // The editor reports edits to its TSF store, which only exists after Attach.
        pendingText_ = text;
        hasPendingText_ = true;
        return;
    }
    hasPendingText_ = false;
    if (GetText() == text)
    {
        return;
    }
    suppressTextChanged_ = true;
    editor_->TerminateCompositionString();
    editor_->SelectAll();
    if (text.empty())
    {
        if (editor_->HasSelection())
        {
            editor_->DeleteSelection();
        }
    }
    else
    {
        editor_->InsertAtSelection(text.c_str());
    }
    editor_->MoveSelection(0, 0);
    // Inserting restarts the caret blink; an unfocused box must not show a caret.
    editor_->SetCaretVisible(focused_ ? TRUE : FALSE);
    suppressTextChanged_ = false;
    InvalidateVisual();
}

void TextBox::SetPasswordMode(bool password)
{
    if (password_ == password)
    {
        return;
    }
    password_ = password;
    if (editor_)
    {
        editor_->SetMaskCharacter(password ? L'\x2022' : 0);
    }
    InvalidateVisual();
}

void TextBox::SetMultiline(bool multiline)
{
    multiline_ = multiline ? 1 : 0;
    RefreshEditorLayout();
}

void TextBox::SetEnabled(bool enabled)
{
    if (enabled_ == enabled)
    {
        return;
    }
    enabled_ = enabled;
    if (!enabled && focused_ && window_)
    {
        window_->FocusVisual(nullptr);
    }
    InvalidateVisual();
}

void TextBox::SetOnSubmit(std::function<void()> handler)
{
    onSubmit_ = std::move(handler);
}

void TextBox::SetPreferredHeight(float height)
{
    if (preferredHeight_ == height)
    {
        return;
    }
    preferredHeight_ = height;
    InvalidateMeasure();
}

bool TextBox::IsMultiline() const
{
    return multiline_ < 0 ? preferredHeight_ > 100.0f : multiline_ == 1;
}

void TextBox::ApplyFont()
{
    const std::wstring &family =
        appearance_.fontFamily.empty() ? ThemeManager::GetCurrent().textInputFontFamily : appearance_.fontFamily;
    wcscpy_s(font_.lfFaceName, LF_FACESIZE, family.c_str());
    font_.lfWeight = static_cast<LONG>(appearance_.fontWeight);
    if (editor_ && window_)
    {
        font_.lfHeight = -static_cast<LONG>(std::lround(DipsToPixels(fontSizeDips_, window_->GetDpi())));
        editor_->SetLetterSpacing(appearance_.letterSpacing);
        editor_->SetCssLineHeight(appearance_.lineHeight);
        editor_->SetFont(&font_);
    }
}

void TextBox::RefreshEditorLayout()
{
    if (!editor_ || !window_)
    {
        return;
    }
    editor_->SetHostRect(ComputeEditorHostRect());
    editor_->SetSingleLine(IsMultiline() ? FALSE : TRUE);
    editor_->SetContentPadding(ComputeEditorContentPadding());
    editor_->SetScrollbarGutter(
        static_cast<LONG>(std::lround(DipsToPixels(appearance_.scrollbarGutter, window_->GetDpi()))));
    editor_->UpdateLayout();
    editor_->NotifyLayoutChange();
}

TextBox::ScrollInfo TextBox::GetScrollInfo() const
{
    ScrollInfo info;
    if (!editor_ || !IsMultiline())
    {
        return info;
    }
    const ::CTextLayout *layout = editor_->GetLayout();
    info.offset = layout->GetVerticalScroll();
    info.maxOffset = layout->GetMaxVerticalScroll();
    info.contentHeight = layout->GetContentHeight();
    info.viewportHeight = layout->GetViewportHeight();
    return info;
}

bool TextBox::SetScrollOffset(float offset)
{
    if (!editor_ || !IsMultiline() || !editor_->SetVerticalScroll(offset))
    {
        return false;
    }
    InvalidateVisual();
    return true;
}

bool TextBox::OnMouseWheel(const POINT &point, short delta, WPARAM keyState)
{
    (void)point;
    (void)keyState;
    const ScrollInfo info = GetScrollInfo();
    if (info.maxOffset <= 0.0f)
    {
        return false;
    }
    // 100 DIPs a notch, the same step as the page.
    return SetScrollOffset(info.offset - static_cast<float>(delta) / WHEEL_DELTA * 100.0f);
}

SizeF TextBox::Measure(const SizeF &availableSize)
{
    return {availableSize.width, preferredHeight_};
}

void TextBox::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    MSIMEUI_DEBUG_LOG("TextBox::Arrange this=" << this << " preferredHeight=" << preferredHeight_ << " bounds=("
                                               << bounds_.x << "," << bounds_.y << "," << bounds_.width << ","
                                               << bounds_.height << ")");
    // Also runs when only the DPI changed: the editor works in pixels.
    RefreshEditorLayout();
}

void TextBox::Render(DeviceResources &deviceResources)
{
    if (!editor_)
    {
        return;
    }

    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    if (!EnsureInitialized(&deviceResources))
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    const auto pick = [](const D2D1_COLOR_F &color, const D2D1_COLOR_F &fallback) {
        return color.a > 0.0f ? color : fallback;
    };
    editor_->SetColors(pick(appearance_.text, theme.textPrimary), pick(appearance_.selection, theme.primary));
    if (chromeVisible_)
    {
        D2D1_COLOR_F border = pick(appearance_.border, theme.borderStrong);
        if (focused_)
        {
            border = pick(appearance_.focusBorder, theme.primaryFocusStrong);
        }
        else if (hovered_ && enabled_ && appearance_.hoverBorder.a > 0.0f)
        {
            border = appearance_.hoverBorder;
        }
        ID2D1SolidColorBrush *fillBrush = deviceResources.GetSolidColorBrush(pick(appearance_.fill, theme.surface));
        ID2D1SolidColorBrush *strokeBrush = deviceResources.GetSolidColorBrush(border);
        if (!fillBrush || !strokeBrush)
        {
            return;
        }

        const float stroke = appearance_.borderWidth;
        const float half = stroke * 0.5f;
        const float radius = appearance_.cornerRadius;
        target->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.width, bounds_.y + bounds_.height),
                              radius, radius),
            fillBrush);
        if (stroke > 0.0f)
        {
            target->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(bounds_.x + half, bounds_.y + half, bounds_.x + bounds_.width - half,
                                              bounds_.y + bounds_.height - half),
                                  std::max(radius - half, 0.0f), std::max(radius - half, 0.0f)),
                strokeBrush, stroke);
        }
    }

    RECT hostRectPixels = ComputeEditorHostRect();
    const float dpi = window_ ? window_->GetDpi() : 96.0f;
    const float hostX = PixelsToDips(static_cast<float>(hostRectPixels.left), dpi);
    const float hostY = PixelsToDips(static_cast<float>(hostRectPixels.top), dpi);
    const float contentWidth =
        PixelsToDips(static_cast<float>(std::max(hostRectPixels.right - hostRectPixels.left, 0L)), dpi);
    const float contentHeight =
        PixelsToDips(static_cast<float>(std::max(hostRectPixels.bottom - hostRectPixels.top, 0L)), dpi);

    D2D1_MATRIX_3X2_F oldTransform = {};
    target->GetTransform(&oldTransform);
    target->SetTransform(D2D1::Matrix3x2F::Translation(hostX, hostY) * oldTransform);
    target->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, contentWidth, contentHeight),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (!enabled_)
    {
        // Disabled boxes keep their text but read as inactive.
        editor_->SetColors(pick(appearance_.placeholder, theme.textSecondary),
                           pick(appearance_.selection, theme.primary));
    }
    editor_->Render(target);
    if (editor_->GetTextLength() == 0 && !placeholder_.empty())
    {
        const std::wstring &family =
            appearance_.fontFamily.empty() ? ThemeManager::GetCurrent().textInputFontFamily : appearance_.fontFamily;
        IDWriteTextFormat *placeholderFormat = deviceResources.GetTextFormat(
            family, placeholderFontSizeDips_, appearance_.fontWeight, DWRITE_TEXT_ALIGNMENT_LEADING,
            IsMultiline() ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            IsMultiline() ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        ID2D1SolidColorBrush *placeholderBrush =
            deviceResources.GetSolidColorBrush(pick(appearance_.placeholder, theme.textSecondary));
        if (placeholderFormat && placeholderBrush)
        {
            const RECT padding = ComputeEditorContentPadding();
            const float left = PixelsToDips(static_cast<float>(padding.left), dpi);
            const float top = PixelsToDips(static_cast<float>(padding.top), dpi);
            const float right = PixelsToDips(static_cast<float>(padding.right), dpi);
            const float bottom = PixelsToDips(static_cast<float>(padding.bottom), dpi);
            target->DrawTextW(placeholder_.c_str(), static_cast<UINT32>(placeholder_.size()), placeholderFormat,
                              D2D1::RectF(left, top, contentWidth - right, contentHeight - bottom), placeholderBrush,
                              D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    target->PopAxisAlignedClip();
    target->SetTransform(oldTransform);
}

void TextBox::Attach(Window *window)
{
    Visual::Attach(window);
    if (editor_)
    {
        editor_->SetWnd(window->GetHandle());
        ApplyFont();
        editor_->SetCaretVisible(FALSE);
        if (!tsfInitialized_)
        {
            tsfInitialized_ = InitializeTsfTextServices(window->GetInstance()) && editor_->InitTSF();
        }
        if (tsfInitialized_ && hasPendingText_)
        {
            SetText(pendingText_);
        }
        MSIMEUI_DEBUG_LOG("TextBox::Attach this=" << this << " hwnd=" << window->GetHandle()
                                                  << " tsfInitialized=" << tsfInitialized_);
    }
}

bool TextBox::HitTest(const PointF &point) const
{
    const RectF innerRect = InsetRectF(bounds_, focused_ ? 2.0f : 1.0f);
    return IsMultiline() ? PointInRoundedRect(innerRect, appearance_.cornerRadius - 1.0f, point)
                         : PointInRect(innerRect, point);
}

bool TextBox::IsFocusable() const
{
    return enabled_;
}

void TextBox::OnFocusChanged(bool focused)
{
    MSIMEUI_DEBUG_LOG("TextBox::OnFocusChanged this=" << this << " focused=" << focused
                                                      << " tsfInitialized=" << tsfInitialized_);

    if (!focused && editor_ && tsfInitialized_)
    {
        editor_->ClearFocusDocumentMgr();
    }

    focused_ = focused;
    if (editor_)
    {
        editor_->SetCaretVisible(focused_ ? TRUE : FALSE);
    }
    if (focused_ && editor_ && tsfInitialized_)
    {
        editor_->SetFocusDocumentMgr();
    }

    if (onFocusChanged_)
    {
        const auto handler = onFocusChanged_;
        handler(focused_);
    }

    InvalidateVisual();
}

bool TextBox::OnMouseDown(const POINT &point, WPARAM keyState)
{
    if (!editor_ || !enabled_)
    {
        return false;
    }

    POINT local = ToLocalPoint(point);
    MSIMEUI_DEBUG_LOG("TextBox::OnMouseDown this=" << this << " global=(" << point.x << "," << point.y << ") local=("
                                                   << local.x << "," << local.y << ")");
    if (AlertMouseSink(local, keyState))
    {
        return true;
    }

    editor_->TerminateCompositionString();
    dragSelectionStart_ = static_cast<UINT>(-1);
    if (editor_->MoveSelectionAtPoint(local))
    {
        dragSelectionStart_ = editor_->GetSelectionStart();
        InvalidateVisual();
    }
    return true;
}

bool TextBox::OnMouseUp(const POINT &point, WPARAM keyState)
{
    if (!editor_ || !enabled_)
    {
        return false;
    }

    POINT local = ToLocalPoint(point);
    if (AlertMouseSink(local, keyState))
    {
        return true;
    }

    const UINT selectionStart = editor_->GetSelectionStart();
    const UINT selectionEnd = editor_->GetSelectionEnd();
    if (editor_->MoveSelectionAtPoint(local))
    {
        const UINT newStart = editor_->GetSelectionStart();
        const UINT newEnd = editor_->GetSelectionEnd();
        editor_->MoveSelection(min(selectionStart, newStart), max(selectionEnd, newEnd));
        InvalidateVisual();
    }
    dragSelectionStart_ = static_cast<UINT>(-1);
    return true;
}

bool TextBox::OnMouseMove(const POINT &point, WPARAM keyState)
{
    if (!editor_ || !enabled_)
    {
        return false;
    }

    POINT local = ToLocalPoint(point);
    if (AlertMouseSink(local, keyState))
    {
        return true;
    }

    if ((keyState & MK_LBUTTON) && dragSelectionStart_ != static_cast<UINT>(-1))
    {
        if (editor_->MoveSelectionAtPoint(local))
        {
            const UINT newStart = editor_->GetSelectionStart();
            const UINT newEnd = editor_->GetSelectionEnd();
            editor_->MoveSelection(min(dragSelectionStart_, newStart), max(dragSelectionStart_, newEnd));
            InvalidateVisual();
        }
    }
    return true;
}

void TextBox::OnMouseEnter()
{
    hovered_ = true;
    if (appearance_.hoverBorder.a > 0.0f)
    {
        InvalidateVisual();
    }
}

void TextBox::OnMouseLeave()
{
    hovered_ = false;
    if (appearance_.hoverBorder.a > 0.0f)
    {
        InvalidateVisual();
    }
}

bool TextBox::OnKeyDown(WPARAM key, LPARAM lParam)
{
    (void)lParam;

    if (!editor_ || !enabled_)
    {
        return false;
    }

    UINT selectionStart = 0;
    UINT selectionEnd = 0;
    const bool ctrlPressed = IsCtrlPressed();

    if (ctrlPressed)
    {
        switch (0xff & key)
        {
        case 'A':
            editor_->SelectAll();
            InvalidateVisual();
            return true;

        case 'C':
            // A masked box never hands its secret to the clipboard.
            if (editor_->HasSelection() && !password_)
            {
                CopyTextToClipboard(window_ ? window_->GetHandle() : nullptr, editor_->GetSelectedText());
            }
            return true;

        case 'X':
            if (editor_->HasSelection() && !password_)
            {
                if (CopyTextToClipboard(window_ ? window_->GetHandle() : nullptr, editor_->GetSelectedText()))
                {
                    editor_->DeleteSelection();
                    InvalidateVisual();
                }
            }
            return true;

        case 'V': {
            std::wstring clipboardText = ReadClipboardText(window_ ? window_->GetHandle() : nullptr);
            if (!IsMultiline())
            {
                // A single line keeps a pasted multi-line text on one line.
                clipboardText.erase(std::remove_if(clipboardText.begin(), clipboardText.end(),
                                                   [](wchar_t ch) { return ch == L'\r' || ch == L'\n'; }),
                                    clipboardText.end());
            }
            if (!clipboardText.empty())
            {
                editor_->InsertAtSelection(clipboardText.c_str());
                InvalidateVisual();
            }
            return true;
        }

        default:
            break;
        }
    }

    switch (0xff & key)
    {
    case VK_ESCAPE:
        return false;

    case VK_RETURN:
        if (IsMultiline())
        {
            editor_->InsertAtSelection(L"\n");
            InvalidateVisual();
            return true;
        }
        if (onSubmit_)
        {
            // A handler may tear down the control that owns onSubmit_ while it runs.
            const auto handler = onSubmit_;
            handler();
            return true;
        }
        return false;

    case VK_LEFT:
        if (GetKeyState(VK_SHIFT) & 0x80)
        {
            selectionStart = editor_->GetSelectionStart();
            selectionEnd = editor_->GetSelectionEnd();
            if (selectionStart > 0)
            {
                editor_->MoveSelection(selectionStart - 1, selectionEnd);
            }
        }
        else
        {
            editor_->MoveSelectionPrev();
        }
        break;

    case VK_RIGHT:
        if (GetKeyState(VK_SHIFT) & 0x80)
        {
            selectionStart = editor_->GetSelectionStart();
            selectionEnd = editor_->GetSelectionEnd();
            editor_->MoveSelection(selectionStart, selectionEnd + 1);
        }
        else
        {
            editor_->MoveSelectionNext();
        }
        break;

    case VK_UP:
        editor_->MoveSelectionUpDown(TRUE);
        break;

    case VK_DOWN:
        editor_->MoveSelectionUpDown(FALSE);
        break;

    case VK_HOME:
        editor_->MoveSelectionToLineFirstEnd(TRUE);
        break;

    case VK_END:
        editor_->MoveSelectionToLineFirstEnd(FALSE);
        break;

    case VK_DELETE:
        selectionStart = editor_->GetSelectionStart();
        selectionEnd = editor_->GetSelectionEnd();
        if (selectionStart == selectionEnd)
        {
            if (ctrlPressed)
            {
                editor_->DeleteNextWord();
            }
            else
            {
                editor_->DeleteAtSelection(FALSE);
            }
        }
        else
        {
            editor_->DeleteSelection();
        }
        break;

    case VK_BACK:
        selectionStart = editor_->GetSelectionStart();
        selectionEnd = editor_->GetSelectionEnd();
        if (selectionStart == selectionEnd)
        {
            if (ctrlPressed)
            {
                editor_->DeletePreviousWord();
            }
            else
            {
                editor_->DeleteAtSelection(TRUE);
            }
        }
        else
        {
            editor_->DeleteSelection();
        }
        break;

    default:
        return false;
    }

    InvalidateVisual();
    return true;
}

bool TextBox::OnChar(wchar_t ch, LPARAM lParam)
{
    (void)lParam;

    if (!editor_ || !enabled_)
    {
        return false;
    }

    if (ch < 0x20 || ch == 0x7F)
    {
        return true;
    }

    wchar_t buffer[2] = {ch, L'\0'};
    editor_->InsertAtSelection(buffer);
    InvalidateVisual();
    return true;
}

bool TextBox::OnTimer(UINT_PTR timerId)
{
    if (timerId != kCaretTimerId || !focused_ || !editor_)
    {
        return false;
    }

    editor_->BlinkCaret();
    InvalidateVisual();
    return true;
}

HCURSOR TextBox::GetCursor() const
{
    return LoadCursor(nullptr, enabled_ ? IDC_IBEAM : IDC_ARROW);
}

std::wstring TextBox::GetText() const
{
    if (hasPendingText_)
    {
        return pendingText_;
    }
    if (!editor_ || !editor_->GetTextBuffer() || editor_->GetTextLength() == 0)
    {
        return {};
    }
    return std::wstring(editor_->GetTextBuffer(), editor_->GetTextLength());
}

void TextBox::SetOnTextChanged(TextChangedHandler handler)
{
    onTextChanged_ = std::move(handler);
}

void TextBox::SetOnFocusChanged(std::function<void(bool focused)> handler)
{
    onFocusChanged_ = std::move(handler);
}

void TextBox::SetFontSize(float fontSizeDips)
{
    fontSizeDips_ = std::max(fontSizeDips, 1.0f);
    if (editor_ && window_)
    {
        ApplyFont();
        editor_->UpdateLayout();
        editor_->NotifyLayoutChange();
        InvalidateVisual();
    }
}

void TextBox::SetPlaceholderFontSize(float fontSizeDips)
{
    placeholderFontSizeDips_ = std::max(fontSizeDips, 1.0f);
    InvalidateVisual();
}

void TextBox::SetPlaceholderText(std::wstring placeholder)
{
    if (placeholder_ == placeholder)
    {
        return;
    }
    placeholder_ = std::move(placeholder);
    InvalidateVisual();
}

void TextBox::SetChromeVisible(bool visible)
{
    if (chromeVisible_ == visible)
    {
        return;
    }
    chromeVisible_ = visible;
    InvalidateVisual();
}

POINT TextBox::ToLocalPoint(const POINT &point) const
{
    const RECT hostRect = ComputeEditorHostRect();
    POINT local = {point.x - hostRect.left, point.y - hostRect.top};
    return local;
}

RECT TextBox::ComputeEditorHostRect() const
{
    const float dpi = window_ ? window_->GetDpi() : 96.0f;
    // A single line is clipped at its horizontal padding like an <input>; a multi-line
    // box scrolls its text under the padding like a <textarea>, so only the border clips.
    const float insetX = IsMultiline() ? appearance_.borderWidth : appearance_.paddingX;
    const float insetY = appearance_.borderWidth;
    const RectF inner = {bounds_.x + insetX, bounds_.y + insetY, std::max(bounds_.width - insetX * 2.0f, 0.0f),
                         std::max(bounds_.height - insetY * 2.0f, 0.0f)};
    return ToRectPixels(inner, dpi);
}

RECT TextBox::ComputeEditorContentPadding() const
{
    const float dpi = window_ ? window_->GetDpi() : 96.0f;
    const auto pixels = [dpi](float dips) {
        return static_cast<LONG>(std::lround(DipsToPixels(std::max(dips, 0.0f), dpi)));
    };
    RECT padding = {};
    const LONG y = pixels(appearance_.paddingY - appearance_.borderWidth);
    padding.top = y;
    padding.bottom = y;
    if (IsMultiline())
    {
        const LONG x = pixels(appearance_.paddingX - appearance_.borderWidth);
        padding.left = x;
        padding.right = x;
    }
    return padding;
}

bool TextBox::EnsureInitialized(DeviceResources *deviceResources)
{
    if (!editor_ || !window_ || !deviceResources)
    {
        return false;
    }

    if (!renderInitialized_)
    {
        renderInitialized_ = editor_->InitializeRenderResources(deviceResources->GetDWriteFactory()) == TRUE;
        if (renderInitialized_)
        {
            editor_->SetMaskCharacter(password_ ? L'\x2022' : 0);
            RefreshEditorLayout();
        }
    }

    return renderInitialized_ && tsfInitialized_;
}

bool TextBox::AlertMouseSink(const POINT &point, WPARAM keyState)
{
    if (!editor_)
    {
        return false;
    }

    DWORD buttonState = 0;
    if (keyState & MK_LBUTTON)
    {
        buttonState = MK_LBUTTON;
    }
    else if (keyState & MK_RBUTTON)
    {
        buttonState = MK_RBUTTON;
    }

    BOOL eaten = FALSE;
    editor_->AleartMouseSink(point, buttonState, &eaten);
    return eaten == TRUE;
}
} // namespace msimeui