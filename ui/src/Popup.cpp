// Popup overlays and the hosts that open them: Popup, PopupHost (click trigger) and ContextMenuHost
// (right-click trigger).
#include "msimeui/Controls.h"

#include "msimeui/Animation.h"
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
SizeF DeflateSizeLocal(const SizeF &size, const Thickness &thickness)
{
    return {std::max(size.width - thickness.left - thickness.right, 0.0f),
            std::max(size.height - thickness.top - thickness.bottom, 0.0f)};
}

RectF DeflateRectLocal(const RectF &rect, const Thickness &thickness)
{
    return {rect.x + thickness.left, rect.y + thickness.top,
            std::max(rect.width - thickness.left - thickness.right, 0.0f),
            std::max(rect.height - thickness.top - thickness.bottom, 0.0f)};
}

void DrawPopupShadow(DeviceResources &deviceResources, const RectF &bounds, float radius)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    struct ShadowLayer
    {
        float spread;
        float offsetY;
        float alpha;
    };

    static constexpr ShadowLayer kLayers[] = {
        {1.0f, 1.0f, 0.08f},
        {4.0f, 4.0f, 0.045f},
        {8.0f, 9.0f, 0.02f},
    };

    for (const ShadowLayer &layer : kLayers)
    {
        const RectF shadowRect = {bounds.x - layer.spread, bounds.y - layer.spread + layer.offsetY,
                                  bounds.width + layer.spread * 2.0f, bounds.height + layer.spread * 2.0f};
        const D2D1_COLOR_F shadowColor = D2D1::ColorF(0x000000, layer.alpha);
        ID2D1SolidColorBrush *shadowBrush = deviceResources.GetSolidColorBrush(shadowColor);
        if (!shadowBrush)
        {
            continue;
        }

        const auto rounded = D2D1::RoundedRect(
            D2D1::RectF(shadowRect.x, shadowRect.y, shadowRect.x + shadowRect.width, shadowRect.y + shadowRect.height),
            radius + layer.spread, radius + layer.spread);
        target->FillRoundedRectangle(rounded, shadowBrush);
    }
}
} // namespace

Popup::Popup(std::shared_ptr<Visual> child) : child_(std::move(child))
{
    AdoptChild(child_);
    SetPadding({12.0f, 12.0f, 12.0f, 12.0f});
}

void Popup::SetAnchorRect(const RectF &anchorRect)
{
    anchorRect_ = anchorRect;
}

void Popup::SetPlacement(PopupPlacement placement)
{
    placement_ = placement;
}

void Popup::SetOffset(float x, float y)
{
    offsetX_ = x;
    offsetY_ = y;
}

void Popup::SetMatchAnchorWidth(bool matchAnchorWidth)
{
    matchAnchorWidth_ = matchAnchorWidth;
}

void Popup::SetConstrainToViewport(bool constrainToViewport)
{
    constrainToViewport_ = constrainToViewport;
}

void Popup::SetBackgroundFill(const D2D1_COLOR_F &fill)
{
    backgroundFill_ = fill;
    InvalidateVisual();
}

void Popup::SetBorderColor(const D2D1_COLOR_F &border)
{
    borderColor_ = border;
    InvalidateVisual();
}

void Popup::SetCornerRadius(float radius)
{
    cornerRadius_ = std::max(radius, 0.0f);
    InvalidateVisual();
}

void Popup::SetShadowEnabled(bool enabled)
{
    shadowEnabled_ = enabled;
    InvalidateVisual();
}

void Popup::SetShadowPasses(std::vector<ShadowPass> passes)
{
    shadowPasses_ = std::move(passes);
    InvalidateVisual();
}

void Popup::SetTopHighlight(const D2D1_COLOR_F &color)
{
    topHighlight_ = color;
    InvalidateVisual();
}

void Popup::BeginAppearAnimation(double durationMs, float fromOffsetY, float fromScale)
{
    appearStartMs_ = NowMs();
    appearDurationMs_ = durationMs;
    appearOffsetY_ = fromOffsetY;
    appearScale_ = fromScale;
    if (window_)
    {
        window_->RequestAnimationFrame();
    }
}

void Popup::RenderChrome(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (shadowEnabled_)
    {
        if (shadowPasses_.empty() ||
            !shadowCache_.Render(target, bounds_, cornerRadius_, shadowPasses_.data(), shadowPasses_.size()))
        {
            DrawPopupShadow(deviceResources, bounds_, cornerRadius_);
        }
    }

    FillRoundedRect(deviceResources, bounds_, cornerRadius_, backgroundFill_, borderColor_, 1.0f);
    if (topHighlight_.a > 0.0f)
    {
        if (ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(topHighlight_))
        {
            const float inset = std::max(cornerRadius_ - 1.0f, 1.0f);
            target->FillRectangle(
                D2D1::RectF(bounds_.x + inset, bounds_.y + 1.0f, bounds_.x + bounds_.width - inset, bounds_.y + 2.0f),
                brush);
        }
    }
}

SizeF Popup::Measure(const SizeF &availableSize)
{
    const SizeF inner = DeflateSizeLocal(availableSize, padding_);
    if (!child_)
    {
        return {padding_.left + padding_.right, padding_.top + padding_.bottom};
    }

    const SizeF childSize = child_->MeasureInLayout(inner);
    return {childSize.width + padding_.left + padding_.right, childSize.height + padding_.top + padding_.bottom};
}

void Popup::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    if (child_)
    {
        child_->ArrangeInLayout(DeflateRectLocal(bounds_, padding_));
    }
}

void Popup::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    float progress = 1.0f;
    if (appearDurationMs_ > 0.0 && window_)
    {
        const double elapsed = window_->GetFrameTime() - appearStartMs_;
        progress = Easing::EaseOut()(static_cast<float>(std::clamp(elapsed / appearDurationMs_, 0.0, 1.0)));
        if (elapsed < appearDurationMs_)
        {
            window_->RequestAnimationFrame();
        }
        else
        {
            appearDurationMs_ = 0.0;
        }
    }

    const bool animating = progress < 1.0f;
    D2D1_MATRIX_3X2_F previous = D2D1::Matrix3x2F::Identity();
    if (animating)
    {
        if (!layer_ || layerTarget_.Get() != target)
        {
            layer_.Reset();
            layerTarget_ = target;
            target->CreateLayer(nullptr, layer_.GetAddressOf());
        }
        target->GetTransform(&previous);
        const float scale = Lerp(appearScale_, 1.0f, progress);
        const float centerX = bounds_.x + bounds_.width * 0.5f;
        const float centerY = bounds_.y + bounds_.height * 0.5f;
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(centerX, centerY)) *
                             D2D1::Matrix3x2F::Translation(0.0f, appearOffsetY_ * (1.0f - progress)) * previous);
        if (layer_)
        {
            target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                    D2D1::IdentityMatrix(), progress),
                              layer_.Get());
        }
    }

    RenderChrome(deviceResources);
    if (child_)
    {
        child_->Render(deviceResources);
    }

    if (animating)
    {
        if (layer_)
        {
            target->PopLayer();
        }
        target->SetTransform(previous);
    }
}

void Popup::Attach(Window *window)
{
    Visual::Attach(window);
    if (child_)
    {
        child_->Attach(window);
    }
}

Visual *Popup::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    if (child_)
    {
        if (Visual *hit = child_->FindVisualAt(point))
        {
            return hit;
        }
    }

    return this;
}

Visual *Popup::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    return child_ ? child_->FindFocusableAt(point) : nullptr;
}

Visual *Popup::FindFirstFocusableDescendant()
{
    return child_ ? child_->FindFirstFocusableDescendant() : nullptr;
}

bool Popup::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool Popup::KeepsPopupsOpenOnClick() const
{
    return true;
}

void Popup::LayoutOverlay(const SizeF &viewportSize)
{
    const SizeF available = {std::max(viewportSize.width - 24.0f, 0.0f), std::max(viewportSize.height - 24.0f, 0.0f)};
    const SizeF measured = MeasureInLayout(available);
    const float width = matchAnchorWidth_ ? std::max(measured.width, anchorRect_.width) : measured.width;
    const float height = measured.height;

    float x = placement_ == PopupPlacement::RightLeading ? (anchorRect_.x + anchorRect_.width + offsetX_)
                                                         : (anchorRect_.x + offsetX_);
    if (constrainToViewport_ && x + width > viewportSize.width - 8.0f)
    {
        x = std::max(viewportSize.width - width - 8.0f, 8.0f);
    }

    float y = anchorRect_.y + anchorRect_.height + offsetY_;
    if (placement_ == PopupPlacement::AboveLeading)
    {
        y = anchorRect_.y - height - offsetY_;
    }
    else if (placement_ == PopupPlacement::RightLeading)
    {
        y = anchorRect_.y + offsetY_;
    }
    if (constrainToViewport_ && placement_ == PopupPlacement::AboveLeading && y < 8.0f)
    {
        y = anchorRect_.y + anchorRect_.height + offsetY_;
    }
    if (constrainToViewport_ && placement_ == PopupPlacement::BelowLeading && y + height > viewportSize.height - 8.0f)
    {
        const float aboveY = anchorRect_.y - height - offsetY_;
        if (aboveY >= 8.0f)
        {
            y = aboveY;
        }
    }

    if (constrainToViewport_)
    {
        y = std::clamp(y, 8.0f, std::max(viewportSize.height - height - 8.0f, 8.0f));
    }
    ArrangeInLayout({x, y, width, height});
}

PopupHost::PopupHost(std::shared_ptr<Visual> trigger, std::shared_ptr<Popup> popup)
    : trigger_(std::move(trigger)), popup_(std::move(popup))
{
    AdoptChild(trigger_);
}

SizeF PopupHost::Measure(const SizeF &availableSize)
{
    return trigger_ ? trigger_->MeasureInLayout(availableSize) : SizeF{};
}

void PopupHost::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    if (trigger_)
    {
        trigger_->ArrangeInLayout(finalRect);
    }
    if (popup_)
    {
        popup_->SetAnchorRect(bounds_);
    }
}

void PopupHost::Render(DeviceResources &deviceResources)
{
    if (trigger_)
    {
        trigger_->Render(deviceResources);
    }
}

void PopupHost::Attach(Window *window)
{
    Visual::Attach(window);
    if (trigger_)
    {
        trigger_->Attach(window);
    }
    if (popup_)
    {
        popup_->Attach(window);
    }
}

Visual *PopupHost::FindVisualAt(const PointF &point)
{
    return HitTest(point) ? this : nullptr;
}

Visual *PopupHost::FindFocusableAt(const PointF &point)
{
    (void)point;
    return nullptr;
}

Visual *PopupHost::FindFirstFocusableDescendant()
{
    return nullptr;
}

bool PopupHost::HitTest(const PointF &point) const
{
    return trigger_ ? trigger_->HitTest(point) : false;
}

bool PopupHost::OnMouseDown(const POINT &point, WPARAM keyState)
{
    if (!window_)
    {
        return false;
    }

    pressed_ = HitTest(window_->ClientPixelsToDips(point));
    if (pressed_ && trigger_)
    {
        trigger_->OnMouseDown(point, keyState);
    }
    return pressed_;
}

bool PopupHost::OnMouseUp(const POINT &point, WPARAM keyState)
{
    if (!window_ || !pressed_)
    {
        return false;
    }

    const bool shouldToggle = HitTest(window_->ClientPixelsToDips(point));
    if (trigger_)
    {
        trigger_->OnMouseUp(point, keyState);
    }
    pressed_ = false;
    if (shouldToggle)
    {
        TogglePopup();
    }
    return true;
}

HCURSOR PopupHost::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

bool PopupHost::KeepsPopupsOpenOnClick() const
{
    return true;
}

void PopupHost::OpenPopup()
{
    if (open_ || !window_ || !popup_)
    {
        return;
    }

    popup_->SetAnchorRect(bounds_);
    if (Scene *scene = window_->GetScene())
    {
        scene->AddPopup(popup_, [this]() { open_ = false; });
        open_ = true;
        if (Visual *focusTarget = popup_->FindFirstFocusableDescendant())
        {
            window_->FocusVisual(focusTarget);
        }
    }
}

void PopupHost::ClosePopup()
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
}

void PopupHost::TogglePopup()
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

bool PopupHost::IsOpen() const
{
    return open_;
}

ContextMenuHost::ContextMenuHost(std::shared_ptr<Visual> trigger, std::shared_ptr<Popup> popup)
    : trigger_(std::move(trigger)), popup_(std::move(popup))
{
    AdoptChild(trigger_);
}

SizeF ContextMenuHost::Measure(const SizeF &availableSize)
{
    return trigger_ ? trigger_->MeasureInLayout(availableSize) : SizeF{};
}

void ContextMenuHost::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    if (trigger_)
    {
        trigger_->ArrangeInLayout(finalRect);
    }
}

void ContextMenuHost::Render(DeviceResources &deviceResources)
{
    if (trigger_)
    {
        trigger_->Render(deviceResources);
    }
}

void ContextMenuHost::Attach(Window *window)
{
    Visual::Attach(window);
    if (trigger_)
    {
        trigger_->Attach(window);
    }
    if (popup_)
    {
        popup_->Attach(window);
    }
}

Visual *ContextMenuHost::FindVisualAt(const PointF &point)
{
    return HitTest(point) ? this : nullptr;
}

Visual *ContextMenuHost::FindFocusableAt(const PointF &point)
{
    (void)point;
    return nullptr;
}

Visual *ContextMenuHost::FindFirstFocusableDescendant()
{
    return nullptr;
}

bool ContextMenuHost::HitTest(const PointF &point) const
{
    return trigger_ ? trigger_->HitTest(point) : false;
}

bool ContextMenuHost::OnContextMenu(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    if (!HitTest(dipPoint))
    {
        return false;
    }

    OpenPopupAt(dipPoint);
    return true;
}

HCURSOR ContextMenuHost::GetCursor() const
{
    return trigger_ ? trigger_->GetCursor() : LoadCursor(nullptr, IDC_ARROW);
}

bool ContextMenuHost::KeepsPopupsOpenOnClick() const
{
    return true;
}

void ContextMenuHost::OpenPopupAt(const PointF &anchorPoint)
{
    if (!window_ || !popup_)
    {
        return;
    }

    if (Scene *scene = window_->GetScene())
    {
        if (open_)
        {
            scene->RemovePopup(popup_.get(), false);
            open_ = false;
        }

        popup_->SetAnchorRect({anchorPoint.x, anchorPoint.y, 1.0f, 1.0f});
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

void ContextMenuHost::ClosePopup()
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

bool ContextMenuHost::IsOpen() const
{
    return open_;
}
} // namespace msimeui
