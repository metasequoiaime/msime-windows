// Button and CheckBox: the clickable controls with a label, focus ring and pressed/hover states.
#include "msimeui/Controls.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Theme.h"
#include "msimeui/Window.h"

#include "ControlsInternal.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace msimeui
{
using namespace controls_detail;

namespace
{
constexpr float kControlPaddingX = 16.0f;
constexpr float kControlPaddingY = 10.0f;
constexpr float kCheckBoxIndicatorSize = 20.0f;

void DrawTextBlock(DeviceResources &deviceResources, const std::wstring &text, float fontSize, bool bold,
                   D2D1_COLOR_F color, const RectF &rect, DWRITE_TEXT_ALIGNMENT textAlignment)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    IDWriteTextFormat *format = deviceResources.GetTextFormat(
        theme.uiFontFamily, fontSize, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, textAlignment,
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
    ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(color);
    if (!format || !brush)
    {
        return;
    }
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
                      D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height), brush);
}
} // namespace

Button::Button(std::wstring text, float height) : text_(std::move(text)), preferredHeight_(height)
{
}

void Button::SetOnClick(ClickHandler handler)
{
    onClick_ = std::move(handler);
}

void Button::InvalidateTextLayoutCache()
{
    cachedTextLayout_.Reset();
    cachedFontFamily_.clear();
    cachedLayoutWidth_ = -1.0f;
}

SizeF Button::Measure(const SizeF &availableSize)
{
    const float textWidth = std::max(availableSize.width - kControlPaddingX * 2.0f, 1.0f);
    const Theme &theme = ThemeManager::GetCurrent();
    if (!cachedTextLayout_ || cachedLayoutWidth_ != textWidth || cachedFontFamily_ != theme.uiFontFamily)
    {
        cachedTextLayout_ = CreateCachedTextLayout(GetSharedDWriteFactory(), theme.uiFontFamily, text_, 16.0f,
                                                   DWRITE_FONT_WEIGHT_SEMI_BOLD, textWidth, preferredHeight_,
                                                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                                   DWRITE_WORD_WRAPPING_NO_WRAP);
        cachedLayoutWidth_ = textWidth;
        cachedFontFamily_ = theme.uiFontFamily;
    }

    SizeF measuredText = MeasureText(GetSharedDWriteFactory(), text_, 16.0f, true, textWidth);
    if (cachedTextLayout_)
    {
        DWRITE_TEXT_METRICS metrics = {};
        if (SUCCEEDED(cachedTextLayout_->GetMetrics(&metrics)))
        {
            measuredText = {std::ceil(metrics.widthIncludingTrailingWhitespace), std::ceil(metrics.height)};
        }
    }

    return {std::min(availableSize.width, measuredText.width + kControlPaddingX * 2.0f),
            std::max(preferredHeight_, measuredText.height + kControlPaddingY * 2.0f)};
}

void Button::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void Button::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    FillRoundedRect(deviceResources, bounds_, kControlCornerRadius, GetFillColor(), GetStrokeColor(), 1.0f);

    const Theme &theme = ThemeManager::GetCurrent();
    const float textWidth = std::max(bounds_.width - kControlPaddingX * 2.0f, 1.0f);
    if (!cachedTextLayout_ || cachedLayoutWidth_ != textWidth || cachedFontFamily_ != theme.uiFontFamily)
    {
        cachedTextLayout_ = CreateCachedTextLayout(deviceResources.GetDWriteFactory(), theme.uiFontFamily, text_, 16.0f,
                                                   DWRITE_FONT_WEIGHT_SEMI_BOLD, textWidth, bounds_.height,
                                                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                                   DWRITE_WORD_WRAPPING_NO_WRAP);
        cachedLayoutWidth_ = textWidth;
        cachedFontFamily_ = theme.uiFontFamily;
    }

    ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(GetTextColor());
    if (!cachedTextLayout_ || !brush)
    {
        return;
    }

    target->DrawTextLayout(D2D1::Point2F(bounds_.x + kControlPaddingX, bounds_.y), cachedTextLayout_.Get(), brush,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

bool Button::HitTest(const PointF &point) const
{
    return PointInRoundedRect(bounds_, kControlCornerRadius, point);
}

bool Button::IsFocusable() const
{
    return true;
}

void Button::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool Button::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    pressed_ = HitTest(window_->ClientPixelsToDips(point));
    InvalidateVisual();
    return pressed_;
}

bool Button::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const bool shouldClick = HitTest(window_->ClientPixelsToDips(point));
    pressed_ = false;
    InvalidateVisual();
    if (shouldClick)
    {
        OnClick();
    }
    return true;
}

HCURSOR Button::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

void Button::OnClick()
{
    if (onClick_)
    {
        // Same reason as MenuFlyoutItem::OnMouseUp: a handler that tears down the
        // panel holding this button destroys onClick_ mid-call.
        ClickHandler handler = onClick_;
        handler();
    }
}

D2D1_COLOR_F Button::GetFillColor() const
{
    const Theme &theme = ThemeManager::GetCurrent();
    return pressed_ ? theme.primaryPressed : theme.primary;
}

D2D1_COLOR_F Button::GetStrokeColor() const
{
    const Theme &theme = ThemeManager::GetCurrent();
    return theme.primaryPressed;
}

D2D1_COLOR_F Button::GetTextColor() const
{
    return ThemeManager::GetCurrent().textInverse;
}

CheckBox::CheckBox(std::wstring text, bool checked) : text_(std::move(text)), checked_(checked)
{
}

void CheckBox::SetOnChanged(ChangeHandler handler)
{
    onChanged_ = std::move(handler);
}

bool CheckBox::IsChecked() const
{
    return checked_;
}

void CheckBox::SetChecked(bool checked)
{
    if (checked_ == checked)
    {
        return;
    }

    checked_ = checked;
    InvalidateVisual();
}

SizeF CheckBox::Measure(const SizeF &availableSize)
{
    const float textWidth = std::max(availableSize.width - kCheckBoxIndicatorSize - 12.0f, 1.0f);
    const SizeF measuredText = MeasureText(GetSharedDWriteFactory(), text_, 15.0f, false, textWidth);
    return {std::min(availableSize.width, kCheckBoxIndicatorSize + 12.0f + measuredText.width),
            std::max(28.0f, measuredText.height)};
}

void CheckBox::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void CheckBox::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    const float indicatorY = bounds_.y + (bounds_.height - kCheckBoxIndicatorSize) * 0.5f;
    const RectF indicator = {bounds_.x, indicatorY, kCheckBoxIndicatorSize, kCheckBoxIndicatorSize};
    FillRoundedRect(deviceResources, indicator, 6.0f, checked_ ? theme.primary : theme.surface,
                    focused_ ? theme.primaryFocusStrong : theme.border, focused_ ? 2.0f : 1.0f);

    if (checked_)
    {
        ID2D1SolidColorBrush *checkBrush = deviceResources.GetSolidColorBrush(theme.textInverse);
        if (!checkBrush)
        {
            return;
        }
        target->DrawLine(D2D1::Point2F(indicator.x + 5.0f, indicator.y + 11.0f),
                         D2D1::Point2F(indicator.x + 9.0f, indicator.y + 15.0f), checkBrush, 2.0f);
        target->DrawLine(D2D1::Point2F(indicator.x + 9.0f, indicator.y + 15.0f),
                         D2D1::Point2F(indicator.x + 15.0f, indicator.y + 6.0f), checkBrush, 2.0f);
    }

    const RectF textRect = {indicator.x + indicator.width + 12.0f, bounds_.y,
                            std::max(bounds_.width - indicator.width - 12.0f, 0.0f), bounds_.height};
    DrawTextBlock(deviceResources, text_, 15.0f, false, theme.textPrimary, textRect, DWRITE_TEXT_ALIGNMENT_LEADING);
}

bool CheckBox::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool CheckBox::IsFocusable() const
{
    return true;
}

void CheckBox::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool CheckBox::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    pressed_ = HitTest(window_->ClientPixelsToDips(point));
    return pressed_;
}

bool CheckBox::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const bool shouldToggle = HitTest(window_->ClientPixelsToDips(point));
    pressed_ = false;
    if (shouldToggle)
    {
        checked_ = !checked_;
        if (onChanged_)
        {
            const auto handler = onChanged_;
            handler(checked_);
        }
    }

    InvalidateVisual();
    return true;
}

HCURSOR CheckBox::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}
} // namespace msimeui
