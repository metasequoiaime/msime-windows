#include "msimeui/Scene.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Window.h"

#include <algorithm>

namespace msimeui
{
namespace
{
Visual *FindCommonAncestor(Visual *lhs, Visual *rhs)
{
    if (!lhs)
    {
        return rhs;
    }
    if (!rhs)
    {
        return lhs;
    }
    if (lhs == rhs)
    {
        return lhs;
    }

    for (Visual *left = lhs; left; left = left->GetParentVisual())
    {
        for (Visual *right = rhs; right; right = right->GetParentVisual())
        {
            if (left == right)
            {
                return left;
            }
        }
    }

    return lhs;
}

bool RemeasureDirtyPath(Visual *source, Visual *root)
{
    if (!source || !root)
    {
        return false;
    }

    std::vector<Visual *> path;
    for (Visual *current = source; current; current = current->GetParentVisual())
    {
        path.push_back(current);
        if (current == root)
        {
            break;
        }
    }

    if (path.empty() || path.back() != root)
    {
        return false;
    }

    for (Visual *current : path)
    {
        if (!current->HasMeasureSlot())
        {
            return false;
        }
    }

    for (Visual *current : path)
    {
        current->MeasureInLayout(current->GetLastMeasureAvailableSize());
    }

    return true;
}
} // namespace

void Scene::SetRoot(std::shared_ptr<Visual> root)
{
    root_ = std::move(root);
    if (root_ && window_)
    {
        root_->Attach(window_);
    }
    measureDirty_ = true;
    arrangeDirty_ = true;
    measureDirtySource_ = root_.get();
    arrangeDirtyRoot_ = root_.get();
}

void Scene::SetRootOffset(const PointF &offset)
{
    if (rootOffset_.x == offset.x && rootOffset_.y == offset.y)
    {
        return;
    }
    rootOffset_ = offset;
    arrangeDirty_ = true;
    arrangeDirtyRoot_ = root_.get();
    InvalidateVisual();
}

void Scene::Attach(Window *window)
{
    window_ = window;
    if (root_)
    {
        root_->Attach(window);
    }
    for (auto &popup : popups_)
    {
        if (popup.visual)
        {
            popup.visual->Attach(window);
        }
    }
}

void Scene::EnsureLayout(const SizeF &size)
{
    const bool sizeChanged =
        !hasLastLayoutSize_ || lastLayoutSize_.width != size.width || lastLayoutSize_.height != size.height;
    if (sizeChanged || measureDirty_ || arrangeDirty_)
    {
        Layout(size);
    }
}

void Scene::Layout(const SizeF &size)
{
    const bool sizeChanged =
        !hasLastLayoutSize_ || lastLayoutSize_.width != size.width || lastLayoutSize_.height != size.height;
    const bool needsMeasure = sizeChanged || measureDirty_ || !hasLastLayoutSize_;
    const bool needsArrange = needsMeasure || sizeChanged || arrangeDirty_ || !hasLastLayoutSize_;

    if (!root_)
    {
        lastLayoutSize_ = size;
        hasLastLayoutSize_ = true;
        measureDirty_ = false;
        arrangeDirty_ = false;
        measureDirtySource_ = nullptr;
        arrangeDirtyRoot_ = nullptr;
        return;
    }

    const SizeF innerSize = size;
    if (needsMeasure)
    {
        const bool canUseDirtyPath = !sizeChanged && measureDirtySource_ && measureDirtySource_ != root_.get();
        if (!canUseDirtyPath || !RemeasureDirtyPath(measureDirtySource_, root_.get()))
        {
            root_->MeasureInLayout(innerSize);
        }
    }
    if (needsArrange)
    {
        const RectF rootSlot = {rootOffset_.x, rootOffset_.y, innerSize.width, innerSize.height};
        if (!needsMeasure && !sizeChanged && arrangeDirtyRoot_ && arrangeDirtyRoot_ != root_.get() &&
            arrangeDirtyRoot_->HasArrangeSlot())
        {
            arrangeDirtyRoot_->ArrangeInLayout(arrangeDirtyRoot_->lastArrangeRect_);
        }
        else
        {
            root_->ArrangeInLayout(rootSlot);
        }
    }
    lastLayoutSize_ = size;
    hasLastLayoutSize_ = true;
    measureDirty_ = false;
    arrangeDirty_ = false;
    measureDirtySource_ = nullptr;
    arrangeDirtyRoot_ = nullptr;

    for (auto &popup : popups_)
    {
        if (popup.visual)
        {
            popup.visual->LayoutOverlay(size);
        }
    }
}

void Scene::AddPopup(std::shared_ptr<Visual> popup, std::function<void()> onClose)
{
    if (!popup)
    {
        return;
    }

    for (const auto &entry : popups_)
    {
        if (entry.visual == popup)
        {
            return;
        }
    }

    if (window_)
    {
        popup->Attach(window_);
    }
    popups_.push_back({std::move(popup), std::move(onClose)});
    arrangeDirty_ = true;
    InvalidateVisual();
}

void Scene::RemovePopup(Visual *popup, bool notify)
{
    if (!popup)
    {
        return;
    }

    auto it = std::find_if(popups_.begin(), popups_.end(),
                           [popup](const PopupEntry &entry) { return entry.visual.get() == popup; });
    if (it == popups_.end())
    {
        return;
    }

    std::function<void()> onClose = std::move(it->onClose);
    popups_.erase(it);
    if (notify && onClose)
    {
        onClose();
    }
    InvalidateVisual();
}

void Scene::ClearPopups(bool notify)
{
    if (popups_.empty())
    {
        return;
    }

    auto popups = std::move(popups_);
    popups_.clear();
    if (notify)
    {
        for (auto &entry : popups)
        {
            if (entry.onClose)
            {
                entry.onClose();
            }
        }
    }
    InvalidateVisual();
}

bool Scene::DismissPopupsForClick(const PointF &point, Visual *target)
{
    if (popups_.empty())
    {
        return false;
    }

    if (target && target->KeepsPopupsOpenOnClick())
    {
        return false;
    }

    for (auto it = popups_.rbegin(); it != popups_.rend(); ++it)
    {
        if (it->visual && it->visual->HitTest(point))
        {
            return false;
        }
    }

    ClearPopups(true);
    return true;
}

void Scene::RelayoutPopups()
{
    if (!hasLastLayoutSize_)
    {
        return;
    }

    for (auto &popup : popups_)
    {
        if (popup.visual)
        {
            popup.visual->LayoutOverlay(lastLayoutSize_);
        }
    }

    InvalidateVisual();
}

void Scene::InvalidateMeasure()
{
    measureDirty_ = true;
    arrangeDirty_ = true;
    measureDirtySource_ = root_.get();
    arrangeDirtyRoot_ = root_.get();
    InvalidateVisual();
}

void Scene::InvalidateMeasure(Visual *source)
{
    measureDirty_ = true;
    arrangeDirty_ = true;
    if (!source || !root_)
    {
        measureDirtySource_ = root_.get();
        arrangeDirtyRoot_ = root_.get();
    }
    else if (!measureDirtySource_)
    {
        measureDirtySource_ = source;
        arrangeDirtyRoot_ = source;
    }
    else
    {
        measureDirtySource_ = FindCommonAncestor(measureDirtySource_, source);
        arrangeDirtyRoot_ = FindCommonAncestor(arrangeDirtyRoot_, source);
    }
    InvalidateVisual();
}

void Scene::InvalidateArrange()
{
    arrangeDirty_ = true;
    arrangeDirtyRoot_ = root_.get();
    InvalidateVisual();
}

void Scene::InvalidateArrange(Visual *source)
{
    arrangeDirty_ = true;
    if (!source || !root_)
    {
        arrangeDirtyRoot_ = root_.get();
    }
    else if (!arrangeDirtyRoot_)
    {
        arrangeDirtyRoot_ = source;
    }
    else
    {
        arrangeDirtyRoot_ = FindCommonAncestor(arrangeDirtyRoot_, source);
    }
    InvalidateVisual();
}

void Scene::InvalidateVisual()
{
    if (window_)
    {
        window_->Invalidate();
    }
}

void Scene::Render(DeviceResources &deviceResources)
{
    if (root_)
    {
        root_->Render(deviceResources);
    }
    for (const auto &popup : popups_)
    {
        if (popup.visual)
        {
            popup.visual->Render(deviceResources);
        }
    }
    nextFrameDeadline_ = deviceResources.GetNextAnimationDeadline();
}

std::optional<std::chrono::steady_clock::time_point> Scene::GetNextFrameDeadline() const
{
    return nextFrameDeadline_;
}

Visual *Scene::FindVisualAt(const PointF &point)
{
    for (auto it = popups_.rbegin(); it != popups_.rend(); ++it)
    {
        if (it->visual)
        {
            if (Visual *hit = it->visual->FindVisualAt(point))
            {
                return hit;
            }
        }
    }
    return root_ ? root_->FindVisualAt(point) : nullptr;
}

Visual *Scene::FindFocusableAt(const PointF &point)
{
    for (auto it = popups_.rbegin(); it != popups_.rend(); ++it)
    {
        if (it->visual)
        {
            if (Visual *focusable = it->visual->FindFocusableAt(point))
            {
                return focusable;
            }
        }
    }
    return root_ ? root_->FindFocusableAt(point) : nullptr;
}

bool Scene::OnMouseWheel(const POINT &point, short delta, WPARAM keyState)
{
    for (auto it = popups_.rbegin(); it != popups_.rend(); ++it)
    {
        if (it->visual && it->visual->OnMouseWheel(point, delta, keyState))
        {
            return true;
        }
    }
    if (!root_)
    {
        return false;
    }

    // Offer the wheel to the visual under the cursor first and bubble it up through its
    // ancestors, so the innermost scrollable area wins, like in a browser. The root is
    // the last ancestor, which keeps the old root-only behavior as the fallback.
    Visual *start = window_ ? FindVisualAt(window_->ClientPixelsToDips(point)) : nullptr;
    for (Visual *visual = start; visual; visual = visual->GetParentVisual())
    {
        if (visual->OnMouseWheel(point, delta, keyState))
        {
            return true;
        }
        if (visual == root_.get())
        {
            return false;
        }
    }
    return root_->OnMouseWheel(point, delta, keyState);
}

bool Scene::OnTimer(UINT_PTR timerId)
{
    return root_ ? root_->OnTimer(timerId) : false;
}
} // namespace msimeui
