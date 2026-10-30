#include "msimeui/Layout.h"

#include "msimeui/Animation.h"
#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/Theme.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <cmath>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <dwrite_1.h>
#include <limits>
#include <numeric>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace msimeui
{
using Microsoft::WRL::ComPtr;

namespace
{
IDWriteFactory *GetSharedDWriteFactory()
{
    static ComPtr<IDWriteFactory> factory;
    if (!factory)
    {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown **>(factory.GetAddressOf()));
    }

    return factory.Get();
}

constexpr float kPreeditCaretBarWidth = 1.25f;
constexpr float kPreeditCaretSideAir = 0.85f;
constexpr float kPreeditCaretEndAir = 1.5f;
constexpr float kPreeditCaretInsertGap = kPreeditCaretBarWidth + kPreeditCaretSideAir * 2.0f;
constexpr wchar_t kCaretSlotChar = L'\uFFFC';

class CaretGapInlineObject final : public IDWriteInlineObject
{
  public:
    CaretGapInlineObject(float width, float height, float baseline)
        : width_(width), height_(height), baseline_(baseline)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
    {
        if (ppvObject == nullptr)
        {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject))
        {
            *ppvObject = static_cast<IDWriteInlineObject *>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&refCount_);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = InterlockedDecrement(&refCount_);
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Draw(void *, IDWriteTextRenderer *, FLOAT, FLOAT, BOOL, BOOL, IUnknown *) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS *metrics) override
    {
        if (metrics == nullptr)
        {
            return E_POINTER;
        }
        metrics->width = width_;
        metrics->height = height_;
        metrics->baseline = baseline_;
        metrics->supportsSideways = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS *overhangs) override
    {
        if (overhangs == nullptr)
        {
            return E_POINTER;
        }
        *overhangs = {};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION *breakConditionBefore,
                                                 DWRITE_BREAK_CONDITION *breakConditionAfter) override
    {
        if (breakConditionBefore == nullptr || breakConditionAfter == nullptr)
        {
            return E_POINTER;
        }
        *breakConditionBefore = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
        *breakConditionAfter = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
        return S_OK;
    }

  private:
    ULONG refCount_ = 1;
    float width_ = 0.0f;
    float height_ = 0.0f;
    float baseline_ = 0.0f;
};

bool InsertCaretSlot(IDWriteTextLayout *layout, UINT32 caretIndex, float fontSize)
{
    if (layout == nullptr)
    {
        return false;
    }
    ComPtr<CaretGapInlineObject> slot;
    slot.Attach(new CaretGapInlineObject(kPreeditCaretInsertGap, fontSize * 1.2f, fontSize * 0.96f));
    return SUCCEEDED(layout->SetInlineObject(slot.Get(), DWRITE_TEXT_RANGE{caretIndex, 1}));
}

SizeF DeflateSize(const SizeF &size, const Thickness &thickness)
{
    return {std::max(size.width - thickness.left - thickness.right, 0.0f),
            std::max(size.height - thickness.top - thickness.bottom, 0.0f)};
}

RectF DeflateRect(const RectF &rect, const Thickness &thickness)
{
    return {rect.x + thickness.left, rect.y + thickness.top,
            std::max(rect.width - thickness.left - thickness.right, 0.0f),
            std::max(rect.height - thickness.top - thickness.bottom, 0.0f)};
}

float ClampWithOptionalMax(float value, float minValue, float maxValue)
{
    const float lowerBound = std::max(minValue, 0.0f);
    if (maxValue >= 0.0f)
    {
        return std::clamp(value, lowerBound, std::max(lowerBound, maxValue));
    }

    return std::max(value, lowerBound);
}

float ComputeAlignedStart(float origin, float available, float content, HorizontalAlignment alignment)
{
    if (alignment == HorizontalAlignment::Center)
    {
        return origin + std::max((available - content) * 0.5f, 0.0f);
    }
    if (alignment == HorizontalAlignment::Trailing)
    {
        return origin + std::max(available - content, 0.0f);
    }

    return origin;
}

float ComputeAlignedStart(float origin, float available, float content, VerticalAlignment alignment)
{
    if (alignment == VerticalAlignment::Center)
    {
        return origin + std::max((available - content) * 0.5f, 0.0f);
    }
    if (alignment == VerticalAlignment::Trailing)
    {
        return origin + std::max(available - content, 0.0f);
    }

    return origin;
}

// Children may paint a little outside their bounds (shadows, focus rings, indicators),
// so culling keeps anything within this distance of the visible area.
constexpr float kCullingOverdraw = 24.0f;

bool ShouldRenderChild(const DeviceResources &deviceResources, const Visual &child)
{
    const RectF &bounds = child.GetBounds();
    return deviceResources.IsVisible({bounds.x - kCullingOverdraw, bounds.y - kCullingOverdraw,
                                      bounds.width + kCullingOverdraw * 2.0f, bounds.height + kCullingOverdraw * 2.0f});
}

bool PointInRect(const RectF &rect, const PointF &point)
{
    return point.x >= rect.x && point.x <= (rect.x + rect.width) && point.y >= rect.y &&
           point.y <= (rect.y + rect.height);
}

bool IsSameSize(const SizeF &lhs, const SizeF &rhs)
{
    return lhs.width == rhs.width && lhs.height == rhs.height;
}

bool IsSameRect(const RectF &lhs, const RectF &rhs)
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

void DrawLayeredMistShadow(ID2D1RenderTarget *target, const RectF &bounds, float radius, float scale, float opacity)
{
    const float s = std::max(scale, 0.15f);
    // One brush recolored per layer instead of twenty short-lived ones.
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.0f), brush.GetAddressOf())))
    {
        return;
    }
    for (int i = 1; i <= 20; ++i)
    {
        const float t = static_cast<float>(i) / 20.0f;
        const float spread = 1.15f * static_cast<float>(i) * s;
        const float offsetY = 0.4f * static_cast<float>(i) * s;
        const float alpha = 0.14f * (1.0f - t) * (1.0f - t) * opacity;
        brush->SetColor(D2D1::ColorF(0, 0, 0, alpha));
        const auto rounded = D2D1::RoundedRect(D2D1::RectF(bounds.x - spread, bounds.y - spread + offsetY,
                                                           bounds.x + bounds.width + spread,
                                                           bounds.y + bounds.height + spread + offsetY),
                                               radius + spread, radius + spread);
        target->FillRoundedRectangle(rounded, brush.Get());
    }
}

// One gaussian-blurred rounded-rect pass shared by the built-in and explicit-pass card
// shadows; stdDeviation/alpha/offset are already-scaled absolute values.
bool DrawGaussianShadowPass(ID2D1RenderTarget *target, ID2D1DeviceContext *dc, const RectF &bounds, float radius,
                            float stdDeviation, float alpha, float offsetX, float offsetY)
{
    const float pad = stdDeviation * 3.0f + 4.0f;
    const D2D1_SIZE_F bitmapSize = {bounds.width + pad * 2.0f, bounds.height + pad * 2.0f};
    if (bitmapSize.width < 2.0f || bitmapSize.height < 2.0f)
    {
        return false;
    }

    ComPtr<ID2D1BitmapRenderTarget> compatible;
    if (FAILED(target->CreateCompatibleRenderTarget(bitmapSize, compatible.GetAddressOf())))
    {
        return false;
    }

    compatible->BeginDraw();
    compatible->Clear(D2D1::ColorF(0, 0.0f));
    ComPtr<ID2D1SolidColorBrush> fill;
    if (FAILED(compatible->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, alpha), fill.GetAddressOf())))
    {
        compatible->EndDraw();
        return false;
    }
    const auto shape =
        D2D1::RoundedRect(D2D1::RectF(pad, pad, pad + bounds.width, pad + bounds.height), radius, radius);
    compatible->FillRoundedRectangle(shape, fill.Get());
    if (FAILED(compatible->EndDraw()))
    {
        return false;
    }

    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(compatible->GetBitmap(bitmap.GetAddressOf())))
    {
        return false;
    }

    ComPtr<ID2D1Effect> blur;
    if (FAILED(dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.GetAddressOf())))
    {
        return false;
    }
    blur->SetInput(0, bitmap.Get());
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, stdDeviation);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY);
    dc->DrawImage(blur.Get(), D2D1::Point2F(bounds.x - pad + offsetX, bounds.y - pad + offsetY));
    blur->SetInput(0, nullptr);
    return true;
}

// Built-in Win11-style diffuse shadow: ambient/mid/contact gaussian passes. Kept here so
// the default path and the explicit-pass path share one rendering routine.
constexpr ShadowPass kMistShadowPasses[] = {
    {11.0f, 0.42f, 0.0f, 3.0f},
    {6.0f, 0.30f, 0.0f, 4.0f},
    {2.8f, 0.48f, 0.0f, 3.0f},
};

// Draws each pass through DrawGaussianShadowPass. scale only shrinks the shadow geometry,
// opacity dims every alpha uniformly. Returns true when the caller's shadow is fully
// handled; false means nothing was drawn and the caller may fall back to the layered
// approximation.
bool DrawGaussianPasses(ID2D1RenderTarget *target, const RectF &bounds, float radius, const ShadowPass *passes,
                        size_t count, float scale, float opacity)
{
    if (!target || bounds.width <= 0.0f || bounds.height <= 0.0f)
    {
        return false;
    }

    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(target->QueryInterface(IID_PPV_ARGS(dc.GetAddressOf()))))
    {
        // No device context: degrade to the layered approximation. It does not
        // reproduce the individual passes, only the overall soft-shadow look.
        DrawLayeredMistShadow(target, bounds, radius, scale, opacity);
        return true;
    }

    const float s = std::max(scale, 0.15f);
    bool drew = false;
    for (size_t i = 0; i < count; ++i)
    {
        if (DrawGaussianShadowPass(target, dc.Get(), bounds, radius, passes[i].sigma * s, passes[i].alpha * opacity,
                                   passes[i].offsetX * s, passes[i].offsetY * s))
        {
            drew = true;
        }
    }
    return drew;
}

void DrawWin11WindowShadow(ID2D1RenderTarget *target, const RectF &bounds, float radius, float scale, float opacity)
{
    if (!target || bounds.width <= 0.0f || bounds.height <= 0.0f)
    {
        return;
    }
    if (!DrawGaussianPasses(target, bounds, radius, kMistShadowPasses, std::size(kMistShadowPasses), scale, opacity))
    {
        DrawLayeredMistShadow(target, bounds, radius, scale, opacity);
    }
}

// Width of the strip stretched across the straight middle of a nine-sliced shadow,
// and how far its cuts keep inside the straight part so that samples taken just
// across a cut still read the same values.
constexpr float kShadowStretchSpan = 4.0f;
constexpr float kShadowStretchGuard = 2.0f;

// Where one axis of the cached shadow bitmap is cut and where each piece lands.
// The end pieces are drawn 1:1 and the middle strip covers whatever is left.
struct ShadowAxisSlices
{
    float source[4] = {};
    float dest[4] = {};
    size_t pieces = 1;
};

ShadowAxisSlices SliceShadowAxis(float bitmapExtent, float destExtent, float cut)
{
    ShadowAxisSlices slices;
    if (bitmapExtent == destExtent)
    {
        slices.source[1] = slices.dest[1] = bitmapExtent;
        return slices;
    }
    const float tail = bitmapExtent - cut - kShadowStretchSpan;
    slices.source[1] = slices.dest[1] = cut;
    slices.source[2] = cut + kShadowStretchSpan;
    slices.source[3] = bitmapExtent;
    slices.dest[2] = destExtent - tail;
    slices.dest[3] = destExtent;
    slices.pieces = 3;
    return slices;
}
} // namespace

void Visual::Attach(Window *window)
{
    window_ = window;
}

bool Visual::HitTest(const PointF &point) const
{
    return point.x >= bounds_.x && point.x < (bounds_.x + bounds_.width) && point.y >= bounds_.y &&
           point.y < (bounds_.y + bounds_.height);
}

Visual *Visual::FindVisualAt(const PointF &point)
{
    return HitTest(point) ? this : nullptr;
}

Visual *Visual::FindFocusableAt(const PointF &point)
{
    if (HitTest(point) && IsFocusable())
    {
        return this;
    }

    return nullptr;
}

Visual *Visual::FindFirstFocusableDescendant()
{
    return IsFocusable() ? this : nullptr;
}

bool Visual::IsFocusable() const
{
    return false;
}

void Visual::OnFocusChanged(bool focused)
{
    (void)focused;
}

bool Visual::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)point;
    (void)keyState;
    return false;
}

bool Visual::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)point;
    (void)keyState;
    return false;
}

bool Visual::OnMouseMove(const POINT &point, WPARAM keyState)
{
    (void)point;
    (void)keyState;
    return false;
}

void Visual::OnMouseEnter()
{
}

void Visual::OnMouseLeave()
{
}

bool Visual::OnContextMenu(const POINT &point, WPARAM keyState)
{
    (void)point;
    (void)keyState;
    return false;
}

bool Visual::OnMouseWheel(const POINT &point, short delta, WPARAM keyState)
{
    (void)point;
    (void)delta;
    (void)keyState;
    return false;
}

bool Visual::OnKeyDown(WPARAM key, LPARAM lParam)
{
    (void)key;
    (void)lParam;
    return false;
}

bool Visual::OnChar(wchar_t ch, LPARAM lParam)
{
    (void)ch;
    (void)lParam;
    return false;
}

bool Visual::OnTimer(UINT_PTR timerId)
{
    (void)timerId;
    return false;
}

HCURSOR Visual::GetCursor() const
{
    return LoadCursor(nullptr, IDC_ARROW);
}

void Visual::LayoutOverlay(const SizeF &viewportSize)
{
    (void)viewportSize;
}

bool Visual::KeepsPopupsOpenOnClick() const
{
    return false;
}

void Visual::InvalidateMeasure()
{
    BubbleMeasureInvalidation(this);
}

void Visual::InvalidateArrange()
{
    BubbleArrangeInvalidation(this);
}

void Visual::InvalidateVisual()
{
    if (window_)
    {
        if (bounds_.width > 0.0f && bounds_.height > 0.0f)
        {
            window_->Invalidate(bounds_);
        }
        else
        {
            window_->Invalidate();
        }
    }
}

void Visual::SetMargin(float uniform)
{
    SetMargin({uniform, uniform, uniform, uniform});
}

void Visual::SetMargin(Thickness margin)
{
    margin_ = margin;
    InvalidateMeasure();
}

const Thickness &Visual::GetMargin() const
{
    return margin_;
}

void Visual::SetPadding(float uniform)
{
    SetPadding({uniform, uniform, uniform, uniform});
}

void Visual::SetPadding(Thickness padding)
{
    padding_ = padding;
    InvalidateMeasure();
}

const Thickness &Visual::GetPadding() const
{
    return padding_;
}

void Visual::SetWidth(float width)
{
    explicitWidth_ = std::max(width, 0.0f);
    InvalidateMeasure();
}

void Visual::SetHeight(float height)
{
    explicitHeight_ = std::max(height, 0.0f);
    InvalidateMeasure();
}

void Visual::SetMinWidth(float width)
{
    minWidth_ = std::max(width, 0.0f);
    InvalidateMeasure();
}

void Visual::SetMinHeight(float height)
{
    minHeight_ = std::max(height, 0.0f);
    InvalidateMeasure();
}

void Visual::SetMaxWidth(float width)
{
    maxWidth_ = width >= 0.0f ? width : -1.0f;
    InvalidateMeasure();
}

void Visual::SetMaxHeight(float height)
{
    maxHeight_ = height >= 0.0f ? height : -1.0f;
    InvalidateMeasure();
}

void Visual::ClearWidth()
{
    explicitWidth_ = -1.0f;
    InvalidateMeasure();
}

void Visual::ClearHeight()
{
    explicitHeight_ = -1.0f;
    InvalidateMeasure();
}

void Visual::ClearMinWidth()
{
    minWidth_ = 0.0f;
    InvalidateMeasure();
}

void Visual::ClearMinHeight()
{
    minHeight_ = 0.0f;
    InvalidateMeasure();
}

void Visual::ClearMaxWidth()
{
    maxWidth_ = -1.0f;
    InvalidateMeasure();
}

void Visual::ClearMaxHeight()
{
    maxHeight_ = -1.0f;
    InvalidateMeasure();
}

void Visual::SetHorizontalAlignment(HorizontalAlignment alignment)
{
    horizontalAlignment_ = alignment;
    InvalidateArrange();
}

void Visual::SetVerticalAlignment(VerticalAlignment alignment)
{
    verticalAlignment_ = alignment;
    InvalidateArrange();
}

SizeF Visual::MeasureInLayout(const SizeF &availableSize)
{
    if (IsMeasureCacheValid(availableSize))
    {
        return measuredOuterSize_;
    }

    const SizeF innerAvailable = DeflateSize(availableSize, margin_);
    desiredSize_ = Measure(innerAvailable);
    if (HasExplicitWidth())
    {
        desiredSize_.width = explicitWidth_;
    }
    if (HasExplicitHeight())
    {
        desiredSize_.height = explicitHeight_;
    }

    desiredSize_.width = ClampWithOptionalMax(desiredSize_.width, minWidth_, maxWidth_);
    desiredSize_.height = ClampWithOptionalMax(desiredSize_.height, minHeight_, maxHeight_);
    desiredSize_.width = std::min(desiredSize_.width, innerAvailable.width);
    desiredSize_.height = std::min(desiredSize_.height, innerAvailable.height);

    measuredOuterSize_ = {desiredSize_.width + margin_.left + margin_.right,
                          desiredSize_.height + margin_.top + margin_.bottom};
    lastMeasureAvailableSize_ = availableSize;
    measureDirty_ = false;
    hasMeasureCache_ = true;
    return measuredOuterSize_;
}

void Visual::ArrangeInLayout(const RectF &finalRect)
{
    if (IsArrangeCacheValid(finalRect))
    {
        return;
    }

    const RectF innerRect = DeflateRect(finalRect, margin_);

    float arrangedWidth = desiredSize_.width;
    if (HasExplicitWidth())
    {
        arrangedWidth = std::min(explicitWidth_, innerRect.width);
    }
    else if (horizontalAlignment_ == HorizontalAlignment::Stretch)
    {
        arrangedWidth = innerRect.width;
    }
    else
    {
        arrangedWidth = std::min(arrangedWidth, innerRect.width);
    }
    arrangedWidth = ClampWithOptionalMax(arrangedWidth, minWidth_, maxWidth_);
    arrangedWidth = std::min(arrangedWidth, innerRect.width);

    float arrangedHeight = desiredSize_.height;
    if (HasExplicitHeight())
    {
        arrangedHeight = std::min(explicitHeight_, innerRect.height);
    }
    else if (verticalAlignment_ == VerticalAlignment::Stretch)
    {
        arrangedHeight = innerRect.height;
    }
    else
    {
        arrangedHeight = std::min(arrangedHeight, innerRect.height);
    }
    arrangedHeight = ClampWithOptionalMax(arrangedHeight, minHeight_, maxHeight_);
    arrangedHeight = std::min(arrangedHeight, innerRect.height);

    float arrangedX = innerRect.x;
    if (horizontalAlignment_ == HorizontalAlignment::Center)
    {
        arrangedX += (innerRect.width - arrangedWidth) * 0.5f;
    }
    else if (horizontalAlignment_ == HorizontalAlignment::Trailing)
    {
        arrangedX += innerRect.width - arrangedWidth;
    }

    float arrangedY = innerRect.y;
    if (verticalAlignment_ == VerticalAlignment::Center)
    {
        arrangedY += (innerRect.height - arrangedHeight) * 0.5f;
    }
    else if (verticalAlignment_ == VerticalAlignment::Trailing)
    {
        arrangedY += innerRect.height - arrangedHeight;
    }

    Arrange({arrangedX, arrangedY, std::max(arrangedWidth, 0.0f), std::max(arrangedHeight, 0.0f)});
    lastArrangeRect_ = finalRect;
    arrangeDirty_ = false;
    hasArrangeCache_ = true;
}

const RectF &Visual::GetBounds() const
{
    return bounds_;
}

bool Visual::HasMeasureSlot() const
{
    return hasMeasureCache_;
}

bool Visual::HasArrangeSlot() const
{
    return hasArrangeCache_;
}

const SizeF &Visual::GetLastMeasureAvailableSize() const
{
    return lastMeasureAvailableSize_;
}

Visual *Visual::GetParentVisual() const
{
    return parent_;
}

void Visual::BubbleMeasureInvalidation(Visual *source)
{
    MarkMeasureDirty();
    if (parent_)
    {
        parent_->BubbleMeasureInvalidation(source);
        return;
    }

    if (window_)
    {
        window_->InvalidateMeasure(source);
    }
}

void Visual::BubbleArrangeInvalidation(Visual *source)
{
    MarkArrangeDirty();
    if (parent_)
    {
        parent_->BubbleArrangeInvalidation(source);
        return;
    }

    if (window_)
    {
        window_->InvalidateArrange(source);
    }
}

void Visual::MarkMeasureDirty()
{
    measureDirty_ = true;
    arrangeDirty_ = true;
    hasMeasureCache_ = false;
    hasArrangeCache_ = false;
}

void Visual::MarkArrangeDirty()
{
    arrangeDirty_ = true;
    hasArrangeCache_ = false;
}

bool Visual::IsMeasureCacheValid(const SizeF &availableSize) const
{
    return hasMeasureCache_ && !measureDirty_ && IsSameSize(lastMeasureAvailableSize_, availableSize);
}

bool Visual::IsArrangeCacheValid(const RectF &finalRect) const
{
    return hasArrangeCache_ && !arrangeDirty_ && IsSameRect(lastArrangeRect_, finalRect);
}

bool Visual::HasExplicitWidth() const
{
    return explicitWidth_ >= 0.0f;
}

void Visual::AdoptChild(const std::shared_ptr<Visual> &child)
{
    if (!child)
    {
        return;
    }

    child->SetParent(this);
    if (window_)
    {
        child->Attach(window_);
    }
}

void Visual::ReleaseChild(const std::shared_ptr<Visual> &child)
{
    if (child && child->GetParent() == this)
    {
        child->SetParent(nullptr);
    }
}

bool Visual::HasExplicitHeight() const
{
    return explicitHeight_ >= 0.0f;
}

void Visual::SetParent(Visual *parent)
{
    parent_ = parent;
}

Visual *Visual::GetParent() const
{
    return parent_;
}

void Panel::AddChild(std::shared_ptr<Visual> child)
{
    if (!child)
    {
        return;
    }

    AdoptChild(child);
    children_.push_back(std::move(child));
    InvalidateMeasure();
}

void Panel::Attach(Window *window)
{
    Visual::Attach(window);
    for (const auto &child : children_)
    {
        child->Attach(window);
    }
}

Visual *Panel::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
    {
        if (Visual *hit = (*it)->FindVisualAt(point))
        {
            return hit;
        }
    }

    return this;
}

Visual *Panel::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
    {
        if (!(*it)->HitTest(point))
        {
            continue;
        }

        if (Visual *focusable = (*it)->FindFocusableAt(point))
        {
            return focusable;
        }
    }

    return IsFocusable() ? this : nullptr;
}

Visual *Panel::FindFirstFocusableDescendant()
{
    if (IsFocusable())
    {
        return this;
    }

    for (const auto &child : children_)
    {
        if (Visual *focusable = child->FindFirstFocusableDescendant())
        {
            return focusable;
        }
    }

    return nullptr;
}

StackPanel::StackPanel(float spacing) : spacing_(spacing)
{
}

void StackPanel::SetSpacing(float spacing)
{
    if (spacing_ == spacing)
    {
        return;
    }
    spacing_ = spacing;
    InvalidateMeasure();
}

void StackPanel::SetHorizontalContentAlignment(HorizontalAlignment alignment)
{
    horizontalContentAlignment_ = alignment;
}

void StackPanel::SetVerticalContentAlignment(VerticalAlignment alignment)
{
    verticalContentAlignment_ = alignment;
}

SizeF StackPanel::Measure(const SizeF &availableSize)
{
    measuredChildren_.clear();

    const SizeF innerAvailable = DeflateSize(availableSize, padding_);
    float width = 0.0f;
    float height = 0.0f;
    for (const auto &child : children_)
    {
        const SizeF measured = child->MeasureInLayout(innerAvailable);
        measuredChildren_.push_back(measured);
        width = std::max(width, measured.width);
        height += measured.height;
    }

    if (!children_.empty())
    {
        height += spacing_ * static_cast<float>(children_.size() - 1);
    }

    measuredContent_ = {width, height};
    return {std::min(width + padding_.left + padding_.right, availableSize.width),
            std::min(height + padding_.top + padding_.bottom, availableSize.height)};
}

void StackPanel::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    const RectF inner = DeflateRect(finalRect, padding_);

    float cursorY = ComputeAlignedStart(inner.y, inner.height, measuredContent_.height, verticalContentAlignment_);
    // A child added since the last measure has no measurement yet; it waits for the next pass.
    for (size_t index = 0; index < children_.size() && index < measuredChildren_.size(); ++index)
    {
        const SizeF measured = measuredChildren_[index];
        float slotX = inner.x;
        float slotWidth = inner.width;
        if (horizontalContentAlignment_ != HorizontalAlignment::Stretch)
        {
            slotWidth = std::min(measured.width, inner.width);
            slotX = ComputeAlignedStart(inner.x, inner.width, slotWidth, horizontalContentAlignment_);
        }

        children_[index]->ArrangeInLayout({slotX, cursorY, slotWidth, measured.height});
        cursorY += measured.height + spacing_;
    }
}

void StackPanel::Render(DeviceResources &deviceResources)
{
    for (const auto &child : children_)
    {
        if (ShouldRenderChild(deviceResources, *child))
        {
            child->Render(deviceResources);
        }
    }
}

HorizontalStackPanel::HorizontalStackPanel(float spacing) : spacing_(spacing)
{
}

void HorizontalStackPanel::SetHorizontalContentAlignment(HorizontalAlignment alignment)
{
    horizontalContentAlignment_ = alignment;
}

void HorizontalStackPanel::SetVerticalContentAlignment(VerticalAlignment alignment)
{
    verticalContentAlignment_ = alignment;
}

SizeF HorizontalStackPanel::Measure(const SizeF &availableSize)
{
    measuredChildren_.clear();

    const SizeF innerAvailable = DeflateSize(availableSize, padding_);
    float width = 0.0f;
    float height = 0.0f;
    for (const auto &child : children_)
    {
        const SizeF measured = child->MeasureInLayout(innerAvailable);
        measuredChildren_.push_back(measured);
        width += measured.width;
        height = std::max(height, measured.height);
    }

    if (!children_.empty())
    {
        width += spacing_ * static_cast<float>(children_.size() - 1);
    }

    measuredContent_ = {width, height};
    return {std::min(width + padding_.left + padding_.right, availableSize.width),
            std::min(height + padding_.top + padding_.bottom, availableSize.height)};
}

void HorizontalStackPanel::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    const RectF inner = DeflateRect(finalRect, padding_);

    float cursorX = ComputeAlignedStart(inner.x, inner.width, measuredContent_.width, horizontalContentAlignment_);
    // A child added since the last measure has no measurement yet; it waits for the next pass.
    for (size_t index = 0; index < children_.size() && index < measuredChildren_.size(); ++index)
    {
        const SizeF measured = measuredChildren_[index];
        float slotY = inner.y;
        float slotHeight = inner.height;
        if (verticalContentAlignment_ != VerticalAlignment::Stretch)
        {
            slotHeight = std::min(measured.height, inner.height);
            slotY = ComputeAlignedStart(inner.y, inner.height, slotHeight, verticalContentAlignment_);
        }

        children_[index]->ArrangeInLayout({cursorX, slotY, measured.width, slotHeight});
        cursorX += measured.width + spacing_;
    }
}

void HorizontalStackPanel::Render(DeviceResources &deviceResources)
{
    for (const auto &child : children_)
    {
        if (ShouldRenderChild(deviceResources, *child))
        {
            child->Render(deviceResources);
        }
    }
}

WrapPanel::WrapPanel(float spacing, float runSpacing) : spacing_(spacing), runSpacing_(runSpacing)
{
}

SizeF WrapPanel::Measure(const SizeF &availableSize)
{
    measuredChildren_.clear();
    rowItems_.clear();

    const float maxWidth = std::max(availableSize.width, 1.0f);
    float currentX = 0.0f;
    float currentY = 0.0f;
    float rowHeight = 0.0f;
    float measuredWidth = 0.0f;

    for (size_t index = 0; index < children_.size(); ++index)
    {
        const SizeF childSize = children_[index]->MeasureInLayout(availableSize);
        measuredChildren_.push_back(childSize);

        const bool wrap = currentX > 0.0f && (currentX + childSize.width) > maxWidth;
        if (wrap)
        {
            measuredWidth = std::max(measuredWidth, currentX - spacing_);
            currentX = 0.0f;
            currentY += rowHeight + runSpacing_;
            rowHeight = 0.0f;
        }

        rowItems_.push_back({index, childSize, currentX, currentY});
        currentX += childSize.width + spacing_;
        rowHeight = std::max(rowHeight, childSize.height);
    }

    measuredWidth = std::max(measuredWidth, currentX > 0.0f ? currentX - spacing_ : 0.0f);
    measured_ = {std::min(measuredWidth, availableSize.width), std::min(currentY + rowHeight, availableSize.height)};
    return measured_;
}

void WrapPanel::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;

    for (const RowItem &item : rowItems_)
    {
        children_[item.childIndex]->ArrangeInLayout(
            {finalRect.x + item.x, finalRect.y + item.y, item.size.width, item.size.height});
    }
}

void WrapPanel::Render(DeviceResources &deviceResources)
{
    for (const auto &child : children_)
    {
        if (ShouldRenderChild(deviceResources, *child))
        {
            child->Render(deviceResources);
        }
    }
}

void OverlayPanel::AddChildAt(std::shared_ptr<Visual> child, float x, float y)
{
    if (!child)
    {
        return;
    }
    Panel::AddChild(std::move(child));
    positions_.push_back({x, y});
}

void OverlayPanel::SetChildPosition(const Visual *child, float x, float y)
{
    for (size_t index = 0; index < children_.size(); ++index)
    {
        if (children_[index].get() == child)
        {
            positions_[index] = {x, y};
            InvalidateMeasure();
            return;
        }
    }
}

PointF OverlayPanel::GetChildPosition(const Visual *child) const
{
    for (size_t index = 0; index < children_.size(); ++index)
    {
        if (children_[index].get() == child)
        {
            return positions_[index];
        }
    }
    return {};
}

SizeF OverlayPanel::Measure(const SizeF &availableSize)
{
    measuredChildren_.clear();
    const SizeF innerAvailable = DeflateSize(availableSize, padding_);
    float width = 0.0f;
    float height = 0.0f;
    for (size_t index = 0; index < children_.size(); ++index)
    {
        const SizeF measured = children_[index]->MeasureInLayout(innerAvailable);
        measuredChildren_.push_back(measured);
        width = std::max(width, positions_[index].x + measured.width);
        height = std::max(height, positions_[index].y + measured.height);
    }
    return {std::min(width + padding_.left + padding_.right, availableSize.width),
            std::min(height + padding_.top + padding_.bottom, availableSize.height)};
}

void OverlayPanel::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    const RectF inner = DeflateRect(finalRect, padding_);
    for (size_t index = 0; index < children_.size() && index < measuredChildren_.size(); ++index)
    {
        children_[index]->ArrangeInLayout({inner.x + positions_[index].x, inner.y + positions_[index].y,
                                           measuredChildren_[index].width, measuredChildren_[index].height});
    }
}

void OverlayPanel::Render(DeviceResources &deviceResources)
{
    for (const auto &child : children_)
    {
        if (ShouldRenderChild(deviceResources, *child))
        {
            child->Render(deviceResources);
        }
    }
}

ScrollViewer::ScrollViewer(std::shared_ptr<Visual> content) : content_(std::move(content))
{
    AdoptChild(content_);
}

void ScrollViewer::SetAppearance(const Appearance &appearance)
{
    appearance_ = appearance;
    InvalidateMeasure();
}

float ScrollViewer::GetScrollOffset() const
{
    return scrollOffsetY_;
}

float ScrollViewer::GetMaxScrollOffset() const
{
    return MaxScrollOffset(GetVerticalExtent());
}

void ScrollViewer::SetScrollOffset(float offset)
{
    scrollAnimating_ = false;
    ApplyScrollOffset(offset);
}

void ScrollViewer::ScrollIntoView(const RectF &rect)
{
    const float contentTop = rect.y - bounds_.y + scrollOffsetY_;
    const float revealed = OffsetToReveal(GetVerticalExtent(), scrollOffsetY_, contentTop, rect.height);
    if (revealed != scrollOffsetY_)
    {
        SetScrollOffset(revealed);
    }
}

void ScrollViewer::ApplyScrollOffset(float offset)
{
    const float clamped = msimeui::ClampScrollOffset(offset, GetVerticalExtent());
    if (clamped == scrollOffsetY_)
    {
        return;
    }
    scrollOffsetY_ = clamped;
    UpdateContentLayout();
    RefreshViewport();
}

SizeF ScrollViewer::Measure(const SizeF &availableSize)
{
    if (!content_)
    {
        return availableSize;
    }

    measuredContent_ = content_->MeasureInLayout({availableSize.width, 1000000.0f});
    // Content that overflows loses the scrollbar's width; measure it again at the width
    // it is going to be arranged at, or wrapped text comes out too short.
    if (appearance_.reservedWidth > 0.0f && measuredContent_.height > availableSize.height + 0.5f)
    {
        measuredContent_ =
            content_->MeasureInLayout({std::max(availableSize.width - appearance_.reservedWidth, 0.0f), 1000000.0f});
    }
    return availableSize;
}

void ScrollViewer::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    ClampScrollOffset();
    UpdateContentLayout();
}

void ScrollViewer::Render(DeviceResources &deviceResources)
{
    if (!content_)
    {
        return;
    }

    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    if (scrollAnimating_ && window_)
    {
        // The offset is a function of the frame time, not stepped per frame, so a late
        // frame catches up instead of slowing the whole glide down.
        const double now = window_->GetFrameTime();
        scrollFrameMs_ = now;
        const bool done = glide_.IsDoneAt(now);
        const float offset =
            msimeui::ClampScrollOffset(done ? glide_.target : glide_.OffsetAt(now), GetVerticalExtent());
        if (offset != scrollOffsetY_)
        {
            scrollOffsetY_ = offset;
            UpdateContentLayout();
        }
        if (done)
        {
            scrollAnimating_ = false;
            window_->RefreshHover();
        }
        else
        {
            window_->RequestAnimationFrame();
        }
    }

    deviceResources.PushClip(bounds_);
    content_->Render(deviceResources);
    deviceResources.PopClip();

    if (!HasVerticalScrollbar())
    {
        return;
    }

    const RectF trackRect = GetScrollbarTrackRect();
    const RectF thumbRect = GetScrollbarThumbRect();
    const Theme &theme = ThemeManager::GetCurrent();
    const auto pick = [](const D2D1_COLOR_F &color, const D2D1_COLOR_F &fallback) {
        return color.a > 0.0f ? color : fallback;
    };
    const bool hot = scrollbarDragging_ || (appearance_.hoverThumbOnly ? thumbHovered_ : scrollbarHovered_);
    ID2D1SolidColorBrush *thumbBrush = deviceResources.GetSolidColorBrush(
        hot ? pick(appearance_.thumbHoverColor, theme.thumbActive) : pick(appearance_.thumbColor, theme.thumb));
    if (!thumbBrush)
    {
        return;
    }

    const float radius = appearance_.cornerRadius;
    if (appearance_.drawTrack)
    {
        if (ID2D1SolidColorBrush *trackBrush =
                deviceResources.GetSolidColorBrush(pick(appearance_.trackColor, theme.track)))
        {
            target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(trackRect.x, trackRect.y, trackRect.x + trackRect.width,
                                              trackRect.y + trackRect.height),
                                  radius, radius),
                trackBrush);
        }
    }
    target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(thumbRect.x, thumbRect.y, thumbRect.x + thumbRect.width,
                                                               thumbRect.y + thumbRect.height),
                                                   radius, radius),
                                 thumbBrush);
}

void ScrollViewer::Attach(Window *window)
{
    Visual::Attach(window);
    if (content_)
    {
        content_->Attach(window);
    }
}

Visual *ScrollViewer::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    if (HasVerticalScrollbar() && PointInRect(GetScrollbarTrackRect(), point))
    {
        return this;
    }

    if (content_)
    {
        if (Visual *hit = content_->FindVisualAt(point))
        {
            return hit;
        }
    }

    return this;
}

Visual *ScrollViewer::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    if (HasVerticalScrollbar() && PointInRect(GetScrollbarTrackRect(), point))
    {
        return nullptr;
    }

    if (content_)
    {
        return content_->FindFocusableAt(point);
    }

    return nullptr;
}

Visual *ScrollViewer::FindFirstFocusableDescendant()
{
    return content_ ? content_->FindFirstFocusableDescendant() : nullptr;
}

bool ScrollViewer::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !HasVerticalScrollbar())
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const RectF trackRect = GetScrollbarTrackRect();
    if (!PointInRect(trackRect, dipPoint))
    {
        return false;
    }

    scrollAnimating_ = false;
    const ThumbGrab grab = PressScrollbar(GetVerticalExtent(), GetVerticalTrack(), scrollOffsetY_, dipPoint.y);
    ApplyScrollOffset(grab.offset);
    scrollbarDragging_ = true;
    scrollbarGrip_ = grab.grip;
    RefreshViewport();
    return true;
}

bool ScrollViewer::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)point;
    (void)keyState;
    if (!scrollbarDragging_)
    {
        return false;
    }

    scrollbarDragging_ = false;
    RefreshViewport();
    return true;
}

bool ScrollViewer::OnMouseMove(const POINT &point, WPARAM keyState)
{
    if (!window_ || !HasVerticalScrollbar())
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const bool hovered = PointInRect(GetScrollbarTrackRect(), dipPoint);
    const bool thumbHovered = hovered && PointInRect(GetScrollbarThumbRect(), dipPoint);
    if (scrollbarHovered_ != hovered || thumbHovered_ != thumbHovered)
    {
        scrollbarHovered_ = hovered;
        thumbHovered_ = thumbHovered;
        RefreshViewport();
    }

    if (!scrollbarDragging_ || !(keyState & MK_LBUTTON))
    {
        return hovered;
    }

    ApplyScrollOffset(DragThumb(GetVerticalExtent(), GetVerticalTrack(), scrollbarGrip_, dipPoint.y));
    return true;
}

bool ScrollViewer::OnMouseWheel(const POINT &point, short delta, WPARAM keyState)
{
    (void)keyState;

    if (!window_ || !HitTest(window_->ClientPixelsToDips(point)))
    {
        return false;
    }

    const float maxOffset = GetMaxScrollOffset();
    if (maxOffset <= 0.0f)
    {
        return false;
    }

    const float notch = WheelNotchDistance(SystemWheelScrollLines(), appearance_.wheelStep, bounds_.height);
    if (notch <= 0.0f)
    {
        return true;
    }
    const float step = WheelScrollDelta(delta, notch);
    if (appearance_.smoothScrollMs <= 0.0)
    {
        ApplyScrollOffset(scrollOffsetY_ + step);
        return true;
    }

    // Successive notches extend the glide from where it is heading, and the new glide
    // starts from where the last frame showed it.
    const double now = scrollAnimating_ ? std::max(NowMs(), scrollFrameMs_) : NowMs();
    const float from = scrollAnimating_ ? glide_.OffsetAt(now) : scrollOffsetY_;
    const float velocity = scrollAnimating_ ? glide_.VelocityAt(now) : 0.0f;
    const float base = scrollAnimating_ ? glide_.target : scrollOffsetY_;
    const float target = msimeui::ClampScrollOffset(base + step, GetVerticalExtent());
    if (target == base && !scrollAnimating_)
    {
        return true;
    }

    glide_ = ScrollGlide::Start(from, velocity, target, now, appearance_.smoothScrollMs);
    scrollFrameMs_ = now;
    scrollAnimating_ = true;
    window_->RequestAnimationFrame();
    return true;
}

HCURSOR ScrollViewer::GetCursor() const
{
    return Visual::GetCursor();
}

void ScrollViewer::OnMouseEnter()
{
}

void ScrollViewer::OnMouseLeave()
{
    if (scrollbarHovered_ || thumbHovered_)
    {
        scrollbarHovered_ = false;
        thumbHovered_ = false;
        RefreshViewport();
    }
}

void ScrollViewer::ClampScrollOffset()
{
    const ScrollExtent extent = GetVerticalExtent();
    scrollOffsetY_ = msimeui::ClampScrollOffset(scrollOffsetY_, extent);
    if (scrollAnimating_)
    {
        glide_.from = msimeui::ClampScrollOffset(glide_.from, extent);
        glide_.target = msimeui::ClampScrollOffset(glide_.target, extent);
    }
}

void ScrollViewer::RefreshViewport()
{
    if (window_ && bounds_.width > 0.0f && bounds_.height > 0.0f)
    {
        window_->Invalidate(bounds_);
        return;
    }

    InvalidateVisual();
}

RectF ScrollViewer::GetScrollbarTrackRect() const
{
    const float width = appearance_.trackWidth;
    return {bounds_.x + bounds_.width - width, bounds_.y, width, std::max(bounds_.height, 0.0f)};
}

ScrollExtent ScrollViewer::GetVerticalExtent() const
{
    return {measuredContent_.height, bounds_.height};
}

ScrollTrack ScrollViewer::GetVerticalTrack() const
{
    const RectF trackRect = GetScrollbarTrackRect();
    return {trackRect.y, trackRect.height, appearance_.thumbInset, appearance_.thumbMinLength};
}

RectF ScrollViewer::GetScrollbarThumbRect() const
{
    const RectF trackRect = GetScrollbarTrackRect();
    const ThumbSpan thumb = ThumbAt(GetVerticalExtent(), GetVerticalTrack(), scrollOffsetY_);
    const float x = trackRect.x + trackRect.width - appearance_.thumbRightInset - appearance_.thumbWidth;
    return {x, thumb.start, appearance_.thumbWidth, thumb.length};
}

bool ScrollViewer::HasVerticalScrollbar() const
{
    return CanScroll(GetVerticalExtent());
}

void ScrollViewer::UpdateContentLayout()
{
    if (!content_)
    {
        return;
    }

    const float scrollbarReserve = HasVerticalScrollbar() ? appearance_.reservedWidth : 0.0f;
    // Content moves in whole device pixels: a glide passes through fractional offsets,
    // which would resample text and hairlines differently on every frame.
    float offset = scrollOffsetY_;
    if (window_)
    {
        const float scale = window_->GetDpi() / 96.0f;
        offset = std::round(offset * scale) / scale;
    }
    content_->ArrangeInLayout(
        {bounds_.x, bounds_.y - offset, std::max(bounds_.width - scrollbarReserve, 0.0f), measuredContent_.height});

    if (window_)
    {
        if (Scene *scene = window_->GetScene())
        {
            scene->RelayoutPopups();
        }
    }
}

void Grid::AddRow(GridLength length)
{
    rowDefinitions_.push_back(length);
    InvalidateMeasure();
}

void Grid::AddColumn(GridLength length)
{
    columnDefinitions_.push_back(length);
    InvalidateMeasure();
}

void Grid::AddChild(std::shared_ptr<Visual> child, size_t row, size_t column, size_t rowSpan, size_t columnSpan)
{
    if (!child)
    {
        return;
    }

    AdoptChild(child);
    children_.push_back(
        {std::move(child), {row, column, std::max<size_t>(rowSpan, 1), std::max<size_t>(columnSpan, 1)}, {}});
    InvalidateMeasure();
}

void Grid::SetRowSpacing(float spacing)
{
    rowSpacing_ = std::max(spacing, 0.0f);
    InvalidateMeasure();
}

void Grid::SetColumnSpacing(float spacing)
{
    columnSpacing_ = std::max(spacing, 0.0f);
    InvalidateMeasure();
}

void Grid::Attach(Window *window)
{
    Visual::Attach(window);
    for (auto &child : children_)
    {
        child.visual->Attach(window);
    }
}

Visual *Grid::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
    {
        if (Visual *hit = it->visual->FindVisualAt(point))
        {
            return hit;
        }
    }

    return this;
}

Visual *Grid::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
    {
        if (Visual *focusable = it->visual->FindFocusableAt(point))
        {
            return focusable;
        }
    }

    return nullptr;
}

Visual *Grid::FindFirstFocusableDescendant()
{
    for (auto &child : children_)
    {
        if (Visual *focusable = child.visual->FindFirstFocusableDescendant())
        {
            return focusable;
        }
    }

    return nullptr;
}

SizeF Grid::Measure(const SizeF &availableSize)
{
    if (rowDefinitions_.empty())
    {
        rowDefinitions_.push_back({GridUnitType::Star, 1.0f});
    }
    if (columnDefinitions_.empty())
    {
        columnDefinitions_.push_back({GridUnitType::Star, 1.0f});
    }

    for (auto &child : children_)
    {
        child.measured = child.visual->MeasureInLayout(availableSize);
    }

    auto columns = ResolveTrackSizes(columnDefinitions_, columnSpacing_, availableSize.width, true);
    auto rows = ResolveTrackSizes(rowDefinitions_, rowSpacing_, availableSize.height, false);

    const float totalColumnWidth = std::accumulate(columns.begin(), columns.end(), 0.0f) +
                                   columnSpacing_ * std::max<int>(static_cast<int>(columns.size()) - 1, 0);
    const float totalRowHeight = std::accumulate(rows.begin(), rows.end(), 0.0f) +
                                 rowSpacing_ * std::max<int>(static_cast<int>(rows.size()) - 1, 0);

    measuredContent_ = {std::min(totalColumnWidth, availableSize.width),
                        std::min(totalRowHeight, availableSize.height)};
    return measuredContent_;
}

void Grid::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;

    auto columns = ResolveTrackSizes(columnDefinitions_, columnSpacing_, finalRect.width, true);
    auto rows = ResolveTrackSizes(rowDefinitions_, rowSpacing_, finalRect.height, false);

    std::vector<float> columnOffsets(columns.size(), finalRect.x);
    std::vector<float> rowOffsets(rows.size(), finalRect.y);

    for (size_t i = 1; i < columns.size(); ++i)
    {
        columnOffsets[i] = columnOffsets[i - 1] + columns[i - 1] + columnSpacing_;
    }
    for (size_t i = 1; i < rows.size(); ++i)
    {
        rowOffsets[i] = rowOffsets[i - 1] + rows[i - 1] + rowSpacing_;
    }

    for (auto &child : children_)
    {
        const size_t row = std::min(child.cell.row, rows.size() - 1);
        const size_t column = std::min(child.cell.column, columns.size() - 1);
        const size_t rowEnd = std::min(row + child.cell.rowSpan, rows.size());
        const size_t columnEnd = std::min(column + child.cell.columnSpan, columns.size());

        float width = 0.0f;
        for (size_t i = column; i < columnEnd; ++i)
        {
            width += columns[i];
        }
        if (columnEnd > column)
        {
            width += columnSpacing_ * static_cast<float>(columnEnd - column - 1);
        }

        float height = 0.0f;
        for (size_t i = row; i < rowEnd; ++i)
        {
            height += rows[i];
        }
        if (rowEnd > row)
        {
            height += rowSpacing_ * static_cast<float>(rowEnd - row - 1);
        }

        child.visual->ArrangeInLayout({columnOffsets[column], rowOffsets[row], width, height});
    }
}

void Grid::Render(DeviceResources &deviceResources)
{
    for (auto &child : children_)
    {
        if (ShouldRenderChild(deviceResources, *child.visual))
        {
            child.visual->Render(deviceResources);
        }
    }
}

std::vector<float> Grid::ResolveTrackSizes(const std::vector<GridLength> &definitions, float spacing, float available,
                                           bool horizontalAxis)
{
    std::vector<float> sizes(definitions.size(), 0.0f);
    if (definitions.empty())
    {
        return sizes;
    }

    const float usable =
        std::max(available - spacing * std::max<int>(static_cast<int>(definitions.size()) - 1, 0), 0.0f);
    float used = 0.0f;
    float totalStar = 0.0f;

    for (size_t index = 0; index < definitions.size(); ++index)
    {
        const GridLength &definition = definitions[index];
        if (definition.unitType == GridUnitType::Pixel)
        {
            sizes[index] = std::max(definition.value, 0.0f);
            used += sizes[index];
        }
        else if (definition.unitType == GridUnitType::Star)
        {
            totalStar += std::max(definition.value, 0.0f);
        }
    }

    for (size_t index = 0; index < definitions.size(); ++index)
    {
        if (definitions[index].unitType != GridUnitType::Auto)
        {
            continue;
        }

        float autoSize = 0.0f;
        for (const auto &child : children_)
        {
            const size_t track = horizontalAxis ? child.cell.column : child.cell.row;
            const size_t span = horizontalAxis ? child.cell.columnSpan : child.cell.rowSpan;
            if (track != index || span != 1)
            {
                continue;
            }

            autoSize = std::max(autoSize, horizontalAxis ? child.measured.width : child.measured.height);
        }

        sizes[index] = autoSize;
        used += autoSize;
    }

    const float remaining = std::max(usable - used, 0.0f);
    if (totalStar > 0.0f)
    {
        for (size_t index = 0; index < definitions.size(); ++index)
        {
            if (definitions[index].unitType != GridUnitType::Star)
            {
                continue;
            }

            sizes[index] = remaining * (std::max(definitions[index].value, 0.0f) / totalStar);
        }
    }

    return sizes;
}

Container::Container(std::shared_ptr<Visual> child) : child_(std::move(child))
{
    AdoptChild(child_);
}

void Container::SetChild(std::shared_ptr<Visual> child)
{
    ReleaseChild(child_);

    child_ = std::move(child);
    AdoptChild(child_);
    InvalidateMeasure();
}

void Container::Attach(Window *window)
{
    Visual::Attach(window);
    if (child_)
    {
        child_->Attach(window);
    }
}

Visual *Container::FindVisualAt(const PointF &point)
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

Visual *Container::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    return child_ ? child_->FindFocusableAt(point) : nullptr;
}

Visual *Container::FindFirstFocusableDescendant()
{
    return child_ ? child_->FindFirstFocusableDescendant() : nullptr;
}

SizeF Container::Measure(const SizeF &availableSize)
{
    const SizeF inner = DeflateSize(availableSize, padding_);
    if (!child_)
    {
        return {padding_.left + padding_.right, padding_.top + padding_.bottom};
    }

    const SizeF childSize = child_->MeasureInLayout(inner);
    return {childSize.width + padding_.left + padding_.right, childSize.height + padding_.top + padding_.bottom};
}

void Container::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    if (child_)
    {
        child_->ArrangeInLayout(GetContentRect());
    }
}

void Container::Render(DeviceResources &deviceResources)
{
    if (child_)
    {
        child_->Render(deviceResources);
    }
}

RectF Container::GetContentRect() const
{
    return DeflateRect(bounds_, padding_);
}

Border::Border(Brush brush, std::shared_ptr<Visual> child) : Container(std::move(child)), brush_(brush)
{
}

SizeF Border::Measure(const SizeF &availableSize)
{
    const float inset = brush_.strokeWidth;
    const Thickness borderInset = {inset, inset, inset, inset};
    const SizeF inner = DeflateSize(DeflateSize(availableSize, padding_), borderInset);
    if (!child_)
    {
        return {padding_.left + padding_.right + inset * 2.0f, padding_.top + padding_.bottom + inset * 2.0f};
    }

    const SizeF childSize = child_->MeasureInLayout(inner);
    return {childSize.width + padding_.left + padding_.right + inset * 2.0f,
            childSize.height + padding_.top + padding_.bottom + inset * 2.0f};
}

void Border::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    if (!child_)
    {
        return;
    }

    const Thickness borderInset = {brush_.strokeWidth, brush_.strokeWidth, brush_.strokeWidth, brush_.strokeWidth};
    child_->ArrangeInLayout(DeflateRect(DeflateRect(bounds_, borderInset), padding_));
}

void Border::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    ID2D1SolidColorBrush *fillBrush = deviceResources.GetSolidColorBrush(brush_.fill);
    ID2D1SolidColorBrush *strokeBrush = deviceResources.GetSolidColorBrush(brush_.stroke);
    if (!fillBrush || !strokeBrush)
    {
        return;
    }

    const auto roundedRect =
        D2D1::RoundedRect(D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.width, bounds_.y + bounds_.height),
                          brush_.radiusX, brush_.radiusY);
    target->FillRoundedRectangle(roundedRect, fillBrush);
    target->DrawRoundedRectangle(roundedRect, strokeBrush, brush_.strokeWidth);

    Container::Render(deviceResources);
}

Card::Card(Brush brush, float padding) : brush_(brush), padding_{padding, padding, padding, padding}
{
}

void Card::SetBrush(Brush brush)
{
    brush_ = brush;
    InvalidateVisual();
}

void Card::SetPadding(float padding)
{
    SetPadding(Thickness{padding, padding, padding, padding});
}

void Card::SetPadding(Thickness padding)
{
    if (padding_.left == padding.left && padding_.top == padding.top && padding_.right == padding.right &&
        padding_.bottom == padding.bottom)
    {
        return;
    }
    padding_ = padding;
    InvalidateMeasure();
}

void Card::SetShadowScale(float scale)
{
    const float clamped = std::max(scale, 0.15f);
    if (shadowScale_ == clamped)
    {
        return;
    }
    shadowScale_ = clamped;
    InvalidateVisual();
}

void Card::SetShadowOpacity(float opacity)
{
    const float clamped = std::clamp(opacity, 0.0f, 1.0f);
    if (shadowOpacity_ == clamped)
    {
        return;
    }
    shadowOpacity_ = clamped;
    InvalidateVisual();
}

void Card::SetShadowPasses(const std::vector<ShadowPass> &passes)
{
    if (shadowPasses_ == passes)
    {
        return;
    }
    shadowPasses_ = passes;
    InvalidateVisual();
}

void Card::SetShadowEnabled(bool enabled)
{
    if (shadowEnabled_ == enabled)
    {
        return;
    }
    shadowEnabled_ = enabled;
    InvalidateVisual();
}

void Card::SetShadowOutsideOnly(bool outsideOnly)
{
    if (shadowOutsideOnly_ == outsideOnly)
    {
        return;
    }
    shadowOutsideOnly_ = outsideOnly;
    InvalidateVisual();
}

namespace
{
// Layer masking out `shape`: everything within `reach` of it except the shape itself.
bool PushOutsideLayer(ID2D1RenderTarget *target, const D2D1_ROUNDED_RECT &shape, float reach)
{
    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    ComPtr<ID2D1RectangleGeometry> outer;
    ComPtr<ID2D1RoundedRectangleGeometry> inner;
    ComPtr<ID2D1PathGeometry> ring;
    ComPtr<ID2D1GeometrySink> sink;
    ComPtr<ID2D1Layer> layer;
    const D2D1_RECT_F &r = shape.rect;
    if (!factory ||
        FAILED(factory->CreateRectangleGeometry(
            D2D1::RectF(r.left - reach, r.top - reach, r.right + reach, r.bottom + reach), outer.GetAddressOf())) ||
        FAILED(factory->CreateRoundedRectangleGeometry(shape, inner.GetAddressOf())) ||
        FAILED(factory->CreatePathGeometry(ring.GetAddressOf())) || FAILED(ring->Open(sink.GetAddressOf())) ||
        FAILED(outer->CombineWithGeometry(inner.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get())) ||
        FAILED(sink->Close()) || FAILED(target->CreateLayer(nullptr, layer.GetAddressOf())))
    {
        return false;
    }
    target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), ring.Get()), layer.Get());
    return true;
}
} // namespace

SizeF Card::Measure(const SizeF &availableSize)
{
    const SizeF inner = {std::max(availableSize.width - padding_.left - padding_.right, 0.0f),
                         std::max(availableSize.height - padding_.top - padding_.bottom, 0.0f)};

    float width = 0.0f;
    float height = 0.0f;
    for (const auto &child : children_)
    {
        childSize_ = child->MeasureInLayout(inner);
        width = std::max(width, childSize_.width);
        height = std::max(height, childSize_.height);
    }

    return {width + padding_.left + padding_.right, height + padding_.top + padding_.bottom};
}

void Card::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;

    const RectF inner = {finalRect.x + padding_.left, finalRect.y + padding_.top,
                         std::max(finalRect.width - padding_.left - padding_.right, 0.0f),
                         std::max(finalRect.height - padding_.top - padding_.bottom, 0.0f)};

    for (const auto &child : children_)
    {
        child->ArrangeInLayout(inner);
    }
}

void Card::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    ID2D1SolidColorBrush *fillBrush = deviceResources.GetSolidColorBrush(brush_.fill);
    ID2D1SolidColorBrush *strokeBrush = deviceResources.GetSolidColorBrush(brush_.stroke);
    if (!fillBrush || !strokeBrush)
    {
        return;
    }

    const auto roundedRect =
        D2D1::RoundedRect(D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.width, bounds_.y + bounds_.height),
                          brush_.radiusX, brush_.radiusY);
    if (shadowEnabled_)
    {
        // The shadow passes reach well under 256 DIPs past the card.
        const bool masked = shadowOutsideOnly_ && PushOutsideLayer(target, roundedRect, 256.0f);
        RenderShadow(target);
        if (masked)
        {
            target->PopLayer();
        }
    }
    target->FillRoundedRectangle(roundedRect, fillBrush);
    RenderBackgroundImage(deviceResources, target, roundedRect);
    target->DrawRoundedRectangle(roundedRect, strokeBrush, brush_.strokeWidth);

    for (const auto &child : children_)
    {
        child->Render(deviceResources);
    }
}

void Card::SetBackgroundImage(std::wstring filePath, ImageStretch stretch, float opacity)
{
    const float clamped = std::clamp(opacity, 0.0f, 1.0f);
    if (backgroundImage_ == filePath && !backgroundNineSlice_ && backgroundStretch_ == stretch &&
        backgroundOpacity_ == clamped)
    {
        return;
    }
    backgroundImage_ = std::move(filePath);
    backgroundNineSlice_ = false;
    backgroundStretch_ = stretch;
    backgroundOpacity_ = clamped;
    InvalidateVisual();
}

namespace
{
Thickness NonNegative(const Thickness &value)
{
    return {(std::max)(0.0f, value.left), (std::max)(0.0f, value.top), (std::max)(0.0f, value.right),
            (std::max)(0.0f, value.bottom)};
}

bool SameThickness(const Thickness &a, const Thickness &b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// Moves a DIP coordinate onto the nearest device pixel under the target's current transform,
// when that transform only scales and translates.
struct PixelSnapper
{
    explicit PixelSnapper(ID2D1RenderTarget *target)
    {
        float dpiX = 96.0f;
        float dpiY = 96.0f;
        target->GetDpi(&dpiX, &dpiY);
        D2D1_MATRIX_3X2_F transform = {};
        target->GetTransform(&transform);
        enabled = transform._12 == 0.0f && transform._21 == 0.0f && transform._11 > 0.0f && transform._22 > 0.0f &&
                  dpiX > 0.0f && dpiY > 0.0f;
        scaleX = transform._11 * dpiX / 96.0f;
        scaleY = transform._22 * dpiY / 96.0f;
        offsetX = transform._31 * dpiX / 96.0f;
        offsetY = transform._32 * dpiY / 96.0f;
    }

    float X(float value) const
    {
        return enabled ? (std::round(value * scaleX + offsetX) - offsetX) / scaleX : value;
    }

    float Y(float value) const
    {
        return enabled ? (std::round(value * scaleY + offsetY) - offsetY) / scaleY : value;
    }

    bool enabled = false;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
};
} // namespace

void DrawNineSlice(ID2D1RenderTarget *target, ID2D1Bitmap *bitmap, const RectF &destination, const NineSlice &nineSlice,
                   float opacity)
{
    if (!target || !bitmap || destination.width <= 0.0f || destination.height <= 0.0f || opacity <= 0.0f)
    {
        return;
    }
    // Source rectangles are in the bitmap's DIPs; slices are image pixels.
    const D2D1_SIZE_U pixels = bitmap->GetPixelSize();
    const D2D1_SIZE_F dips = bitmap->GetSize();
    if (pixels.width == 0 || pixels.height == 0 || dips.width <= 0.0f || dips.height <= 0.0f)
    {
        return;
    }
    const float srcW = static_cast<float>(pixels.width);
    const float srcH = static_cast<float>(pixels.height);
    const float toDipX = dips.width / srcW;
    const float toDipY = dips.height / srcH;
    const Thickness slice = NonNegative(nineSlice.slice);
    // A slice past the image edge means the whole image (CSS).
    const float sl = (std::min)(slice.left, srcW);
    const float st = (std::min)(slice.top, srcH);
    const float sr = (std::min)(slice.right, srcW);
    const float sb = (std::min)(slice.bottom, srcH);

    Thickness width = NonNegative(nineSlice.width);
    // CSS border-image-width: when opposite widths overlap, all four shrink by one factor.
    float factor = 1.0f;
    if (width.left + width.right > destination.width)
    {
        factor = (std::min)(factor, destination.width / (width.left + width.right));
    }
    if (width.top + width.bottom > destination.height)
    {
        factor = (std::min)(factor, destination.height / (width.top + width.bottom));
    }
    width = {width.left * factor, width.top * factor, width.right * factor, width.bottom * factor};

    const PixelSnapper snap(target);
    const float x0 = snap.X(destination.x);
    const float x3 = snap.X(destination.x + destination.width);
    const float x1 = (std::min)(snap.X(destination.x + width.left), x3);
    const float x2 = (std::max)(snap.X(destination.x + destination.width - width.right), x1);
    const float y0 = snap.Y(destination.y);
    const float y3 = snap.Y(destination.y + destination.height);
    const float y1 = (std::min)(snap.Y(destination.y + width.top), y3);
    const float y2 = (std::max)(snap.Y(destination.y + destination.height - width.bottom), y1);

    const float dx[4] = {x0, x1, x2, x3};
    const float dy[4] = {y0, y1, y2, y3};
    // When the left and right slices meet or cross, the top, bottom and middle parts are empty.
    const float sourceLeft[3] = {0.0f, sl, srcW - sr};
    const float sourceRight[3] = {sl, srcW - sr, srcW};
    const float sourceTop[3] = {0.0f, st, srcH - sb};
    const float sourceBottom[3] = {st, srcH - sb, srcH};
    // DrawBitmap with a source rectangle still samples across its edges when it scales, which
    // bleeds the neighbouring part into every seam. An image brush clamped to the part does not.
    ComPtr<ID2D1DeviceContext> context;
    target->QueryInterface(IID_PPV_ARGS(context.GetAddressOf()));
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
        {
            if (row == 1 && col == 1 && !nineSlice.fill)
            {
                continue;
            }
            const float sourceWidth = sourceRight[col] - sourceLeft[col];
            const float sourceHeight = sourceBottom[row] - sourceTop[row];
            if (sourceWidth <= 0.0f || sourceHeight <= 0.0f || dx[col + 1] <= dx[col] || dy[row + 1] <= dy[row])
            {
                continue;
            }
            const D2D1_RECT_F source = D2D1::RectF(sourceLeft[col] * toDipX, sourceTop[row] * toDipY,
                                                   sourceRight[col] * toDipX, sourceBottom[row] * toDipY);
            const D2D1_RECT_F part = D2D1::RectF(dx[col], dy[row], dx[col + 1], dy[row + 1]);
            // CSS border-image-repeat：横向平铺只作用于中列（上、中、下三段），
            // 纵向平铺只作用于中行（左、中、右三段），四角始终拉伸。
            const bool tileX = nineSlice.repeatX && col == 1;
            const bool tileY = nineSlice.repeatY && row == 1;
            ComPtr<ID2D1ImageBrush> brush;
            if (context)
            {
                // An image brush puts the top-left of its source rectangle at the brush origin.
                // Tiled axes keep the source at its native size (scale 1) and wrap past its edge.
                const float scaleX = tileX ? 1.0f : (part.right - part.left) / (source.right - source.left);
                const float scaleY = tileY ? 1.0f : (part.bottom - part.top) / (source.bottom - source.top);
                const D2D1_MATRIX_3X2_F transform =
                    D2D1::Matrix3x2F::Scale(scaleX, scaleY) * D2D1::Matrix3x2F::Translation(part.left, part.top);
                context->CreateImageBrush(
                    bitmap,
                    D2D1::ImageBrushProperties(source, tileX ? D2D1_EXTEND_MODE_WRAP : D2D1_EXTEND_MODE_CLAMP,
                                               tileY ? D2D1_EXTEND_MODE_WRAP : D2D1_EXTEND_MODE_CLAMP,
                                               D2D1_INTERPOLATION_MODE_LINEAR),
                    D2D1::BrushProperties(opacity, transform), brush.GetAddressOf());
            }
            if (brush)
            {
                context->FillRectangle(part, brush.Get());
            }
            else
            {
                target->DrawBitmap(bitmap, part, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &source);
            }
        }
    }
}

void Card::SetBackgroundNineSlice(std::wstring filePath, const NineSlice &nineSlice, float opacity)
{
    const float clamped = std::clamp(opacity, 0.0f, 1.0f);
    NineSlice normalized = nineSlice;
    normalized.slice = NonNegative(nineSlice.slice);
    normalized.width = NonNegative(nineSlice.width);
    if (backgroundImage_ == filePath && backgroundNineSlice_ && SameThickness(nineSlice_.slice, normalized.slice) &&
        SameThickness(nineSlice_.width, normalized.width) && nineSlice_.fill == normalized.fill &&
        nineSlice_.repeatX == normalized.repeatX && nineSlice_.repeatY == normalized.repeatY &&
        backgroundOpacity_ == clamped)
    {
        return;
    }
    backgroundImage_ = std::move(filePath);
    backgroundNineSlice_ = true;
    nineSlice_ = normalized;
    backgroundOpacity_ = clamped;
    InvalidateVisual();
}

void Card::RenderBackgroundImage(DeviceResources &deviceResources, ID2D1RenderTarget *target,
                                 const D2D1_ROUNDED_RECT &shape)
{
    if (backgroundImage_.empty() || backgroundOpacity_ <= 0.0f || bounds_.width <= 0.0f || bounds_.height <= 0.0f)
    {
        return;
    }
    D2D1_SIZE_F sourceSize = {};
    ID2D1Bitmap *bitmap = deviceResources.GetImageFrame(backgroundImage_, &sourceSize);
    if (!bitmap || sourceSize.width <= 0.0f || sourceSize.height <= 0.0f)
    {
        return;
    }
    if (backgroundNineSlice_)
    {
        // Like CSS border-image, the panel image ignores the corner radius.
        DrawNineSlice(target, bitmap, bounds_, nineSlice_, backgroundOpacity_);
        return;
    }

    // Same placement rules as Image: Uniform / UniformToFill center the scaled bitmap,
    // None keeps its natural size at the top-left corner.
    RectF destination = bounds_;
    if (backgroundStretch_ == ImageStretch::None)
    {
        destination.width = sourceSize.width;
        destination.height = sourceSize.height;
    }
    else if (backgroundStretch_ == ImageStretch::Uniform || backgroundStretch_ == ImageStretch::UniformToFill)
    {
        const float scaleX = bounds_.width / sourceSize.width;
        const float scaleY = bounds_.height / sourceSize.height;
        const float scale =
            backgroundStretch_ == ImageStretch::Uniform ? std::min(scaleX, scaleY) : std::max(scaleX, scaleY);
        destination.width = sourceSize.width * scale;
        destination.height = sourceSize.height * scale;
        destination.x = bounds_.x + (bounds_.width - destination.width) * 0.5f;
        destination.y = bounds_.y + (bounds_.height - destination.height) * 0.5f;
    }

    // Clip to the rounded card shape so the image corners follow the card radius.
    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    ComPtr<ID2D1Layer> layer;
    if (!factory || FAILED(factory->CreateRoundedRectangleGeometry(shape, clip.GetAddressOf())) ||
        FAILED(target->CreateLayer(nullptr, layer.GetAddressOf())))
    {
        return;
    }
    target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
    target->DrawBitmap(bitmap,
                       D2D1::RectF(destination.x, destination.y, destination.x + destination.width,
                                   destination.y + destination.height),
                       backgroundOpacity_, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    target->PopLayer();
}

void Card::RenderShadow(ID2D1RenderTarget *target)
{
    const bool builtIn = shadowPasses_.empty();
    const ShadowPass *passes = builtIn ? kMistShadowPasses : shadowPasses_.data();
    const size_t count = builtIn ? std::size(kMistShadowPasses) : shadowPasses_.size();
    if (shadowCache_.Render(target, bounds_, brush_.radiusX, passes, count, shadowScale_, shadowOpacity_))
    {
        return;
    }

    // Uncached path, unchanged from before the cache existed.
    shadowCache_.Reset();
    if (builtIn)
    {
        DrawWin11WindowShadow(target, bounds_, brush_.radiusX, shadowScale_, shadowOpacity_);
    }
    else
    {
        DrawGaussianPasses(target, bounds_, brush_.radiusX, passes, count, shadowScale_, shadowOpacity_);
    }
}

void ShadowCache::Reset()
{
    bitmap_.Reset();
    owner_.Reset();
}

namespace
{
ComPtr<IUnknown> ShadowBitmapOwner(ID2D1RenderTarget *target)
{
    ComPtr<IUnknown> owner;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<ID2D1Device> device;
    if (SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(context.GetAddressOf()))))
    {
        context->GetDevice(device.GetAddressOf());
    }
    // Identity through the canonical IUnknown.
    if (device)
    {
        device.As(&owner);
    }
    else
    {
        target->QueryInterface(IID_PPV_ARGS(owner.GetAddressOf()));
    }
    return owner;
}
} // namespace

bool ShadowCache::Render(ID2D1RenderTarget *target, const RectF &bounds, float radius, const ShadowPass *passes,
                         size_t count, float scale, float opacity)
{
    if (!target || bounds.width <= 0.0f || bounds.height <= 0.0f || count == 0)
    {
        return false;
    }

    float dpiX = 96.0f;
    float dpiY = 96.0f;
    target->GetDpi(&dpiX, &dpiY);

    const float s = std::max(scale, 0.15f);
    float pad = 0.0f;
    for (size_t i = 0; i < count; ++i)
    {
        // Same margin DrawGaussianShadowPass gives each pass, plus that pass's offset.
        const float passPad = passes[i].sigma * s * 3.0f + 4.0f +
                              std::max(std::fabs(passes[i].offsetX), std::fabs(passes[i].offsetY)) * s;
        pad = std::max(pad, passPad);
    }
    pad = std::ceil(pad);
    // A blurred pixel only sees the shape within pad of itself, so once it is
    // pad + radius inside both ends of an edge it sees a straight edge and has
    // the same value all along it. A box at least this long on an axis shares
    // one bitmap for that axis.
    const float reach = pad + std::max(radius, 0.0f);
    const float sliceable = std::ceil(reach * 2.0f + kShadowStretchGuard * 2.0f + kShadowStretchSpan);
    const SizeF blurred = {std::min(bounds.width, sliceable), std::min(bounds.height, sliceable)};

    const ComPtr<IUnknown> owner = ShadowBitmapOwner(target);
    const bool hit = bitmap_ && owner_ == owner && IsSameSize(size_, blurred) && radius_ == radius && scale_ == scale &&
                     opacity_ == opacity && dpiX_ == dpiX && dpiY_ == dpiY && passes_.size() == count &&
                     std::equal(passes_.begin(), passes_.end(), passes);
    if (!hit)
    {
        Reset();

        ComPtr<ID2D1BitmapRenderTarget> composed;
        if (FAILED(target->CreateCompatibleRenderTarget({blurred.width + pad * 2.0f, blurred.height + pad * 2.0f},
                                                        composed.GetAddressOf())))
        {
            return false;
        }
        ComPtr<ID2D1DeviceContext> composedDc;
        if (FAILED(composed.As(&composedDc)))
        {
            return false;
        }
        composed->BeginDraw();
        composed->Clear(D2D1::ColorF(0, 0.0f));
        const RectF local = {pad, pad, blurred.width, blurred.height};
        bool drew = false;
        for (size_t i = 0; i < count; ++i)
        {
            if (DrawGaussianShadowPass(composed.Get(), composedDc.Get(), local, radius, passes[i].sigma * s,
                                       passes[i].alpha * opacity, passes[i].offsetX * s, passes[i].offsetY * s))
            {
                drew = true;
            }
        }
        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(composed->EndDraw()) || !drew || FAILED(composed->GetBitmap(bitmap.GetAddressOf())))
        {
            return false;
        }

        owner_ = owner;
        bitmap_ = bitmap;
        size_ = blurred;
        radius_ = radius;
        scale_ = scale;
        opacity_ = opacity;
        dpiX_ = dpiX;
        dpiY_ = dpiY;
        passes_.assign(passes, passes + count);
    }

    // The cuts sit inside the straight stretch, so whatever sits across them is
    // the same value; drawing the pieces aliased tiles them without seams.
    const float cut = pad + reach + kShadowStretchGuard;
    const ShadowAxisSlices columns = SliceShadowAxis(blurred.width + pad * 2.0f, bounds.width + pad * 2.0f, cut);
    const ShadowAxisSlices rows = SliceShadowAxis(blurred.height + pad * 2.0f, bounds.height + pad * 2.0f, cut);
    const float originX = bounds.x - pad;
    const float originY = bounds.y - pad;
    const D2D1_ANTIALIAS_MODE antialias = target->GetAntialiasMode();
    target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t row = 0; row < rows.pieces; ++row)
    {
        for (size_t column = 0; column < columns.pieces; ++column)
        {
            const D2D1_RECT_F source =
                D2D1::RectF(columns.source[column], rows.source[row], columns.source[column + 1], rows.source[row + 1]);
            const D2D1_RECT_F dest = D2D1::RectF(originX + columns.dest[column], originY + rows.dest[row],
                                                 originX + columns.dest[column + 1], originY + rows.dest[row + 1]);
            target->DrawBitmap(bitmap_.Get(), dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
        }
    }
    target->SetAntialiasMode(antialias);
    return true;
}

TextBlock::TextBlock(std::wstring text, float fontSize, D2D1_COLOR_F color, bool bold)
    : text_(std::move(text)), fontSize_(fontSize), color_(color), caretColor_(color), bold_(bold)
{
}

void TextBlock::SetText(std::wstring text)
{
    text_ = std::move(text);
    InvalidateTextLayoutCache();
    InvalidateMeasure();
}

void TextBlock::SetColor(D2D1_COLOR_F color)
{
    color_ = color;
    InvalidateVisual();
}

void TextBlock::SetFontSize(float fontSize)
{
    if (fontSize_ == fontSize)
    {
        return;
    }
    fontSize_ = fontSize;
    InvalidateTextFormatCache();
    InvalidateMeasure();
}

void TextBlock::SetTextAlignment(DWRITE_TEXT_ALIGNMENT alignment)
{
    if (textAlignment_ == alignment)
    {
        return;
    }

    textAlignment_ = alignment;
    InvalidateTextFormatCache();
    InvalidateVisual();
}

void TextBlock::SetFontFamily(std::wstring fontFamily)
{
    if (fontFamilyOverride_ == fontFamily)
    {
        return;
    }

    fontFamilyOverride_ = std::move(fontFamily);
    InvalidateTextFormatCache();
    InvalidateMeasure();
}

void TextBlock::InvalidateTextLayoutCache()
{
    cachedTextLayout_.Reset();
    cachedFontFamily_.clear();
    cachedLayoutWidth_ = -1.0f;
}

void TextBlock::InvalidateTextFormatCache()
{
    cachedTextFormat_.Reset();
    cachedFormatFamily_.clear();
    InvalidateTextLayoutCache();
}

void TextBlock::SetFallbackFontFamilies(std::vector<std::wstring> families)
{
    if (hasCustomFontFallback_ && fallbackFontFamilies_ == families)
        return;
    hasCustomFontFallback_ = true;
    fallbackFontFamilies_ = std::move(families);
    InvalidateTextFormatCache();
    InvalidateMeasure();
}

void TextBlock::SetTextLayoutPadding(Thickness padding)
{
    textLayoutPadding_ = padding;
    InvalidateTextLayoutCache();
    InvalidateMeasure();
}

void TextBlock::SetLetterSpacing(float dips)
{
    const float spacing = (std::max)(dips, 0.0f);
    if (letterSpacing_ == spacing)
    {
        return;
    }
    letterSpacing_ = spacing;
    InvalidateTextLayoutCache();
    InvalidateMeasure();
}

void TextBlock::SetCaretIndex(size_t index)
{
    showCaret_ = true;
    caretIndex_ = index;
    InvalidateTextLayoutCache();
    InvalidateMeasure();
    InvalidateVisual();
}

void TextBlock::SetCaretColor(D2D1_COLOR_F color)
{
    caretColor_ = color;
    InvalidateVisual();
}

void TextBlock::ClearCaret()
{
    if (!showCaret_)
    {
        return;
    }
    showCaret_ = false;
    InvalidateTextLayoutCache();
    InvalidateMeasure();
    InvalidateVisual();
}

void TextBlock::SetBackground(D2D1_COLOR_F fill, float cornerRadius)
{
    background_ = fill;
    backgroundRadius_ = (std::max)(cornerRadius, 0.0f);
    InvalidateVisual();
}

void TextBlock::SetBottomRule(D2D1_COLOR_F color, float width)
{
    bottomRuleColor_ = color;
    bottomRuleWidth_ = (std::max)(width, 0.0f);
    InvalidateMeasure();
    InvalidateVisual();
}

float TextBlock::BottomRuleWidth() const
{
    return bottomRuleColor_.a > 0.001f ? bottomRuleWidth_ : 0.0f;
}

SizeF TextBlock::Measure(const SizeF &availableSize)
{
    MeasureText(availableSize);
    measured_.height += BottomRuleWidth();
    return measured_;
}

SizeF TextBlock::MeasureText(const SizeF &availableSize)
{
    const float maxWidth = std::max(availableSize.width, 1.0f);
    IDWriteFactory *dwriteFactory = GetSharedDWriteFactory();
    if (!dwriteFactory)
    {
        measured_ = {maxWidth, fontSize_ + textLayoutPadding_.top + textLayoutPadding_.bottom};
        return measured_;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    const std::wstring &fontFamily = fontFamilyOverride_.empty() ? theme.uiFontFamily : fontFamilyOverride_;
    const bool needsLayout = !cachedTextLayout_ || cachedLayoutWidth_ != maxWidth || cachedFontFamily_ != fontFamily;
    if (needsLayout)
    {
        if (!cachedTextFormat_ || cachedFormatFamily_ != fontFamily)
        {
            ComPtr<IDWriteTextFormat> format;
            if (FAILED(dwriteFactory->CreateTextFormat(
                    fontFamily.c_str(), nullptr, bold_ ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize_, L"", format.GetAddressOf())))
            {
                if (FAILED(dwriteFactory->CreateTextFormat(
                        UiFontFallbackFamily(), nullptr,
                        bold_ ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, fontSize_, L"", format.GetAddressOf())))
                {
                    measured_ = {maxWidth, fontSize_ + textLayoutPadding_.top + textLayoutPadding_.bottom};
                    return measured_;
                }
            }

            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            format->SetTextAlignment(textAlignment_);
            if (hasCustomFontFallback_)
                ApplyFontFallback(dwriteFactory, format.Get(), fallbackFontFamilies_);
            else
                ApplyUiFontFallback(dwriteFactory, format.Get());
            cachedTextFormat_ = format;
            cachedFormatFamily_ = fontFamily;
        }
        IDWriteTextFormat *format = cachedTextFormat_.Get();

        const UINT32 caretPos = static_cast<UINT32>((std::min)(caretIndex_, text_.size()));
        const bool insertSlot = showCaret_ && caretPos < text_.size();
        std::wstring layoutText = text_;
        if (insertSlot)
        {
            layoutText.insert(layoutText.begin() + static_cast<std::ptrdiff_t>(caretPos), kCaretSlotChar);
        }

        if (FAILED(dwriteFactory->CreateTextLayout(layoutText.c_str(), static_cast<UINT32>(layoutText.size()), format,
                                                   maxWidth, std::numeric_limits<float>::max(),
                                                   cachedTextLayout_.ReleaseAndGetAddressOf())))
        {
            measured_ = {maxWidth, fontSize_ + textLayoutPadding_.top + textLayoutPadding_.bottom};
            return measured_;
        }

        if (letterSpacing_ > 0.0f && !layoutText.empty())
        {
            Microsoft::WRL::ComPtr<IDWriteTextLayout1> layout1;
            if (SUCCEEDED(cachedTextLayout_.As(&layout1)))
            {
                layout1->SetCharacterSpacing(0.0f, letterSpacing_, 0.0f,
                                             DWRITE_TEXT_RANGE{0, static_cast<UINT32>(layoutText.size())});
            }
        }

        if (insertSlot)
        {
            InsertCaretSlot(cachedTextLayout_.Get(), caretPos, fontSize_);
        }

        cachedLayoutWidth_ = maxWidth;
        cachedFontFamily_ = fontFamily;
    }

    DWRITE_TEXT_METRICS metrics = {};
    if (FAILED(cachedTextLayout_->GetMetrics(&metrics)))
    {
        measured_ = {maxWidth, fontSize_ + textLayoutPadding_.top + textLayoutPadding_.bottom};
        return measured_;
    }

    DWRITE_OVERHANG_METRICS overhang = {};
    cachedTextLayout_->GetOverhangMetrics(&overhang);

    const float extraTop = std::max(overhang.top, 0.0f);
    const float extraBottom = std::max(overhang.bottom, 0.0f);
    const float extraLeft = std::max(overhang.left, 0.0f);
    const float extraRight = std::max(overhang.right, 0.0f);
    const float paddedHeight =
        metrics.height + extraTop + extraBottom + textLayoutPadding_.top + textLayoutPadding_.bottom;
    const float minimumHeight = fontSize_ + textLayoutPadding_.top + textLayoutPadding_.bottom;
    const float measuredHeight = std::ceil(std::max(paddedHeight, minimumHeight));
    const float caretEndReserve =
        (showCaret_ && caretIndex_ >= text_.size()) ? (kPreeditCaretEndAir + kPreeditCaretBarWidth) : 0.0f;
    const float contentWidth =
        metrics.width + extraLeft + extraRight + textLayoutPadding_.left + textLayoutPadding_.right + caretEndReserve;
    const float measuredWidth = std::min(std::max(contentWidth, 1.0f), maxWidth);

    measured_ = {measuredWidth, measuredHeight};
    return measured_;
}

void TextBlock::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void TextBlock::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    const std::wstring &fontFamily = fontFamilyOverride_.empty() ? theme.uiFontFamily : fontFamilyOverride_;
    if (!cachedTextLayout_ || cachedLayoutWidth_ != std::max(bounds_.width, 1.0f) || cachedFontFamily_ != fontFamily)
    {
        MeasureText({std::max(bounds_.width, 1.0f), std::max(bounds_.height, 1.0f)});
    }

    // A collapsed block (a hidden preedit arranged at zero height) paints neither its band nor its rule.
    const float ruleWidth = BottomRuleWidth();
    if (bounds_.height > ruleWidth && bounds_.width > 0.0f)
    {
        if (background_.a > 0.001f)
        {
            if (ID2D1SolidColorBrush *fill = deviceResources.GetSolidColorBrush(background_))
            {
                const D2D1_RECT_F rect = D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.width,
                                                     bounds_.y + bounds_.height - ruleWidth);
                if (backgroundRadius_ > 0.0f)
                    target->FillRoundedRectangle(D2D1::RoundedRect(rect, backgroundRadius_, backgroundRadius_), fill);
                else
                    target->FillRectangle(rect, fill);
            }
        }
        if (ruleWidth > 0.0f)
        {
            if (ID2D1SolidColorBrush *rule = deviceResources.GetSolidColorBrush(bottomRuleColor_))
            {
                target->FillRectangle(D2D1::RectF(bounds_.x, bounds_.y + bounds_.height - ruleWidth,
                                                  bounds_.x + bounds_.width, bounds_.y + bounds_.height),
                                      rule);
            }
        }
    }

    ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(color_);
    if (!cachedTextLayout_ || !brush)
    {
        return;
    }

    const float originX = bounds_.x + textLayoutPadding_.left;
    const float originY = bounds_.y + textLayoutPadding_.top;
    if (showCaret_)
    {
        FLOAT caretX = 0.0f;
        FLOAT caretY = 0.0f;
        DWRITE_HIT_TEST_METRICS hit = {};
        const UINT32 caretPos = static_cast<UINT32>((std::min)(caretIndex_, text_.size()));
        const bool caretAtEnd = caretPos == text_.size();
        cachedTextLayout_->HitTestTextPosition(caretPos, caretAtEnd && !text_.empty() ? TRUE : FALSE, &caretX, &caretY,
                                               &hit);
        const float barWidth = kPreeditCaretBarWidth;
        float left = originX + caretX + kPreeditCaretEndAir;
        if (!caretAtEnd)
        {
            const float slotWidth = hit.width > 1.0f ? hit.width : kPreeditCaretInsertGap;
            left = originX + caretX + (slotWidth - barWidth) * 0.5f;
        }
        // Mirrors the CSS caret (`height: 1.2em` inline-block on the baseline,
        // `translate(..., 10%)`): it ends 0.12em below the baseline, well inside
        // the line box. Sizing it to the full line height and nudging it down
        // pushed its bottom past the line into the row below.
        const float barHeight = fontSize_ * 1.2f;
        float baseline = hit.height > 0.0f ? hit.height * 0.8f : fontSize_;
        UINT32 lineCount = 0;
        cachedTextLayout_->GetLineMetrics(nullptr, 0, &lineCount);
        if (lineCount > 0)
        {
            std::vector<DWRITE_LINE_METRICS> lines(lineCount);
            if (SUCCEEDED(cachedTextLayout_->GetLineMetrics(lines.data(), lineCount, &lineCount)))
            {
                float lineTop = 0.0f;
                for (const DWRITE_LINE_METRICS &line : lines)
                {
                    baseline = line.baseline;
                    if (caretY < lineTop + line.height - 0.5f)
                    {
                        break;
                    }
                    lineTop += line.height;
                }
            }
        }
        const float top = originY + caretY + baseline + fontSize_ * 0.12f - barHeight;
        if (ID2D1SolidColorBrush *caretBrush = deviceResources.GetSolidColorBrush(caretColor_))
        {
            target->FillRectangle(D2D1::RectF(left, top, left + barWidth, top + barHeight), caretBrush);
        }
    }

    target->DrawTextLayout(
        D2D1::Point2F(originX, originY), cachedTextLayout_.Get(), brush,
        static_cast<D2D1_DRAW_TEXT_OPTIONS>(D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT));
}

Spacer::Spacer(float height) : height_(height)
{
}

SizeF Spacer::Measure(const SizeF &availableSize)
{
    return {availableSize.width, height_};
}

void Spacer::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void Spacer::Render(DeviceResources &deviceResources)
{
    (void)deviceResources;
}
} // namespace msimeui
