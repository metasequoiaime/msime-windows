#pragma once

#include "Brush.h"
#include "ScrollAxis.h"
#include "Types.h"

#include <dwrite.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>

namespace msimeui
{
class DeviceResources;
class Scene;
class Window;

enum class HorizontalAlignment
{
    Stretch,
    Leading,
    Center,
    Trailing,
};

enum class VerticalAlignment
{
    Stretch,
    Leading,
    Center,
    Trailing,
};

enum class ImageStretch
{
    None,
    Fill,
    Uniform,
    UniformToFill,
};

enum class GridUnitType
{
    Auto,
    Pixel,
    Star,
};

struct GridLength
{
    GridUnitType unitType = GridUnitType::Star;
    float value = 1.0f;
};

class Visual
{
  public:
    friend class Scene;
    virtual ~Visual() = default;

    virtual SizeF Measure(const SizeF &availableSize) = 0;
    virtual void Arrange(const RectF &finalRect) = 0;
    virtual void Render(DeviceResources &deviceResources) = 0;
    virtual void Attach(Window *window);
    virtual bool HitTest(const PointF &point) const;
    virtual Visual *FindVisualAt(const PointF &point);
    virtual Visual *FindFocusableAt(const PointF &point);
    virtual Visual *FindFirstFocusableDescendant();
    virtual bool IsFocusable() const;
    virtual void OnFocusChanged(bool focused);
    virtual bool OnMouseDown(const POINT &point, WPARAM keyState);
    virtual bool OnMouseUp(const POINT &point, WPARAM keyState);
    virtual bool OnMouseMove(const POINT &point, WPARAM keyState);
    virtual void OnMouseEnter();
    virtual void OnMouseLeave();
    virtual bool OnContextMenu(const POINT &point, WPARAM keyState);
    virtual bool OnMouseWheel(const POINT &point, short delta, WPARAM keyState);
    virtual bool OnKeyDown(WPARAM key, LPARAM lParam);
    virtual bool OnChar(wchar_t ch, LPARAM lParam);
    virtual bool OnTimer(UINT_PTR timerId);
    virtual HCURSOR GetCursor() const;
    virtual void LayoutOverlay(const SizeF &viewportSize);
    virtual bool KeepsPopupsOpenOnClick() const;
    void InvalidateMeasure();
    void InvalidateArrange();
    void InvalidateVisual();

    void SetMargin(float uniform);
    void SetMargin(Thickness margin);
    const Thickness &GetMargin() const;
    void SetPadding(float uniform);
    void SetPadding(Thickness padding);
    const Thickness &GetPadding() const;
    void SetWidth(float width);
    void SetHeight(float height);
    void SetMinWidth(float width);
    void SetMinHeight(float height);
    void SetMaxWidth(float width);
    void SetMaxHeight(float height);
    void ClearWidth();
    void ClearHeight();
    void ClearMinWidth();
    void ClearMinHeight();
    void ClearMaxWidth();
    void ClearMaxHeight();
    void SetHorizontalAlignment(HorizontalAlignment alignment);
    void SetVerticalAlignment(VerticalAlignment alignment);
    SizeF MeasureInLayout(const SizeF &availableSize);
    void ArrangeInLayout(const RectF &finalRect);
    const RectF &GetBounds() const;
    bool HasMeasureSlot() const;
    bool HasArrangeSlot() const;
    const SizeF &GetLastMeasureAvailableSize() const;
    Visual *GetParentVisual() const;

  protected:
    void BubbleMeasureInvalidation(Visual *source);
    void BubbleArrangeInvalidation(Visual *source);
    void MarkMeasureDirty();
    void MarkArrangeDirty();
    bool IsMeasureCacheValid(const SizeF &availableSize) const;
    bool IsArrangeCacheValid(const RectF &finalRect) const;
    void AdoptChild(const std::shared_ptr<Visual> &child);
    void ReleaseChild(const std::shared_ptr<Visual> &child);
    bool HasExplicitWidth() const;
    bool HasExplicitHeight() const;
    void SetParent(Visual *parent);
    Visual *GetParent() const;

    Thickness margin_ = {};
    Thickness padding_ = {};
    float explicitWidth_ = -1.0f;
    float explicitHeight_ = -1.0f;
    float minWidth_ = 0.0f;
    float minHeight_ = 0.0f;
    float maxWidth_ = -1.0f;
    float maxHeight_ = -1.0f;
    HorizontalAlignment horizontalAlignment_ = HorizontalAlignment::Stretch;
    VerticalAlignment verticalAlignment_ = VerticalAlignment::Leading;
    SizeF desiredSize_ = {};
    SizeF measuredOuterSize_ = {};
    SizeF lastMeasureAvailableSize_ = {};
    RectF bounds_ = {};
    RectF lastArrangeRect_ = {};
    Visual *parent_ = nullptr;
    Window *window_ = nullptr;
    bool measureDirty_ = true;
    bool arrangeDirty_ = true;
    bool hasMeasureCache_ = false;
    bool hasArrangeCache_ = false;
};

class Panel : public Visual
{
  public:
    void AddChild(std::shared_ptr<Visual> child);
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;

  protected:
    std::vector<std::shared_ptr<Visual>> children_;
};

class StackPanel : public Panel
{
  public:
    explicit StackPanel(float spacing = 0.0f);
    void SetSpacing(float spacing);
    void SetHorizontalContentAlignment(HorizontalAlignment alignment);
    void SetVerticalContentAlignment(VerticalAlignment alignment);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    float spacing_ = 0.0f;
    HorizontalAlignment horizontalContentAlignment_ = HorizontalAlignment::Stretch;
    VerticalAlignment verticalContentAlignment_ = VerticalAlignment::Leading;
    std::vector<SizeF> measuredChildren_;
    SizeF measuredContent_ = {};
};

class HorizontalStackPanel : public Panel
{
  public:
    explicit HorizontalStackPanel(float spacing = 0.0f);
    void SetHorizontalContentAlignment(HorizontalAlignment alignment);
    void SetVerticalContentAlignment(VerticalAlignment alignment);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    float spacing_ = 0.0f;
    HorizontalAlignment horizontalContentAlignment_ = HorizontalAlignment::Leading;
    VerticalAlignment verticalContentAlignment_ = VerticalAlignment::Stretch;
    std::vector<SizeF> measuredChildren_;
    SizeF measuredContent_ = {};
};

class WrapPanel : public Panel
{
  public:
    explicit WrapPanel(float spacing = 0.0f, float runSpacing = 0.0f);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    struct RowItem
    {
        size_t childIndex = 0;
        SizeF size = {};
        float x = 0.0f;
        float y = 0.0f;
    };

    float spacing_ = 0.0f;
    float runSpacing_ = 0.0f;
    SizeF measured_ = {};
    std::vector<SizeF> measuredChildren_;
    std::vector<RowItem> rowItems_;
};

// Absolute placement: each child sits at the (x, y) given to AddChildAt, in DIPs from the
// panel's padded top-left corner, at its own measured size. The panel measures to the
// smallest box that holds every child. Later children draw on top and win hit tests.
class OverlayPanel : public Panel
{
  public:
    void AddChildAt(std::shared_ptr<Visual> child, float x, float y);
    // Moves a child already in the panel; does nothing for any other visual.
    void SetChildPosition(const Visual *child, float x, float y);
    PointF GetChildPosition(const Visual *child) const;

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    // Every child needs a position, so AddChildAt is the only way in.
    using Panel::AddChild;

    std::vector<PointF> positions_;
    std::vector<SizeF> measuredChildren_;
};

class ScrollViewer : public Visual
{
  public:
    struct Appearance
    {
        // Width the scrollbar takes from the content while it is shown; 0 lays the
        // bar over the content.
        float reservedWidth = 20.0f;
        // Hit area along the right edge, and the drawn thumb inside it.
        float trackWidth = 10.0f;
        float thumbWidth = 10.0f;
        // Gap between the thumb and the track ends / the right edge.
        float thumbInset = 0.0f;
        float thumbRightInset = 0.0f;
        float thumbMinLength = 36.0f;
        float cornerRadius = 4.0f;
        bool drawTrack = true;
        // Alpha 0 falls back to ThemeManager's track / thumb colors.
        D2D1_COLOR_F trackColor = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F thumbColor = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F thumbHoverColor = D2D1::ColorF(0, 0.0f);
        // Thumb turns to the hover color only while the pointer is over the thumb
        // itself (WebKit) rather than anywhere over the track.
        bool hoverThumbOnly = false;
        // Distance of one notch at the system's default of 3 lines per notch; other
        // "lines to scroll" settings scale it, "one screen" scrolls a page.
        float wheelStep = 72.0f;
        // Wheel scrolling glides there over this time; 0 jumps.
        double smoothScrollMs = 0.0;
    };

    explicit ScrollViewer(std::shared_ptr<Visual> content);

    void SetAppearance(const Appearance &appearance);
    float GetScrollOffset() const;
    float GetMaxScrollOffset() const;
    // Jumps without animation, clamped to the content.
    void SetScrollOffset(float offset);
    // Scrolls just enough to bring `rect` (DIPs, window space) into view.
    void ScrollIntoView(const RectF &rect);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    void OnMouseEnter() override;
    void OnMouseLeave() override;
    bool OnMouseWheel(const POINT &point, short delta, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    void RefreshViewport();
    void ClampScrollOffset();
    void ApplyScrollOffset(float offset);
    ScrollExtent GetVerticalExtent() const;
    ScrollTrack GetVerticalTrack() const;
    RectF GetScrollbarTrackRect() const;
    RectF GetScrollbarThumbRect() const;
    bool HasVerticalScrollbar() const;
    void UpdateContentLayout();

    std::shared_ptr<Visual> content_;
    Appearance appearance_{};
    SizeF measuredContent_ = {};
    float scrollOffsetY_ = 0.0f;
    // Smooth wheel scrolling while `scrollAnimating_`; each notch starts a new glide from
    // where the last one is. `scrollFrameMs_` is the time of the last frame drawn.
    ScrollGlide glide_{};
    double scrollFrameMs_ = 0.0;
    bool scrollAnimating_ = false;
    bool scrollbarHovered_ = false;
    bool thumbHovered_ = false;
    bool scrollbarDragging_ = false;
    // Where along the thumb the pointer holds it while dragging.
    float scrollbarGrip_ = 0.0f;
};

class Grid : public Visual
{
  public:
    struct Cell
    {
        size_t row = 0;
        size_t column = 0;
        size_t rowSpan = 1;
        size_t columnSpan = 1;
    };

    Grid() = default;

    void AddRow(GridLength length);
    void AddColumn(GridLength length);
    void AddChild(std::shared_ptr<Visual> child, size_t row, size_t column, size_t rowSpan = 1, size_t columnSpan = 1);
    void SetRowSpacing(float spacing);
    void SetColumnSpacing(float spacing);
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;
    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    struct ChildEntry
    {
        std::shared_ptr<Visual> visual;
        Cell cell;
        SizeF measured = {};
    };

    std::vector<float> ResolveTrackSizes(const std::vector<GridLength> &definitions, float spacing, float available,
                                         bool horizontalAxis);

    std::vector<GridLength> rowDefinitions_;
    std::vector<GridLength> columnDefinitions_;
    std::vector<ChildEntry> children_;
    float rowSpacing_ = 0.0f;
    float columnSpacing_ = 0.0f;
    SizeF measuredContent_ = {};
};

class Container : public Visual
{
  public:
    Container() = default;
    explicit Container(std::shared_ptr<Visual> child);

    void SetChild(std::shared_ptr<Visual> child);
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;
    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  protected:
    RectF GetContentRect() const;
    std::shared_ptr<Visual> child_;
};

class Border : public Container
{
  public:
    Border(Brush brush, std::shared_ptr<Visual> child = nullptr);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    Brush brush_;
};

// One gaussian-blurred rounded-rect layer of a card drop shadow.
struct ShadowPass
{
    float sigma;   // blur standard deviation, DIP
    float alpha;   // layer opacity 0..1
    float offsetX; // DIP, positive = right
    float offsetY; // DIP, positive = down

    bool operator==(const ShadowPass &other) const
    {
        return sigma == other.sigma && alpha == other.alpha && offsetX == other.offsetX && offsetY == other.offsetY;
    }
};

// Gaussian drop shadow of a rounded rectangle, blurred once into a bitmap and reused.
// Far enough from the corners a blurred edge no longer changes along it, so the bitmap
// is built for a box just long enough to reach that straight stretch and drawn
// nine-sliced: a box that only grows or shrinks keeps the same bitmap.
class ShadowCache
{
  public:
    // `scale` shrinks the shadow geometry, `opacity` dims every pass. Returns false when
    // nothing could be drawn (no device context behind `target`).
    bool Render(ID2D1RenderTarget *target, const RectF &bounds, float radius, const ShadowPass *passes, size_t count,
                float scale = 1.0f, float opacity = 1.0f);
    void Reset();

  private:
    // What the bitmap can be drawn on: the Direct2D device behind a device context (a
    // drawing surface hands out a new context every frame), else the render target itself.
    Microsoft::WRL::ComPtr<IUnknown> owner_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap_;
    // Size of the box the bitmap was blurred for, not of the box drawn now.
    SizeF size_ = {};
    float radius_ = 0.0f;
    float scale_ = 0.0f;
    float opacity_ = 0.0f;
    float dpiX_ = 0.0f;
    float dpiY_ = 0.0f;
    std::vector<ShadowPass> passes_;
};

// CSS `border-image` with `border-image-repeat: stretch`. The source image is cut along `slice`
// (image pixels, like the numbers of `border-image-slice`) and drawn into borders `width` wide
// (DIPs, like `border-image-width`): corners scale from slice to width, edges stretch along
// their length, the middle stretches both ways and is drawn only with `fill`.
struct NineSlice
{
    Thickness slice;
    Thickness width;
    bool fill = true;
    // 同 CSS border-image-repeat：repeatX 让上、中、下三段沿横向平铺（按源图原尺寸），
    // repeatY 让左、中、右三段沿纵向平铺；false 为拉伸。四角永远不平铺。
    bool repeatX = false;
    bool repeatY = false;
};

// Draws `bitmap` nine-sliced into `destination` (DIPs). Bitmap sizes are read in DIPs at 96 DPI,
// one DIP per image pixel, as DeviceResources creates them. Slices larger than the image are
// clamped to it, and when two opposite widths do not fit into the box they are all scaled down
// by the same factor, as CSS does. Part edges land on device pixels so the parts meet seamlessly.
void DrawNineSlice(ID2D1RenderTarget *target, ID2D1Bitmap *bitmap, const RectF &destination, const NineSlice &nineSlice,
                   float opacity = 1.0f);

class Card : public Panel
{
  public:
    Card(Brush brush, float padding);

    void SetBrush(Brush brush);
    void SetPadding(float padding);
    // 四边独立的内边距，给 v2 皮肤的 content_insets_dip 用。
    void SetPadding(Thickness padding);
    void SetShadowScale(float scale);
    // Multiplies every shadow pass alpha; 1.0 keeps the built-in intensity.
    void SetShadowOpacity(float opacity);
    // Replaces the built-in diffuse shadow with explicit gaussian passes (e.g. to match
    // a themed drop shadow). An empty list (the default) keeps the built-in shadow.
    void SetShadowPasses(const std::vector<ShadowPass> &passes);
    // Turns the drop shadow off entirely; enabled by default.
    void SetShadowEnabled(bool enabled);
    // Keeps the shadow out from under the card shape. A card with a translucent fill
    // (e.g. over a backdrop) would otherwise show its own shadow through itself.
    void SetShadowOutsideOnly(bool outsideOnly);
    // Draws an image file over the fill and under the children, clipped to the rounded
    // card shape. An empty path removes it. Animated images play (DeviceResources::GetImageFrame).
    void SetBackgroundImage(std::wstring filePath, ImageStretch stretch = ImageStretch::UniformToFill,
                            float opacity = 1.0f);
    // Nine-sliced panel image (see NineSlice) over the fill and under the stroke and children.
    // Like CSS border-image it is not clipped to the corner radius. Replaces SetBackgroundImage.
    // Animated images play, though a panel is usually best left still.
    void SetBackgroundNineSlice(std::wstring filePath, const NineSlice &nineSlice, float opacity = 1.0f);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    void RenderShadow(ID2D1RenderTarget *target);
    void RenderBackgroundImage(DeviceResources &deviceResources, ID2D1RenderTarget *target,
                               const D2D1_ROUNDED_RECT &shape);

    Brush brush_;
    Thickness padding_ = {};
    std::wstring backgroundImage_;
    ImageStretch backgroundStretch_ = ImageStretch::UniformToFill;
    float backgroundOpacity_ = 1.0f;
    bool backgroundNineSlice_ = false;
    NineSlice nineSlice_;
    float shadowScale_ = 1.0f;
    float shadowOpacity_ = 1.0f;
    std::vector<ShadowPass> shadowPasses_;
    bool shadowEnabled_ = true;
    bool shadowOutsideOnly_ = false;
    SizeF childSize_ = {};

    // All blur passes composed into one bitmap. Rebuilding the blur inputs and
    // effects dominated the card's repaints.
    ShadowCache shadowCache_;
};

class TextBlock : public Visual
{
  public:
    TextBlock(std::wstring text, float fontSize, D2D1_COLOR_F color, bool bold = false);
    void SetText(std::wstring text);
    void SetColor(D2D1_COLOR_F color);
    void SetFontSize(float fontSize);
    void SetTextAlignment(DWRITE_TEXT_ALIGNMENT alignment);
    void SetFontFamily(std::wstring fontFamily);
    void SetFallbackFontFamilies(std::vector<std::wstring> families);
    void SetTextLayoutPadding(Thickness padding);
    void SetLetterSpacing(float dips);
    void SetCaretIndex(size_t index);
    void SetCaretColor(D2D1_COLOR_F color);
    void ClearCaret();
    // Fills the block's bounds behind the text. Alpha 0 draws nothing.
    void SetBackground(D2D1_COLOR_F fill, float cornerRadius);
    // A horizontal rule along the bottom edge, below the text. The block grows
    // by the rule's width; width <= 0 or alpha 0 removes it.
    void SetBottomRule(D2D1_COLOR_F color, float width);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    SizeF MeasureText(const SizeF &availableSize);
    float BottomRuleWidth() const;
    void InvalidateTextLayoutCache();
    void InvalidateTextFormatCache();

    std::wstring text_;
    float fontSize_ = 16.0f;
    D2D1_COLOR_F color_ = D2D1::ColorF(0x111111);
    D2D1_COLOR_F caretColor_ = D2D1::ColorF(0x6B69D6);
    bool bold_ = false;
    DWRITE_TEXT_ALIGNMENT textAlignment_ = DWRITE_TEXT_ALIGNMENT_LEADING;
    std::wstring fontFamilyOverride_;
    std::vector<std::wstring> fallbackFontFamilies_;
    bool hasCustomFontFallback_ = false;
    Thickness textLayoutPadding_ = {0.0f, 3.0f, 0.0f, 3.0f};
    float letterSpacing_ = 0.0f;
    bool showCaret_ = false;
    size_t caretIndex_ = 0;
    D2D1_COLOR_F background_ = D2D1::ColorF(0, 0.0f);
    float backgroundRadius_ = 0.0f;
    D2D1_COLOR_F bottomRuleColor_ = D2D1::ColorF(0, 0.0f);
    float bottomRuleWidth_ = 0.0f;
    SizeF measured_ = {};
    std::wstring cachedFontFamily_;
    float cachedLayoutWidth_ = -1.0f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> cachedTextLayout_;
    // The format (family, size, weight, alignment, fallback) outlives text and
    // caret changes; building it, and a custom fallback chain in particular,
    // cost more than the layout itself on every keystroke.
    std::wstring cachedFormatFamily_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> cachedTextFormat_;
};

class Spacer : public Visual
{
  public:
    explicit Spacer(float height);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    float height_ = 0.0f;
};
} // namespace msimeui
