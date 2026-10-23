// TreeView: an expandable hierarchy of nodes with guide lines and single selection.
#include "msimeui/Controls.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Theme.h"
#include "msimeui/Window.h"

#include "ControlsInternal.h"

#include <algorithm>
#include <string>
#include <utility>

namespace msimeui
{
using namespace controls_detail;

namespace
{
constexpr float kTreeIndent = 22.0f;
constexpr float kTreeRowLeadingPadding = 8.0f;
constexpr float kTreeGuideLineOffset = 15.0f;
constexpr float kTreeExpanderSize = 14.0f;
constexpr float kTreeTextGapAfterExpander = 10.0f;
} // namespace

TreeView::TreeView(float itemHeight) : itemHeight_(itemHeight)
{
}

void TreeView::InvalidateLayoutCache()
{
    for (auto &entry : visibleNodes_)
    {
        entry.titleLayout.Reset();
        entry.subtitleLayout.Reset();
        entry.titleWidth = -1.0f;
        entry.subtitleWidth = -1.0f;
        entry.fontFamily.clear();
    }
}

void TreeView::AddRoot(Node node)
{
    roots_.push_back(std::move(node));
    BuildVisibleNodes();
    if (!selectedNode_ && !roots_.empty())
    {
        selectedNode_ = &roots_.front();
    }
    InvalidateMeasure();
}

void TreeView::Clear()
{
    roots_.clear();
    visibleNodes_.clear();
    InvalidateLayoutCache();
    pressedNode_ = nullptr;
    selectedNode_ = nullptr;
    InvalidateMeasure();
}

void TreeView::SetOnSelectionChanged(SelectionChangedHandler handler)
{
    onSelectionChanged_ = std::move(handler);
}

SizeF TreeView::Measure(const SizeF &availableSize)
{
    BuildVisibleNodes();
    const float height = visibleNodes_.empty() ? itemHeight_ : itemHeight_ * static_cast<float>(visibleNodes_.size());
    return {availableSize.width, std::min(height, availableSize.height)};
}

void TreeView::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    for (size_t index = 0; index < visibleNodes_.size(); ++index)
    {
        auto &entry = visibleNodes_[index];
        const float rowY = finalRect.y + itemHeight_ * static_cast<float>(index);
        const float rowInset = static_cast<float>(entry.depth) * kTreeIndent;
        const float rowX = finalRect.x + rowInset;
        const float rowWidth = std::max(finalRect.width - rowInset, 0.0f);
        entry.rowRect = {rowX, rowY, rowWidth, itemHeight_ - 6.0f};
        entry.expanderRect = {rowX + kTreeRowLeadingPadding, rowY + 20.0f, kTreeExpanderSize, kTreeExpanderSize};
    }
}

void TreeView::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    IDWriteFactory *factory = deviceResources.GetDWriteFactory();
    if (!target || !factory)
    {
        return;
    }

    ID2D1SolidColorBrush *lineBrush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0xCBD5E1));
    if (!lineBrush)
    {
        return;
    }

    for (size_t index = 0; index < visibleNodes_.size(); ++index)
    {
        auto &entry = visibleNodes_[index];
        const bool selected = entry.node == selectedNode_;
        const bool pressed = pressed_ && entry.node == pressedNode_;
        FillRoundedRect(deviceResources, entry.rowRect, kListItemCornerRadius,
                        pressed ? D2D1::ColorF(0xDBEAFE) : (selected ? D2D1::ColorF(0xEFF6FF) : D2D1::ColorF(0xFFFFFF)),
                        selected ? D2D1::ColorF(0x60A5FA) : D2D1::ColorF(0xD6DCE5), selected || focused_ ? 2.0f : 1.0f);

        const float rowTop = entry.rowRect.y - 3.0f;
        const float rowBottom = entry.rowRect.y + entry.rowRect.height + 3.0f;
        for (size_t ancestorDepth = 0; ancestorDepth < entry.depth; ++ancestorDepth)
        {
            const float guideX = bounds_.x + kTreeGuideLineOffset + static_cast<float>(ancestorDepth) * kTreeIndent;
            target->DrawLine(D2D1::Point2F(guideX, rowTop), D2D1::Point2F(guideX, rowBottom), lineBrush, 1.2f);
        }

        if (entry.node->expanded && !entry.node->children.empty())
        {
            size_t lastDescendantIndex = index;
            for (size_t next = index + 1; next < visibleNodes_.size(); ++next)
            {
                if (visibleNodes_[next].depth <= entry.depth)
                {
                    break;
                }
                lastDescendantIndex = next;
            }

            if (lastDescendantIndex > index)
            {
                const float guideX = entry.expanderRect.x + entry.expanderRect.width * 0.5f;
                const float guideTop = entry.expanderRect.y + entry.expanderRect.height * 0.5f + 6.0f;
                const float guideBottom = visibleNodes_[lastDescendantIndex].rowRect.y +
                                          visibleNodes_[lastDescendantIndex].rowRect.height + 3.0f;
                target->DrawLine(D2D1::Point2F(guideX, guideTop), D2D1::Point2F(guideX, guideBottom), lineBrush, 1.2f);
            }
        }

        const float contentX = entry.rowRect.x + kTreeRowLeadingPadding + kTreeExpanderSize + kTreeTextGapAfterExpander;
        if (!entry.node->children.empty())
        {
            const float centerX = entry.expanderRect.x + entry.expanderRect.width * 0.5f;
            const float centerY = entry.expanderRect.y + entry.expanderRect.height * 0.5f;
            if (entry.node->expanded)
            {
                target->DrawLine(D2D1::Point2F(centerX - 4.0f, centerY - 1.0f), D2D1::Point2F(centerX, centerY + 3.0f),
                                 lineBrush, 1.8f);
                target->DrawLine(D2D1::Point2F(centerX, centerY + 3.0f), D2D1::Point2F(centerX + 4.0f, centerY - 1.0f),
                                 lineBrush, 1.8f);
            }
            else
            {
                target->DrawLine(D2D1::Point2F(centerX - 2.0f, centerY - 4.0f), D2D1::Point2F(centerX + 2.0f, centerY),
                                 lineBrush, 1.8f);
                target->DrawLine(D2D1::Point2F(centerX + 2.0f, centerY), D2D1::Point2F(centerX - 2.0f, centerY + 4.0f),
                                 lineBrush, 1.8f);
            }
        }

        const RectF titleRect = {contentX, entry.rowRect.y + 10.0f,
                                 std::max(entry.rowRect.width - (contentX - entry.rowRect.x) - 18.0f, 0.0f), 22.0f};
        const RectF subtitleRect = {contentX, entry.rowRect.y + 34.0f,
                                    std::max(entry.rowRect.width - (contentX - entry.rowRect.x) - 18.0f, 0.0f), 18.0f};
        const Theme &theme = ThemeManager::GetCurrent();
        if (entry.fontFamily != theme.uiFontFamily || entry.titleWidth != titleRect.width)
        {
            entry.titleLayout = CreateCachedTextLayout(factory, theme.uiFontFamily, entry.node->title, 15.0f,
                                                       DWRITE_FONT_WEIGHT_SEMI_BOLD, std::max(titleRect.width, 1.0f),
                                                       std::max(titleRect.height, 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING,
                                                       DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_NO_WRAP);
            entry.titleWidth = titleRect.width;
            entry.fontFamily = theme.uiFontFamily;
        }
        if (entry.fontFamily != theme.uiFontFamily || entry.subtitleWidth != subtitleRect.width)
        {
            entry.subtitleLayout = CreateCachedTextLayout(
                factory, theme.uiFontFamily, entry.node->subtitle, 13.0f, DWRITE_FONT_WEIGHT_NORMAL,
                std::max(subtitleRect.width, 1.0f), std::max(subtitleRect.height, 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_NO_WRAP);
            entry.subtitleWidth = subtitleRect.width;
            entry.fontFamily = theme.uiFontFamily;
        }

        ID2D1SolidColorBrush *titleBrush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0x0F172A));
        ID2D1SolidColorBrush *subtitleBrush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0x64748B));
        if (entry.titleLayout && titleBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(titleRect.x, titleRect.y), entry.titleLayout.Get(), titleBrush,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (entry.subtitleLayout && subtitleBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(subtitleRect.x, subtitleRect.y), entry.subtitleLayout.Get(),
                                   subtitleBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

bool TreeView::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool TreeView::IsFocusable() const
{
    return true;
}

void TreeView::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool TreeView::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    VisibleNode *hit = HitTestVisibleNode(dipPoint);
    if (!hit)
    {
        return false;
    }

    pressed_ = true;
    pressedNode_ = hit->node;
    pressedExpander_ = !hit->node->children.empty() && PointInRect(hit->expanderRect, dipPoint);
    InvalidateVisual();
    return true;
}

bool TreeView::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    VisibleNode *hit = HitTestVisibleNode(dipPoint);
    Node *pressedNode = pressedNode_;
    const bool pressedExpander = pressedExpander_;
    pressed_ = false;
    pressedNode_ = nullptr;
    pressedExpander_ = false;

    if (hit && hit->node == pressedNode)
    {
        if (pressedExpander && !hit->node->children.empty() && PointInRect(hit->expanderRect, dipPoint))
        {
            hit->node->expanded = !hit->node->expanded;
            BuildVisibleNodes();
            InvalidateMeasure();
        }
        else
        {
            SelectNode(hit->node);
        }
    }

    InvalidateVisual();
    return true;
}

HCURSOR TreeView::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

void TreeView::BuildVisibleNodes()
{
    visibleNodes_.clear();
    for (auto &root : roots_)
    {
        AppendVisibleNodes(root, 0);
    }
}

void TreeView::AppendVisibleNodes(Node &node, size_t depth)
{
    visibleNodes_.push_back({&node, depth, {}, {}});
    if (!node.expanded)
    {
        return;
    }

    for (auto &child : node.children)
    {
        AppendVisibleNodes(child, depth + 1);
    }
}

TreeView::VisibleNode *TreeView::HitTestVisibleNode(const PointF &point)
{
    for (auto &entry : visibleNodes_)
    {
        if (PointInRect(entry.rowRect, point))
        {
            return &entry;
        }
    }

    return nullptr;
}

const TreeView::VisibleNode *TreeView::HitTestVisibleNode(const PointF &point) const
{
    for (const auto &entry : visibleNodes_)
    {
        if (PointInRect(entry.rowRect, point))
        {
            return &entry;
        }
    }

    return nullptr;
}

void TreeView::SelectNode(Node *node)
{
    if (!node || selectedNode_ == node)
    {
        return;
    }

    selectedNode_ = node;
    if (onSelectionChanged_)
    {
        // Read everything the call needs first: the handler may destroy this view.
        const std::wstring title = selectedNode_->title;
        const auto handler = onSelectionChanged_;
        handler(title);
    }
}
} // namespace msimeui
