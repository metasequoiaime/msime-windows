// Value-range controls: ProgressBar and Slider.
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

namespace
{
constexpr float kSliderTrackHeight = 6.0f;
constexpr float kSliderThumbRadius = 9.0f;
} // namespace

ProgressBar::ProgressBar(float height) : preferredHeight_(height)
{
}

void ProgressBar::SetValue(float value)
{
    value_ = std::clamp(value, 0.0f, 1.0f);
    InvalidateVisual();
}

float ProgressBar::GetValue() const
{
    return value_;
}

SizeF ProgressBar::Measure(const SizeF &availableSize)
{
    return {availableSize.width, preferredHeight_};
}

void ProgressBar::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void ProgressBar::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    FillRoundedRect(deviceResources, bounds_, preferredHeight_ * 0.5f, theme.track, theme.border, 1.0f);

    const RectF fillRect = {bounds_.x, bounds_.y, bounds_.width * value_, bounds_.height};
    if (fillRect.width > 0.0f)
    {
        FillRoundedRect(deviceResources, fillRect, preferredHeight_ * 0.5f, theme.success, theme.success, 1.0f);
    }
}

Slider::Slider(float minValue, float maxValue, float value, float height)
    : minValue_(minValue), maxValue_(std::max(maxValue, minValue)), preferredHeight_(height)
{
    SetValueInternal(value, false);
}

void Slider::SetOnChanged(ChangeHandler handler)
{
    onChanged_ = std::move(handler);
}

void Slider::SetValue(float value)
{
    SetValueInternal(value, false);
}

float Slider::GetValue() const
{
    return value_;
}

SizeF Slider::Measure(const SizeF &availableSize)
{
    return {availableSize.width, preferredHeight_};
}

void Slider::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void Slider::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    const RectF track = {bounds_.x, bounds_.y + (bounds_.height - kSliderTrackHeight) * 0.5f, bounds_.width,
                         kSliderTrackHeight};
    FillRoundedRect(deviceResources, track, kSliderTrackHeight * 0.5f, theme.track, theme.border, 1.0f);

    const RectF active = {track.x, track.y, track.width * NormalizedValue(), track.height};
    if (active.width > 0.0f)
    {
        FillRoundedRect(deviceResources, active, kSliderTrackHeight * 0.5f, theme.primary, theme.primary, 1.0f);
    }

    const float thumbCenterX = track.x + track.width * NormalizedValue();
    const float thumbCenterY = bounds_.y + bounds_.height * 0.5f;
    ID2D1SolidColorBrush *fillBrush = deviceResources.GetSolidColorBrush(theme.surface);
    ID2D1SolidColorBrush *strokeBrush =
        deviceResources.GetSolidColorBrush(focused_ || dragging_ ? theme.primary : theme.thumb);
    if (!fillBrush || !strokeBrush)
    {
        return;
    }
    target->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(thumbCenterX, thumbCenterY), kSliderThumbRadius, kSliderThumbRadius), fillBrush);
    target->DrawEllipse(
        D2D1::Ellipse(D2D1::Point2F(thumbCenterX, thumbCenterY), kSliderThumbRadius, kSliderThumbRadius), strokeBrush,
        focused_ || dragging_ ? 2.0f : 1.0f);
}

bool Slider::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool Slider::IsFocusable() const
{
    return true;
}

void Slider::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool Slider::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    dragging_ = UpdateFromPoint(point, true);
    return dragging_;
}

bool Slider::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!dragging_)
    {
        return false;
    }

    UpdateFromPoint(point, true);
    dragging_ = false;
    InvalidateVisual();
    return true;
}

bool Slider::OnMouseMove(const POINT &point, WPARAM keyState)
{
    if (!dragging_ || !(keyState & MK_LBUTTON))
    {
        return false;
    }

    return UpdateFromPoint(point, true);
}

HCURSOR Slider::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

float Slider::NormalizedValue() const
{
    const float range = maxValue_ - minValue_;
    if (range <= 0.0f)
    {
        return 0.0f;
    }

    return std::clamp((value_ - minValue_) / range, 0.0f, 1.0f);
}

void Slider::SetValueInternal(float value, bool notify)
{
    const float oldValue = value_;
    value_ = std::clamp(value, minValue_, maxValue_);
    InvalidateVisual();

    if (notify && oldValue != value_ && onChanged_)
    {
        const auto handler = onChanged_;
        handler(value_);
    }
}

bool Slider::UpdateFromPoint(const POINT &point, bool notify)
{
    if (!window_ || bounds_.width <= 0.0f)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const float ratio = std::clamp((dipPoint.x - bounds_.x) / bounds_.width, 0.0f, 1.0f);
    SetValueInternal(minValue_ + (maxValue_ - minValue_) * ratio, notify);
    return true;
}
} // namespace msimeui
