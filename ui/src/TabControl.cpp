// TabControl: a row of tab headers that switches between content visuals.
#include "msimeui/Controls.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Window.h"

#include "ControlsInternal.h"

#include <algorithm>
#include <utility>

namespace msimeui
{
using namespace controls_detail;

namespace
{
constexpr float kTabCornerRadius = 12.0f;
} // namespace

TabControl::TabControl(float headerHeight) : headerHeight_(headerHeight)
{
}

void TabControl::AddTab(std::wstring title, std::shared_ptr<Visual> content)
{
    AdoptChild(content);
    tabs_.push_back({std::move(title), std::move(content)});
    InvalidateMeasure();
}

void TabControl::ClearTabs()
{
    for (auto &tab : tabs_)
    {
        ReleaseChild(tab.content);
    }
    tabs_.clear();
    headerRects_.clear();
    selectedIndex_ = 0;
    pressedIndex_ = static_cast<size_t>(-1);
    pressed_ = false;
    InvalidateMeasure();
}

void TabControl::SetSelectedIndex(size_t index)
{
    if (tabs_.empty())
    {
        selectedIndex_ = 0;
        return;
    }

    const size_t clamped = std::min(index, tabs_.size() - 1);
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
    InvalidateMeasure();
}

size_t TabControl::GetSelectedIndex() const
{
    return selectedIndex_;
}

void TabControl::SetOnSelectionChanged(SelectionChangedHandler handler)
{
    onSelectionChanged_ = std::move(handler);
}

SizeF TabControl::Measure(const SizeF &availableSize)
{
    float contentHeight = 0.0f;
    float contentWidth = availableSize.width;
    if (!tabs_.empty() && tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content)
    {
        const SizeF contentSize = tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content->MeasureInLayout(
            {availableSize.width, std::max(availableSize.height - headerHeight_ - 12.0f, 0.0f)});
        contentHeight = contentSize.height;
        contentWidth = std::max(contentWidth, contentSize.width);
    }

    return {std::min(contentWidth, availableSize.width),
            std::min(headerHeight_ + 12.0f + contentHeight, availableSize.height)};
}

void TabControl::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    headerRects_.clear();

    float cursorX = finalRect.x;
    for (const auto &tab : tabs_)
    {
        const float textWidth = MeasureText(GetSharedDWriteFactory(), tab.title, 14.0f, true, 400.0f).width;
        const float tabWidth = std::max(textWidth + 28.0f, 96.0f);
        headerRects_.push_back({cursorX, finalRect.y, tabWidth, headerHeight_});
        cursorX += tabWidth + 8.0f;
    }

    if (!tabs_.empty() && tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content)
    {
        tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content->ArrangeInLayout(
            {finalRect.x, finalRect.y + headerHeight_ + 12.0f, finalRect.width,
             std::max(finalRect.height - headerHeight_ - 12.0f, 0.0f)});
    }
}

void TabControl::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    for (size_t index = 0; index < tabs_.size(); ++index)
    {
        const bool selected = index == selectedIndex_;
        const bool pressed = pressed_ && index == pressedIndex_;
        FillRoundedRect(deviceResources, headerRects_[index], kTabCornerRadius,
                        pressed ? D2D1::ColorF(0xDBEAFE) : (selected ? D2D1::ColorF(0xEFF6FF) : D2D1::ColorF(0xF8FAFC)),
                        selected ? D2D1::ColorF(0x60A5FA) : D2D1::ColorF(0xCBD5E1), selected ? 2.0f : 1.0f);
        DrawLabel(deviceResources, tabs_[index].title, 14.0f, true,
                  selected ? D2D1::ColorF(0x1D4ED8) : D2D1::ColorF(0x334155), headerRects_[index],
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    const RectF contentFrame = {bounds_.x, bounds_.y + headerHeight_ + 8.0f, bounds_.width,
                                std::max(bounds_.height - headerHeight_ - 8.0f, 0.0f)};
    FillRoundedRect(deviceResources, contentFrame, 18.0f, D2D1::ColorF(0xFFFFFF), D2D1::ColorF(0xD6DCE5), 1.0f);

    if (!tabs_.empty() && tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content)
    {
        tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content->Render(deviceResources);
    }
}

void TabControl::Attach(Window *window)
{
    Visual::Attach(window);
    for (auto &tab : tabs_)
    {
        if (tab.content)
        {
            tab.content->Attach(window);
        }
    }
}

Visual *TabControl::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    if (HitTestHeader(point) != static_cast<size_t>(-1))
    {
        return this;
    }

    if (!tabs_.empty() && tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content)
    {
        if (Visual *hit = tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content->FindVisualAt(point))
        {
            return hit;
        }
    }

    return this;
}

Visual *TabControl::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    if (!tabs_.empty() && tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content)
    {
        return tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content->FindFocusableAt(point);
    }

    return nullptr;
}

Visual *TabControl::FindFirstFocusableDescendant()
{
    if (!tabs_.empty() && tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content)
    {
        return tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content->FindFirstFocusableDescendant();
    }

    return nullptr;
}

bool TabControl::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool TabControl::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    const size_t hit = HitTestHeader(window_->ClientPixelsToDips(point));
    if (hit == static_cast<size_t>(-1))
    {
        return false;
    }

    pressed_ = true;
    pressedIndex_ = hit;
    InvalidateVisual();
    return true;
}

bool TabControl::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const size_t hit = HitTestHeader(window_->ClientPixelsToDips(point));
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

HCURSOR TabControl::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

size_t TabControl::HitTestHeader(const PointF &point) const
{
    for (size_t index = 0; index < headerRects_.size(); ++index)
    {
        if (PointInRect(headerRects_[index], point))
        {
            return index;
        }
    }

    return static_cast<size_t>(-1);
}
} // namespace msimeui
