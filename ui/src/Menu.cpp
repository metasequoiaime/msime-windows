// Menu flyout rows: MenuFlyoutItem (text, submenu chevron, leading SVG icon, trailing toggle) and
// MenuSeparator.
#include "msimeui/Controls.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/Window.h"

#include "ControlsInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <d2d1_3.h>
#include <utility>
#include <shlwapi.h>

namespace msimeui
{
using namespace controls_detail;

namespace
{
constexpr float kTrayMenuFontSize = 14.0f;
constexpr float kTrayMenuItemPad = 8.0f;

float EstimateTrayLabelWidth(const std::wstring &text)
{
    ComPtr<IDWriteTextLayout> layout = CreateCachedTextLayout(
        GetSharedDWriteFactory(), UiFontFamily(), text, kTrayMenuFontSize, DWRITE_FONT_WEIGHT_NORMAL, 4096.0f, 30.0f,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!layout)
    {
        return kTrayMenuFontSize * static_cast<float>(text.size());
    }
    DWRITE_TEXT_METRICS metrics = {};
    if (FAILED(layout->GetMetrics(&metrics)))
    {
        return kTrayMenuFontSize * static_cast<float>(text.size());
    }
    return std::ceil(metrics.widthIncludingTrailingWhitespace);
}
} // namespace

MenuFlyoutItem::MenuFlyoutItem(std::wstring text, bool hasSubmenu) : text_(std::move(text)), hasSubmenu_(hasSubmenu)
{
    SetHeight(24.0f);
}

void MenuFlyoutItem::SetOnClick(ClickHandler handler)
{
    onClick_ = std::move(handler);
}

void MenuFlyoutItem::SetOnHover(HoverHandler handler)
{
    onHover_ = std::move(handler);
}

void MenuFlyoutItem::SetColors(const D2D1_COLOR_F &text, const D2D1_COLOR_F &hoverFill)
{
    textColor_ = text;
    hoverFill_ = hoverFill;
    InvalidateVisual();
}

void MenuFlyoutItem::SetHasSubmenu(bool hasSubmenu)
{
    hasSubmenu_ = hasSubmenu;
    InvalidateVisual();
}

void MenuFlyoutItem::SetLeadingSvg(std::string svgUtf8)
{
    leadingSvg_ = std::move(svgUtf8);
    svgDocument_.Reset();
    InvalidateMeasure();
    InvalidateVisual();
}

void MenuFlyoutItem::SetTrailingToggle(bool show)
{
    if (showToggle_ == show)
    {
        return;
    }
    showToggle_ = show;
    InvalidateMeasure();
    InvalidateVisual();
}

void MenuFlyoutItem::SetToggleOn(bool on)
{
    if (toggleOn_ == on)
    {
        return;
    }
    toggleOn_ = on;
    InvalidateVisual();
}

bool MenuFlyoutItem::IsToggleOn() const
{
    return toggleOn_;
}

bool MenuFlyoutItem::UsesTrayLayout() const
{
    return !leadingSvg_.empty() || showToggle_;
}

RectF MenuFlyoutItem::ToggleHitRect() const
{
    constexpr float kToggleWidth = 32.0f;
    constexpr float kToggleHeight = 16.0f;
    return {bounds_.x + bounds_.width - kTrayMenuItemPad - kToggleWidth,
            bounds_.y + (bounds_.height - kToggleHeight) * 0.5f, kToggleWidth, kToggleHeight};
}

SizeF MenuFlyoutItem::Measure(const SizeF &availableSize)
{
    if (UsesTrayLayout())
    {
        constexpr float kIcon = 18.0f;
        constexpr float kIconGap = 8.0f;
        constexpr float kToggleWidth = 32.0f;
        constexpr float kToggleGap = 8.0f;
        // HTML .menu { min-width: 12em } inherits body 16px, minus 4px item margins.
        constexpr float kMinItemWidth = 12.0f * 16.0f - 8.0f;
        float width = kTrayMenuItemPad + kIcon + kIconGap + EstimateTrayLabelWidth(text_);
        width += (showToggle_ ? (kToggleGap + kToggleWidth) : 0.0f) + kTrayMenuItemPad;
        width = (std::max)(width, kMinItemWidth);
        if (availableSize.width > 1.0f)
        {
            width = (std::min)(width, availableSize.width);
        }
        return {width, 36.0f};
    }
    const float chevron = hasSubmenu_ ? 16.0f : 0.0f;
    const float width = (std::min)((std::max)(availableSize.width, 88.0f + chevron), 220.0f);
    return {width, 24.0f};
}

void MenuFlyoutItem::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void MenuFlyoutItem::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const bool tray = UsesTrayLayout();
    // Tray rows are inset 4 DIP inside a 10 DIP card, so 6 keeps the corners concentric.
    const float hoverRadius = tray ? 6.0f : 4.0f;
    if (hovered_ || pressed_)
    {
        FillRoundedRect(deviceResources, bounds_, hoverRadius, hoverFill_, hoverFill_, 0.0f);
    }

    const wchar_t *fontFamily = UiFontFamily();
    const float pad = tray ? kTrayMenuItemPad : 4.0f;
    const float iconSize = leadingSvg_.empty() ? 0.0f : 18.0f;
    const float iconGap = leadingSvg_.empty() ? 0.0f : 8.0f;
    const float chevronReserve =
        hasSubmenu_ ? 16.0f : (showToggle_ ? kTrayMenuItemPad + 32.0f + 4.0f : (tray ? 4.0f : 4.0f));
    const float textX = bounds_.x + pad + iconSize + iconGap;
    const float textWidth = (std::max)(bounds_.width - (textX - bounds_.x) - chevronReserve, 1.0f);
    if (!textLayout_ || cachedLayoutWidth_ != textWidth || cachedFontFamily_ != fontFamily)
    {
        textLayout_ = CreateCachedTextLayout(deviceResources.GetDWriteFactory(), fontFamily, text_,
                                             tray ? kTrayMenuFontSize : 14.0f, DWRITE_FONT_WEIGHT_NORMAL, textWidth,
                                             bounds_.height, DWRITE_TEXT_ALIGNMENT_LEADING,
                                             DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        cachedLayoutWidth_ = textWidth;
        cachedFontFamily_ = fontFamily;
    }

    if (ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(textColor_))
    {
        if (!leadingSvg_.empty())
        {
            const float iconY = bounds_.y + (bounds_.height - iconSize) * 0.5f;
            const RectF iconRect = {bounds_.x + pad, iconY, iconSize, iconSize};
            char hex[8];
            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", static_cast<int>(textColor_.r * 255.0f + 0.5f),
                          static_cast<int>(textColor_.g * 255.0f + 0.5f),
                          static_cast<int>(textColor_.b * 255.0f + 0.5f));
            std::string tinted = leadingSvg_;
            for (size_t pos = 0; (pos = tinted.find("currentColor", pos)) != std::string::npos;)
            {
                tinted.replace(pos, 12, hex);
                pos += 7;
            }
            ComPtr<ID2D1DeviceContext5> dc5;
            if (SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(&dc5))))
            {
                if (!svgDocument_ || svgContext_ != dc5.Get() || svgTintCache_ != tinted)
                {
                    svgDocument_.Reset();
                    ComPtr<IStream> stream;
                    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE *>(tinted.data()),
                                                    static_cast<UINT>(tinted.size())));
                    ComPtr<ID2D1SvgDocument> document;
                    if (stream && SUCCEEDED(dc5->CreateSvgDocument(stream.Get(), D2D1::SizeF(iconSize, iconSize),
                                                                   document.GetAddressOf())))
                    {
                        svgDocument_ = document;
                        svgContext_ = dc5.Get();
                        svgTintCache_ = tinted;
                    }
                }
                if (svgDocument_)
                {
                    ComPtr<ID2D1SvgDocument> document;
                    svgDocument_.As(&document);
                    if (document)
                    {
                        document->SetViewportSize(D2D1::SizeF(iconSize, iconSize));
                        D2D1_MATRIX_3X2_F previous = D2D1::Matrix3x2F::Identity();
                        dc5->GetTransform(&previous);
                        dc5->SetTransform(previous * D2D1::Matrix3x2F::Translation(iconRect.x, iconRect.y));
                        dc5->DrawSvgDocument(document.Get());
                        dc5->SetTransform(previous);
                    }
                }
            }
        }
        if (textLayout_)
        {
            target->DrawTextLayout(D2D1::Point2F(textX, bounds_.y), textLayout_.Get(), brush,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (hasSubmenu_)
        {
            const float cx = bounds_.x + bounds_.width - 8.0f;
            const float cy = bounds_.y + bounds_.height * 0.5f;
            const float dx = 3.25f;
            const float dy = 4.0f;
            target->DrawLine(D2D1::Point2F(cx - dx, cy - dy), D2D1::Point2F(cx + 1.0f, cy), brush, 1.25f);
            target->DrawLine(D2D1::Point2F(cx + 1.0f, cy), D2D1::Point2F(cx - dx, cy + dy), brush, 1.25f);
        }
    }

    if (showToggle_)
    {
        const RectF track = ToggleHitRect();
        const D2D1_COLOR_F off = D2D1::ColorF(textColor_.r > 0.5f ? 0x555555 : 0xC8C8C8);
        const D2D1_COLOR_F on = D2D1::ColorF(0x8E8CD8);
        FillRoundedRect(deviceResources, track, 8.0f, toggleOn_ ? on : off, toggleOn_ ? on : off, 0.0f);
        const float thumb = 14.0f;
        const float thumbX = track.x + 1.0f + (toggleOn_ ? 16.0f : 0.0f);
        const float thumbY = track.y + 1.0f;
        if (ID2D1SolidColorBrush *thumbBrush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0xFFFFFF)))
        {
            target->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(thumbX + thumb * 0.5f, thumbY + thumb * 0.5f), thumb * 0.5f, thumb * 0.5f),
                thumbBrush);
        }
    }
}

bool MenuFlyoutItem::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool MenuFlyoutItem::IsFocusable() const
{
    return true;
}

void MenuFlyoutItem::SetHovered(bool hovered)
{
    if (hovered_ == hovered)
    {
        return;
    }
    hovered_ = hovered;
    InvalidateVisual();
    if (onHover_)
    {
        const auto handler = onHover_;
        handler(hovered_);
    }
}

bool MenuFlyoutItem::OnMouseDown(const POINT &point, WPARAM keyState)
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

bool MenuFlyoutItem::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }
    const bool shouldClick = HitTest(window_->ClientPixelsToDips(point));
    pressed_ = false;
    InvalidateVisual();
    if (shouldClick && onClick_ && !hasSubmenu_)
    {
        // A menu item's handler almost always closes the menu it lives in, which
        // drops the last reference to this item and destroys onClick_ while its
        // operator() is still on the stack. The handler body then reloads its
        // captures from freed storage the teardown has already recycled. Copy the
        // handler so the closure outlives the call; nothing below touches *this*.
        ClickHandler handler = onClick_;
        if (showToggle_)
        {
            if (PointInRect(ToggleHitRect(), window_->ClientPixelsToDips(point)))
            {
                toggleOn_ = !toggleOn_;
                InvalidateVisual();
                handler();
            }
        }
        else
        {
            handler();
        }
    }
    return true;
}

bool MenuFlyoutItem::OnMouseMove(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }
    SetHovered(HitTest(window_->ClientPixelsToDips(point)));
    return hovered_;
}

void MenuFlyoutItem::OnMouseLeave()
{
    pressed_ = false;
    SetHovered(false);
}

bool MenuFlyoutItem::KeepsPopupsOpenOnClick() const
{
    return true;
}

HCURSOR MenuFlyoutItem::GetCursor() const
{
    return LoadCursor(nullptr, IDC_ARROW);
}

void MenuSeparator::SetColor(const D2D1_COLOR_F &color)
{
    color_ = color;
    InvalidateVisual();
}

SizeF MenuSeparator::Measure(const SizeF &availableSize)
{
    return {(std::min)((std::max)(availableSize.width, 72.0f), 220.0f), 7.0f};
}

void MenuSeparator::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void MenuSeparator::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }
    if (ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(color_))
    {
        const float y = bounds_.y + bounds_.height * 0.5f;
        target->DrawLine(D2D1::Point2F(bounds_.x + 5.0f, y), D2D1::Point2F(bounds_.x + bounds_.width - 5.0f, y), brush,
                         1.0f);
    }
}

bool MenuSeparator::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}
} // namespace msimeui
