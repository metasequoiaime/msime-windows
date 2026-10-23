// ComboBox and the item list it shows in its drop-down popup.
#include "msimeui/Controls.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Theme.h"
#include "msimeui/Window.h"

#include "ControlsInternal.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

namespace msimeui
{
using namespace controls_detail;

namespace
{
constexpr float kComboBoxItemGap = 6.0f;

class ComboBoxPopupContent : public Visual
{
  public:
    using SelectHandler = std::function<void(size_t index)>;

    explicit ComboBoxPopupContent(float itemHeight) : itemHeight_(itemHeight)
    {
    }

    void SetItems(const std::vector<std::wstring> &items)
    {
        items_ = items;
        if (hoveredIndex_ >= items_.size())
        {
            hoveredIndex_ = static_cast<size_t>(-1);
        }
        if (pressedIndex_ >= items_.size())
        {
            pressedIndex_ = static_cast<size_t>(-1);
        }
        InvalidateMeasure();
        InvalidateVisual();
    }

    void SetSelectedIndex(size_t index)
    {
        selectedIndex_ = index;
        InvalidateVisual();
    }

    void SetHighlightedIndex(size_t index)
    {
        hoveredIndex_ = index < items_.size() ? index : static_cast<size_t>(-1);
        pressedIndex_ = static_cast<size_t>(-1);
        InvalidateVisual();
    }

    void SetOnSelect(SelectHandler handler)
    {
        onSelect_ = std::move(handler);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        float maxWidth = 160.0f;
        for (const auto &item : items_)
        {
            const SizeF measured =
                MeasureText(GetSharedDWriteFactory(), item, 15.0f, false, std::max(availableSize.width - 28.0f, 1.0f));
            maxWidth = std::max(maxWidth, measured.width + 28.0f);
        }

        if (items_.empty())
        {
            return {std::min(availableSize.width, maxWidth), 0.0f};
        }

        return {std::min(availableSize.width, maxWidth), itemHeight_ * static_cast<float>(items_.size()) +
                                                             kComboBoxItemGap * static_cast<float>(items_.size() - 1)};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(DeviceResources &deviceResources) override
    {
        ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
        if (!target)
        {
            return;
        }

        const Theme &theme = ThemeManager::GetCurrent();
        for (size_t index = 0; index < items_.size(); ++index)
        {
            const RectF row = ItemRect(index);
            const bool selected = index == selectedIndex_;
            const bool hovered = index == hoveredIndex_;
            const bool pressed = index == pressedIndex_;

            if (selected || hovered || pressed)
            {
                FillRoundedRect(deviceResources, row, 10.0f,
                                pressed ? theme.primarySoftPressed
                                        : (selected ? theme.primarySoft : theme.surfaceMuted),
                                selected ? theme.primaryFocus : theme.border, selected ? 1.5f : 1.0f);
            }

            DrawLabel(deviceResources, items_[index], 15.0f, selected, theme.textPrimary,
                      {row.x + 14.0f, row.y, std::max(row.width - 28.0f, 0.0f), row.height},
                      DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    bool HitTest(const PointF &point) const override
    {
        return PointInRect(bounds_, point);
    }

    bool IsFocusable() const override
    {
        return true;
    }

    bool OnMouseDown(const POINT &point, WPARAM keyState) override
    {
        (void)keyState;
        if (!window_)
        {
            return false;
        }

        const PointF dipPoint = window_->ClientPixelsToDips(point);
        pressedIndex_ = HitTestItem(dipPoint);
        if (pressedIndex_ != static_cast<size_t>(-1))
        {
            hoveredIndex_ = pressedIndex_;
            InvalidateVisual();
            return true;
        }

        return false;
    }

    bool OnMouseUp(const POINT &point, WPARAM keyState) override
    {
        (void)keyState;
        if (!window_)
        {
            return false;
        }

        const size_t pressedIndex = pressedIndex_;
        pressedIndex_ = static_cast<size_t>(-1);
        const PointF dipPoint = window_->ClientPixelsToDips(point);
        hoveredIndex_ = HitTestItem(dipPoint);
        InvalidateVisual();

        if (pressedIndex == static_cast<size_t>(-1))
        {
            return false;
        }

        if (pressedIndex == hoveredIndex_ && onSelect_)
        {
            const auto handler = onSelect_;
            handler(pressedIndex);
        }
        return true;
    }

    bool OnMouseMove(const POINT &point, WPARAM keyState) override
    {
        (void)keyState;
        if (!window_)
        {
            return false;
        }

        const size_t hovered = HitTestItem(window_->ClientPixelsToDips(point));
        if (hovered != hoveredIndex_)
        {
            hoveredIndex_ = hovered;
            InvalidateVisual();
        }
        return hovered != static_cast<size_t>(-1);
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, IDC_HAND);
    }

  private:
    RectF ItemRect(size_t index) const
    {
        return {bounds_.x, bounds_.y + (itemHeight_ + kComboBoxItemGap) * static_cast<float>(index), bounds_.width,
                itemHeight_};
    }

    size_t HitTestItem(const PointF &point) const
    {
        if (!HitTest(point) || items_.empty())
        {
            return static_cast<size_t>(-1);
        }

        const float localY = point.y - bounds_.y;
        const float stride = itemHeight_ + kComboBoxItemGap;
        const size_t index = static_cast<size_t>(localY / stride);
        const float withinItem = localY - stride * static_cast<float>(index);
        if (withinItem > itemHeight_)
        {
            return static_cast<size_t>(-1);
        }
        return index < items_.size() ? index : static_cast<size_t>(-1);
    }

    std::vector<std::wstring> items_;
    float itemHeight_ = 40.0f;
    size_t selectedIndex_ = static_cast<size_t>(-1);
    size_t hoveredIndex_ = static_cast<size_t>(-1);
    size_t pressedIndex_ = static_cast<size_t>(-1);
    SelectHandler onSelect_;
};
} // namespace

ComboBox::ComboBox(float height) : preferredHeight_(height)
{
    auto popupContent = std::make_shared<ComboBoxPopupContent>(40.0f);
    popupContent->SetOnSelect([this](size_t index) {
        if (index >= items_.size())
        {
            return;
        }

        selectedIndex_ = index;
        SyncPopupState();
        NotifySelectionChanged();
        ClosePopup();
        InvalidateVisual();
    });
    popupContent_ = popupContent;

    popup_ = std::make_shared<Popup>(popupContent_);
    popup_->SetPlacement(PopupPlacement::BelowLeading);
    popup_->SetOffset(0.0f, 6.0f);
    popup_->SetConstrainToViewport(false);
}

void ComboBox::AddItem(std::wstring item)
{
    items_.push_back(std::move(item));
    if (selectedIndex_ == static_cast<size_t>(-1))
    {
        selectedIndex_ = 0;
    }
    SyncPopupState();
    InvalidateMeasure();
    InvalidateVisual();
}

void ComboBox::ClearItems()
{
    items_.clear();
    selectedIndex_ = static_cast<size_t>(-1);
    ClosePopup();
    SyncPopupState();
    InvalidateMeasure();
    InvalidateVisual();
}

void ComboBox::SetSelectedIndex(size_t index)
{
    if (index >= items_.size() || selectedIndex_ == index)
    {
        return;
    }

    selectedIndex_ = index;
    SyncPopupState();
    NotifySelectionChanged();
    InvalidateVisual();
}

size_t ComboBox::GetSelectedIndex() const
{
    return selectedIndex_;
}

const std::wstring &ComboBox::GetSelectedText() const
{
    static const std::wstring empty;
    return selectedIndex_ < items_.size() ? items_[selectedIndex_] : empty;
}

void ComboBox::SetOnSelectionChanged(SelectionChangedHandler handler)
{
    onSelectionChanged_ = std::move(handler);
}

SizeF ComboBox::Measure(const SizeF &availableSize)
{
    float textWidth = 140.0f;
    for (const auto &item : items_)
    {
        const SizeF measured =
            MeasureText(GetSharedDWriteFactory(), item, 15.0f, false, std::max(availableSize.width - 44.0f, 1.0f));
        textWidth = std::max(textWidth, measured.width + 44.0f);
    }

    return {std::min(availableSize.width, textWidth), preferredHeight_};
}

void ComboBox::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    if (popup_)
    {
        popup_->SetAnchorRect(bounds_);
    }
}

void ComboBox::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    const D2D1_COLOR_F fill = pressed_ ? theme.surfaceMuted : theme.surface;
    const D2D1_COLOR_F stroke = focused_ || open_ ? theme.primaryFocus : theme.border;
    FillRoundedRect(deviceResources, bounds_, kControlCornerRadius, fill, stroke, focused_ || open_ ? 2.0f : 1.0f);

    const std::wstring text = GetSelectedText().empty() ? L"Select an option" : GetSelectedText();
    const D2D1_COLOR_F textColor = GetSelectedText().empty() ? theme.textSecondary : theme.textPrimary;
    DrawLabel(deviceResources, text, 15.0f, false, textColor,
              {bounds_.x + 16.0f, bounds_.y, std::max(bounds_.width - 44.0f, 0.0f), bounds_.height},
              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(theme.textSecondary);
    if (!brush)
    {
        return;
    }
    const float centerX = bounds_.x + bounds_.width - 18.0f;
    const float centerY = bounds_.y + bounds_.height * 0.5f + (open_ ? -1.0f : 1.0f);
    const float direction = open_ ? -1.0f : 1.0f;
    target->DrawLine(D2D1::Point2F(centerX - 5.0f, centerY - 3.0f * direction),
                     D2D1::Point2F(centerX, centerY + 2.0f * direction), brush, 1.8f);
    target->DrawLine(D2D1::Point2F(centerX, centerY + 2.0f * direction),
                     D2D1::Point2F(centerX + 5.0f, centerY - 3.0f * direction), brush, 1.8f);
}

void ComboBox::Attach(Window *window)
{
    Visual::Attach(window);
    if (popup_)
    {
        popup_->Attach(window);
    }
}

bool ComboBox::HitTest(const PointF &point) const
{
    return PointInRoundedRect(bounds_, kControlCornerRadius, point);
}

bool ComboBox::IsFocusable() const
{
    return true;
}

void ComboBox::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool ComboBox::OnMouseDown(const POINT &point, WPARAM keyState)
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

bool ComboBox::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const bool shouldToggle = HitTest(window_->ClientPixelsToDips(point));
    pressed_ = false;
    InvalidateVisual();
    if (shouldToggle)
    {
        TogglePopup();
    }
    return true;
}

HCURSOR ComboBox::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

bool ComboBox::KeepsPopupsOpenOnClick() const
{
    return true;
}

void ComboBox::OpenPopup()
{
    if (open_ || !window_ || !popup_ || items_.empty())
    {
        return;
    }

    if (const auto popupContent = std::dynamic_pointer_cast<ComboBoxPopupContent>(popupContent_))
    {
        popupContent->SetHighlightedIndex(selectedIndex_);
    }

    popup_->SetAnchorRect(bounds_);
    if (Scene *scene = window_->GetScene())
    {
        scene->AddPopup(popup_, [this]() {
            open_ = false;
            InvalidateVisual();
        });
        open_ = true;
        if (Visual *focusTarget = popup_->FindFirstFocusableDescendant())
        {
            window_->FocusVisual(focusTarget);
        }
        InvalidateVisual();
    }
}

void ComboBox::ClosePopup()
{
    if (!open_ || !window_ || !popup_)
    {
        return;
    }

    if (Scene *scene = window_->GetScene())
    {
        scene->RemovePopup(popup_.get(), false);
    }
    open_ = false;
    InvalidateVisual();
}

void ComboBox::TogglePopup()
{
    if (open_)
    {
        ClosePopup();
    }
    else
    {
        OpenPopup();
    }
}

void ComboBox::NotifySelectionChanged()
{
    if (onSelectionChanged_ && selectedIndex_ < items_.size())
    {
        // Copy the arguments first: the handler may replace items_ or destroy this control.
        const size_t index = selectedIndex_;
        const std::wstring value = items_[index];
        const auto handler = onSelectionChanged_;
        handler(index, value);
    }
}

void ComboBox::SyncPopupState()
{
    if (const auto popupContent = std::dynamic_pointer_cast<ComboBoxPopupContent>(popupContent_))
    {
        popupContent->SetItems(items_);
        popupContent->SetSelectedIndex(selectedIndex_);
    }
}
} // namespace msimeui
