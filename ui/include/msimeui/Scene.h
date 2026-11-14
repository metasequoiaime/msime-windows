#pragma once

#include "Layout.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace msimeui
{
class Scene
{
  public:
    struct PopupEntry
    {
        std::shared_ptr<Visual> visual;
        std::function<void()> onClose;
    };

    void SetRoot(std::shared_ptr<Visual> root);
    // Moves the root inside the viewport. The root is laid out in the space left
    // past the offset, so it never extends beyond the viewport's right or bottom
    // edge. Hosts use this when they grow the window left/up around fixed content
    // (a popup needs room off the card) and the content must keep its position on
    // screen.
    void SetRootOffset(const PointF &offset);
    void Attach(Window *window);
    void EnsureLayout(const SizeF &size);
    void Layout(const SizeF &size);
    void AddPopup(std::shared_ptr<Visual> popup, std::function<void()> onClose = {});
    void RemovePopup(Visual *popup, bool notify = true);
    void ClearPopups(bool notify = true);
    bool DismissPopupsForClick(const PointF &point, Visual *target);
    void RelayoutPopups();
    void InvalidateMeasure();
    void InvalidateMeasure(Visual *source);
    void InvalidateArrange();
    void InvalidateArrange(Visual *source);
    void InvalidateVisual();
    // Hosts call DeviceResources::BeginFrame before this, so the animation clock and the
    // next-frame deadline belong to this frame.
    void Render(DeviceResources &deviceResources);
    // When the scene rendered last needs its next frame: the earliest change of an animated
    // image it drew. Empty when nothing on screen animates. Hosts that render on demand
    // schedule one repaint for it (msimeui::Window does so itself).
    std::optional<std::chrono::steady_clock::time_point> GetNextFrameDeadline() const;
    Visual *FindVisualAt(const PointF &point);
    Visual *FindFocusableAt(const PointF &point);
    bool OnMouseWheel(const POINT &point, short delta, WPARAM keyState);

    bool OnTimer(UINT_PTR timerId);

  private:
    std::shared_ptr<Visual> root_;
    std::vector<PopupEntry> popups_;
    Window *window_ = nullptr;
    Visual *measureDirtySource_ = nullptr;
    Visual *arrangeDirtyRoot_ = nullptr;
    PointF rootOffset_ = {};
    SizeF lastLayoutSize_ = {};
    bool hasLastLayoutSize_ = false;
    bool measureDirty_ = true;
    bool arrangeDirty_ = true;
    std::optional<std::chrono::steady_clock::time_point> nextFrameDeadline_;
};
} // namespace msimeui
