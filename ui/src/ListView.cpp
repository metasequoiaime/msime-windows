// ListView: a vertical list of selectable text items.
#include "msimeui/Controls.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Theme.h"
#include "msimeui/Window.h"

#include "ControlsInternal.h"

#include <algorithm>
#include <utility>

namespace msimeui
{
using namespace controls_detail;

ListView::ListView(float itemHeight) : itemHeight_(itemHeight)
{
}

void ListView::InvalidateLayoutCache()
{
    layoutCache_.clear();
}

void ListView::AddItem(Item item)
{
    items_.push_back(std::move(item));
    layoutCache_.push_back({});
    InvalidateMeasure();
}

void ListView::ClearItems()
{
    items_.clear();
    InvalidateLayoutCache();
    selectedIndex_ = 0;
    pressedIndex_ = static_cast<size_t>(-1);
    InvalidateMeasure();
}

void ListView::SetOnSelectionChanged(SelectionChangedHandler handler)
{
    onSelectionChanged_ = std::move(handler);
}

void ListView::SetSelectedIndex(size_t index)
{
    if (items_.empty())
    {
        selectedIndex_ = 0;
        return;
    }

    const size_t clamped = std::min(index, items_.size() - 1);
    if (selectedIndex_ == clamped)
    {
        return;
    }

    selectedIndex_ = clamped;
    if (onSelectionChanged_)
    {
        const auto handler = onSelectionChanged_;
        handler(selectedIndex_);
    }
    if (window_)
    {
        window_->Relayout();
    }
}

size_t ListView::GetSelectedIndex() const
{
    return selectedIndex_;
}

SizeF ListView::Measure(const SizeF &availableSize)
{
    const float height = items_.empty() ? itemHeight_ : itemHeight_ * static_cast<float>(items_.size());
    return {availableSize.width, std::min(height, availableSize.height)};
}

void ListView::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void ListView::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    IDWriteFactory *factory = deviceResources.GetDWriteFactory();
    if (!target || !factory)
    {
        return;
    }

    if (layoutCache_.size() != items_.size())
    {
        layoutCache_.resize(items_.size());
    }

    const Theme &theme = ThemeManager::GetCurrent();
    for (size_t index = 0; index < items_.size(); ++index)
    {
        const float itemY = bounds_.y + itemHeight_ * static_cast<float>(index);
        const RectF itemRect = {bounds_.x, itemY, bounds_.width, itemHeight_ - 6.0f};
        const bool selected = index == selectedIndex_;
        const bool pressed = pressed_ && index == pressedIndex_;

        FillRoundedRect(deviceResources, itemRect, kListItemCornerRadius,
                        pressed ? D2D1::ColorF(0xDBEAFE) : (selected ? D2D1::ColorF(0xEFF6FF) : D2D1::ColorF(0xFFFFFF)),
                        selected ? D2D1::ColorF(0x60A5FA) : D2D1::ColorF(0xD6DCE5), selected || focused_ ? 2.0f : 1.0f);

        const RectF titleRect = {itemRect.x + 16.0f, itemRect.y + 10.0f, std::max(itemRect.width - 120.0f, 0.0f),
                                 22.0f};
        const RectF subtitleRect = {itemRect.x + 16.0f, itemRect.y + 34.0f, std::max(itemRect.width - 120.0f, 0.0f),
                                    18.0f};
        auto &cache = layoutCache_[index];
        if (cache.fontFamily != theme.uiFontFamily || cache.titleWidth != titleRect.width)
        {
            cache.titleLayout = CreateCachedTextLayout(factory, theme.uiFontFamily, items_[index].title, 15.0f,
                                                       DWRITE_FONT_WEIGHT_SEMI_BOLD, std::max(titleRect.width, 1.0f),
                                                       std::max(titleRect.height, 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING,
                                                       DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_NO_WRAP);
            cache.titleWidth = titleRect.width;
            cache.fontFamily = theme.uiFontFamily;
        }
        if (cache.fontFamily != theme.uiFontFamily || cache.subtitleWidth != subtitleRect.width)
        {
            cache.subtitleLayout = CreateCachedTextLayout(
                factory, theme.uiFontFamily, items_[index].subtitle, 13.0f, DWRITE_FONT_WEIGHT_NORMAL,
                std::max(subtitleRect.width, 1.0f), std::max(subtitleRect.height, 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_NO_WRAP);
            cache.subtitleWidth = subtitleRect.width;
            cache.fontFamily = theme.uiFontFamily;
        }

        ID2D1SolidColorBrush *titleBrush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0x0F172A));
        ID2D1SolidColorBrush *subtitleBrush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0x64748B));
        if (cache.titleLayout && titleBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(titleRect.x, titleRect.y), cache.titleLayout.Get(), titleBrush,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (cache.subtitleLayout && subtitleBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(subtitleRect.x, subtitleRect.y), cache.subtitleLayout.Get(),
                                   subtitleBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        if (!items_[index].badge.empty())
        {
            const RectF badgeRect = {itemRect.x + itemRect.width - 88.0f, itemRect.y + 18.0f, 72.0f, 28.0f};
            FillRoundedRect(deviceResources, badgeRect, 14.0f,
                            selected ? D2D1::ColorF(0x2563EB) : D2D1::ColorF(0xE2E8F0),
                            selected ? D2D1::ColorF(0x2563EB) : D2D1::ColorF(0xCBD5E1), 1.0f);
            if (cache.fontFamily != theme.uiFontFamily || cache.badgeWidth != badgeRect.width)
            {
                cache.badgeLayout = CreateCachedTextLayout(
                    factory, theme.uiFontFamily, items_[index].badge, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                    std::max(badgeRect.width, 1.0f), std::max(badgeRect.height, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                cache.badgeWidth = badgeRect.width;
                cache.fontFamily = theme.uiFontFamily;
            }

            ID2D1SolidColorBrush *badgeBrush =
                deviceResources.GetSolidColorBrush(selected ? D2D1::ColorF(0xFFFFFF) : D2D1::ColorF(0x334155));
            if (cache.badgeLayout && badgeBrush)
            {
                target->DrawTextLayout(D2D1::Point2F(badgeRect.x, badgeRect.y), cache.badgeLayout.Get(), badgeBrush,
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }
}

bool ListView::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool ListView::IsFocusable() const
{
    return true;
}

void ListView::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool ListView::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const size_t hit = HitTestItem(dipPoint);
    if (hit == static_cast<size_t>(-1))
    {
        return false;
    }

    pressed_ = true;
    pressedIndex_ = hit;
    InvalidateVisual();
    return true;
}

bool ListView::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const size_t hit = HitTestItem(dipPoint);
    const size_t pressedIndex = pressedIndex_;
    pressed_ = false;
    pressedIndex_ = static_cast<size_t>(-1);

    if (hit != static_cast<size_t>(-1) && hit == pressedIndex)
    {
        SetSelectedIndex(hit);
    }

    InvalidateVisual();
    return true;
}

HCURSOR ListView::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

size_t ListView::HitTestItem(const PointF &point) const
{
    if (!HitTest(point) || items_.empty())
    {
        return static_cast<size_t>(-1);
    }

    const float relativeY = point.y - bounds_.y;
    const size_t index = static_cast<size_t>(relativeY / itemHeight_);
    return index < items_.size() ? index : static_cast<size_t>(-1);
}
} // namespace msimeui
