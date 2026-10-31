#pragma once

#include "Layout.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

class CTextEditor;

namespace msimeui
{
enum class PopupPlacement
{
    BelowLeading,
    AboveLeading,
    // 贴锚点右缘、顶边与锚点顶边对齐（x = anchor.x + anchor.width + offsetX）。
    RightLeading,
};

class Image : public Visual
{
  public:
    explicit Image(std::wstring filePath);

    void SetSource(std::wstring filePath);
    const std::wstring &GetSource() const;
    void SetStretch(ImageStretch stretch);
    void SetOpacity(float opacity);
    void SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE interpolationMode);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    void LoadNaturalSize();

    std::wstring filePath_;
    SizeF naturalSize_ = {};
    bool naturalSizeLoaded_ = false;
    ImageStretch stretch_ = ImageStretch::Uniform;
    float opacity_ = 1.0f;
    D2D1_BITMAP_INTERPOLATION_MODE interpolationMode_ = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
};

class TextBox : public Visual
{
  public:
    using TextChangedHandler = std::function<void(const std::wstring &text)>;

    // Colors with alpha 0 fall back to ThemeManager. Sizes are DIPs.
    struct Appearance
    {
        float cornerRadius = 10.0f;
        float borderWidth = 1.0f;
        // Distance from the outer edge to the text.
        float paddingX = 12.0f;
        float paddingY = 6.0f;
        D2D1_COLOR_F fill = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F border = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F hoverBorder = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F focusBorder = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F text = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F selection = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F placeholder = D2D1::ColorF(0, 0.0f);
        // Empty uses ThemeManager's textInputFontFamily.
        std::wstring fontFamily;
        // Applies to the text and the placeholder.
        DWRITE_FONT_WEIGHT fontWeight = DWRITE_FONT_WEIGHT_NORMAL;
        // CSS letter-spacing in DIPs, applied to the text (not the placeholder).
        float letterSpacing = 0.0f;
        // Multi-line only: CSS line-height in DIPs (0 = the font's own) and the width kept
        // free on the right for a scrollbar while the text overflows.
        float lineHeight = 0.0f;
        float scrollbarGutter = 0.0f;
    };

    // Vertical scroll state of a multi-line box, in DIPs.
    struct ScrollInfo
    {
        float offset = 0.0f;
        float maxOffset = 0.0f;
        float contentHeight = 0.0f;
        float viewportHeight = 0.0f;
    };

    TextBox(float height, std::wstring placeholder);
    ~TextBox() override;

    void SetAppearance(Appearance appearance);
    // Replaces the text without reporting it through the text-changed handler: that
    // handler is for edits the user makes.
    void SetText(const std::wstring &text);
    void SetPasswordMode(bool password);
    void SetMultiline(bool multiline);
    void SetEnabled(bool enabled);
    bool IsEnabled() const
    {
        return enabled_;
    }
    // Enter in a single-line box.
    void SetOnSubmit(std::function<void()> handler);
    void SetPreferredHeight(float height);
    ScrollInfo GetScrollInfo() const;
    // Clamped; returns whether the text moved.
    bool SetScrollOffset(float offset);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    // A multi-line box scrolls its text while it can, then lets the wheel bubble on.
    bool OnMouseWheel(const POINT &point, short delta, WPARAM keyState) override;
    bool OnKeyDown(WPARAM key, LPARAM lParam) override;
    bool OnChar(wchar_t ch, LPARAM lParam) override;
    bool OnTimer(UINT_PTR timerId) override;
    void OnMouseEnter() override;
    void OnMouseLeave() override;
    HCURSOR GetCursor() const override;
    std::wstring GetText() const;
    bool IsFocused() const
    {
        return focused_;
    }
    void SetOnTextChanged(TextChangedHandler handler);
    void SetOnFocusChanged(std::function<void(bool focused)> handler);
    void SetFontSize(float fontSizeDips);
    void SetPlaceholderFontSize(float fontSizeDips);
    void SetPlaceholderText(std::wstring placeholder);
    void SetChromeVisible(bool visible);

  private:
    RECT ComputeEditorHostRect() const;
    RECT ComputeEditorContentPadding() const;
    POINT ToLocalPoint(const POINT &point) const;
    bool EnsureInitialized(DeviceResources *deviceResources);
    bool AlertMouseSink(const POINT &point, WPARAM keyState);
    bool IsMultiline() const;
    void ApplyFont();
    void RefreshEditorLayout();

    float preferredHeight_ = 44.0f;
    std::wstring placeholder_;
    Appearance appearance_{};
    bool password_ = false;
    int multiline_ = -1; // -1 derives it from the height, as before SetMultiline existed
    bool enabled_ = true;
    bool hovered_ = false;
    bool suppressTextChanged_ = false;
    // Text set before the editor's TSF store exists; applied once it does.
    std::wstring pendingText_;
    bool hasPendingText_ = false;
    std::function<void()> onSubmit_;
    bool focused_ = false;
    bool tsfInitialized_ = false;
    bool renderInitialized_ = false;
    UINT dragSelectionStart_ = static_cast<UINT>(-1);
    ::CTextEditor *editor_ = nullptr;
    LOGFONT font_ = {};
    TextChangedHandler onTextChanged_;
    std::function<void(bool focused)> onFocusChanged_;
    float fontSizeDips_ = 18.0f;
    float placeholderFontSizeDips_ = 16.0f;
    bool chromeVisible_ = true;
};

class Button : public Visual
{
  public:
    using ClickHandler = std::function<void()>;

    Button(std::wstring text, float height = 44.0f);

    void SetOnClick(ClickHandler handler);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  protected:
    void InvalidateTextLayoutCache();

    virtual void OnClick();
    virtual D2D1_COLOR_F GetFillColor() const;
    virtual D2D1_COLOR_F GetStrokeColor() const;
    virtual D2D1_COLOR_F GetTextColor() const;

    std::wstring text_;
    float preferredHeight_ = 44.0f;
    bool focused_ = false;
    bool pressed_ = false;
    ClickHandler onClick_;
    std::wstring cachedFontFamily_;
    float cachedLayoutWidth_ = -1.0f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> cachedTextLayout_;
};

class Popup : public Visual
{
  public:
    explicit Popup(std::shared_ptr<Visual> child);

    void SetAnchorRect(const RectF &anchorRect);
    void SetPlacement(PopupPlacement placement);
    void SetOffset(float x, float y);
    void SetMatchAnchorWidth(bool matchAnchorWidth);
    void SetConstrainToViewport(bool constrainToViewport);
    void SetBackgroundFill(const D2D1_COLOR_F &fill);
    void SetBorderColor(const D2D1_COLOR_F &border);
    void SetCornerRadius(float radius);
    void SetShadowEnabled(bool enabled);
    // Gaussian shadow passes (CSS box-shadow: sigma = blur / 2) replacing the built-in
    // layered shadow; empty keeps the built-in one.
    void SetShadowPasses(std::vector<ShadowPass> passes);
    // A 1 DIP line just inside the top edge, like `box-shadow: inset 0 1px color`.
    void SetTopHighlight(const D2D1_COLOR_F &color);
    // Fades in from transparent while sliding by `fromOffsetY` and scaling up from
    // `fromScale` about the popup's center. Call right after adding it to the scene.
    void BeginAppearAnimation(double durationMs, float fromOffsetY, float fromScale);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;
    bool HitTest(const PointF &point) const override;
    bool KeepsPopupsOpenOnClick() const override;
    void LayoutOverlay(const SizeF &viewportSize) override;

  protected:
    void RenderChrome(DeviceResources &deviceResources);

  private:
    std::shared_ptr<Visual> child_;
    RectF anchorRect_ = {};
    PopupPlacement placement_ = PopupPlacement::BelowLeading;
    float offsetX_ = 0.0f;
    float offsetY_ = 8.0f;
    bool matchAnchorWidth_ = true;
    bool constrainToViewport_ = true;
    D2D1_COLOR_F backgroundFill_ = D2D1::ColorF(0xFFFFFF);
    D2D1_COLOR_F borderColor_ = D2D1::ColorF(0xD6DCE5);
    D2D1_COLOR_F topHighlight_ = D2D1::ColorF(0, 0.0f);
    float cornerRadius_ = 16.0f;
    bool shadowEnabled_ = false;
    std::vector<ShadowPass> shadowPasses_;
    ShadowCache shadowCache_;
    double appearStartMs_ = 0.0;
    double appearDurationMs_ = 0.0;
    float appearOffsetY_ = 0.0f;
    float appearScale_ = 1.0f;
    Microsoft::WRL::ComPtr<ID2D1Layer> layer_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> layerTarget_;
};

class MenuFlyoutItem : public Visual
{
  public:
    using ClickHandler = std::function<void()>;
    using HoverHandler = std::function<void(bool hovered)>;

    explicit MenuFlyoutItem(std::wstring text, bool hasSubmenu = false);

    void SetOnClick(ClickHandler handler);
    void SetOnHover(HoverHandler handler);
    void SetColors(const D2D1_COLOR_F &text, const D2D1_COLOR_F &hoverFill);
    void SetHasSubmenu(bool hasSubmenu);
    void SetLeadingSvg(std::string svgUtf8);
    void SetTrailingToggle(bool show);
    void SetToggleOn(bool on);
    bool IsToggleOn() const;

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    void OnMouseLeave() override;
    bool KeepsPopupsOpenOnClick() const override;
    HCURSOR GetCursor() const override;

  private:
    void SetHovered(bool hovered);
    bool UsesTrayLayout() const;
    RectF ToggleHitRect() const;

    std::wstring text_;
    std::string leadingSvg_;
    bool showToggle_ = false;
    bool toggleOn_ = false;
    bool hasSubmenu_ = false;
    bool hovered_ = false;
    bool pressed_ = false;
    D2D1_COLOR_F textColor_ = D2D1::ColorF(0xE9E8E8);
    D2D1_COLOR_F hoverFill_ = D2D1::ColorF(0x414141);
    ClickHandler onClick_;
    HoverHandler onHover_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout_;
    Microsoft::WRL::ComPtr<IUnknown> svgDocument_;
    std::wstring cachedFontFamily_;
    float cachedLayoutWidth_ = -1.0f;
    void *svgContext_ = nullptr;
    std::string svgTintCache_;
};

class MenuSeparator : public Visual
{
  public:
    void SetColor(const D2D1_COLOR_F &color);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;

  private:
    D2D1_COLOR_F color_ = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.125f);
};

class PopupHost : public Visual
{
  public:
    PopupHost(std::shared_ptr<Visual> trigger, std::shared_ptr<Popup> popup);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;
    bool HitTest(const PointF &point) const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;
    bool KeepsPopupsOpenOnClick() const override;

    void OpenPopup();
    void ClosePopup();
    void TogglePopup();
    bool IsOpen() const;

  private:
    std::shared_ptr<Visual> trigger_;
    std::shared_ptr<Popup> popup_;
    bool pressed_ = false;
    bool open_ = false;
};

class ContextMenuHost : public Visual
{
  public:
    ContextMenuHost(std::shared_ptr<Visual> trigger, std::shared_ptr<Popup> popup);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;
    bool HitTest(const PointF &point) const override;
    bool OnContextMenu(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;
    bool KeepsPopupsOpenOnClick() const override;

    void OpenPopupAt(const PointF &anchorPoint);
    void ClosePopup();
    bool IsOpen() const;

  private:
    std::shared_ptr<Visual> trigger_;
    std::shared_ptr<Popup> popup_;
    bool open_ = false;
};

class ComboBox : public Visual
{
  public:
    using SelectionChangedHandler = std::function<void(size_t selectedIndex, const std::wstring &value)>;

    explicit ComboBox(float height = 44.0f);

    void AddItem(std::wstring item);
    void ClearItems();
    void SetSelectedIndex(size_t index);
    size_t GetSelectedIndex() const;
    const std::wstring &GetSelectedText() const;
    void SetOnSelectionChanged(SelectionChangedHandler handler);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;
    bool KeepsPopupsOpenOnClick() const override;

  private:
    void OpenPopup();
    void ClosePopup();
    void TogglePopup();
    void NotifySelectionChanged();
    void SyncPopupState();

    std::vector<std::wstring> items_;
    float preferredHeight_ = 44.0f;
    bool focused_ = false;
    bool pressed_ = false;
    bool open_ = false;
    size_t selectedIndex_ = static_cast<size_t>(-1);
    SelectionChangedHandler onSelectionChanged_;
    std::shared_ptr<Visual> popupContent_;
    std::shared_ptr<Popup> popup_;
};

class CheckBox : public Visual
{
  public:
    using ChangeHandler = std::function<void(bool checked)>;

    CheckBox(std::wstring text, bool checked = false);

    void SetOnChanged(ChangeHandler handler);
    bool IsChecked() const;
    void SetChecked(bool checked);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    std::wstring text_;
    bool checked_ = false;
    bool focused_ = false;
    bool pressed_ = false;
    ChangeHandler onChanged_;
};

class ProgressBar : public Visual
{
  public:
    explicit ProgressBar(float height = 12.0f);

    void SetValue(float value);
    float GetValue() const;

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    float preferredHeight_ = 12.0f;
    float value_ = 0.0f;
};

class Slider : public Visual
{
  public:
    using ChangeHandler = std::function<void(float value)>;

    Slider(float minValue, float maxValue, float value, float height = 34.0f);

    void SetOnChanged(ChangeHandler handler);
    void SetValue(float value);
    float GetValue() const;

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    float NormalizedValue() const;
    void SetValueInternal(float value, bool notify);
    bool UpdateFromPoint(const POINT &point, bool notify);

    float minValue_ = 0.0f;
    float maxValue_ = 100.0f;
    float value_ = 0.0f;
    float preferredHeight_ = 34.0f;
    bool focused_ = false;
    bool dragging_ = false;
    ChangeHandler onChanged_;
};

class Separator : public Visual
{
  public:
    explicit Separator(float height = 1.0f);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    float height_ = 1.0f;
};

class ListView : public Visual
{
  public:
    struct Item
    {
        std::wstring title;
        std::wstring subtitle;
        std::wstring badge;
    };

    using SelectionChangedHandler = std::function<void(size_t selectedIndex)>;

    explicit ListView(float itemHeight = 68.0f);

    void AddItem(Item item);
    void ClearItems();
    void SetOnSelectionChanged(SelectionChangedHandler handler);
    void SetSelectedIndex(size_t index);
    size_t GetSelectedIndex() const;

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    struct ItemLayoutCache
    {
        float titleWidth = -1.0f;
        float subtitleWidth = -1.0f;
        float badgeWidth = -1.0f;
        std::wstring fontFamily;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> titleLayout;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> subtitleLayout;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> badgeLayout;
    };

    void InvalidateLayoutCache();
    size_t HitTestItem(const PointF &point) const;

    std::vector<Item> items_;
    std::vector<ItemLayoutCache> layoutCache_;
    float itemHeight_ = 68.0f;
    bool focused_ = false;
    bool pressed_ = false;
    size_t pressedIndex_ = static_cast<size_t>(-1);
    size_t selectedIndex_ = 0;
    SelectionChangedHandler onSelectionChanged_;
};

class CandidateList : public Visual
{
  public:
    // Role of a run inside the translation text. Secondary runs (e.g. a
    // leading tag) and delimiters take their own colors from Appearance.
    enum class TextRunRole
    {
        Secondary,
        Delimiter,
    };

    // Run in UTF-16 code units of Item::translation.
    struct TextRun
    {
        UINT32 start = 0;
        UINT32 length = 0;
        TextRunRole role = TextRunRole::Secondary;
    };

    struct Item
    {
        std::wstring label;
        // Short faint prefix drawn between the label and the text (e.g. a
        // category tag). Empty leaves the row exactly as before.
        std::wstring tag;
        std::wstring text;
        std::wstring annotation;
        std::wstring translation;
        // Runs not listed here use the translation color.
        std::vector<TextRun> translationRuns;
    };

    enum class Orientation
    {
        Vertical,
        Horizontal,
    };

    struct Appearance
    {
        std::wstring fontFamily;
        std::vector<std::wstring> fallbackFontFamilies;
        float itemHeight = 28.0f;
        float itemGap = 2.0f;
        float fontSize = 16.0f;
        float labelFontSize = 12.8f;
        float tagFontSize = 11.0f;
        float annotationFontSize = 16.0f;
        float cornerRadius = 4.0f;
        // Radius for item corners that sit on the list's own corners, so a
        // fill inside an inset rounded frame follows the frame's R there.
        // <= 0 keeps cornerRadius on every corner. outerTopCornersEnabled
        // false leaves the top corners alone when something (e.g. a preedit
        // row) sits between the list and the frame's top edge.
        float outerCornerRadius = 0.0f;
        bool outerTopCornersEnabled = true;
        // Horizontal only: when the list is arranged wider than a line of
        // items, spread the spare width evenly over that line's items so the
        // last one reaches the list's right edge (full-bleed highlights).
        bool justifyHorizontalRows = false;
        // Horizontal only: items narrower than this are widened to it, content
        // stays leading. 0 keeps every item at its natural width.
        float minItemWidth = 0.0f;
        // Horizontal only: > 0 lays the items on a grid of equal columns that
        // span the available width, as many as fit at minItemWidth each but at
        // most this many. An item wider than one column takes as many columns
        // as it needs. Ignored when the available width is unbounded.
        int gridColumns = 0;
        // Vertical only: > 0 flows the items down a column until the next one
        // would pass the column height budget, then starts a new column to the
        // right (column-major). Each column is as wide as its widest item, so
        // columns have different widths. The number of columns is capped by
        // this value and by the column width budget; when even that cap cannot
        // keep the list inside the height budget the last column overflows and
        // is clipped. 0 keeps the single-column vertical layout.
        int verticalColumns = 0;
        // Column-major budgets. When > 0 they override the available size passed
        // to Measure, so the layout is identical no matter what viewport the
        // popup measures the list against (a popup re-measures its child at the
        // host viewport, which is larger than the on-screen space the panel is
        // actually allowed to use). 0 falls back to the available size.
        float columnBudgetHeight = 0.0f;
        float columnBudgetWidth = 0.0f;
        // Gap between columns in the column-major vertical layout. < 0 reuses
        // itemGap.
        float columnGap = -1.0f;
        // false: no item is ever drawn as selected (no fill, bar or selected
        // colors), so every item, the first included, shows hover and press.
        bool selectable = true;
        // Measure exactly to the content: no 80 DIP minimum list width and no
        // slack after the text beyond the annotation gap. Pair it with a
        // contentPadRight equal to contentPadLeft + textPadLeft for even margins.
        bool fitContent = false;
        float contentPadLeft = 5.0f;
        float contentPadRight = 5.0f;
        // Part of itemHeight that is the row's bottom padding. Lines stacked
        // under the first one (wrapped annotation, horizontal translation)
        // start above it, and the padding is restored below the last line.
        float contentPadBottom = 0.0f;
        float textPadLeft = 0.0f;
        float labelGap = 1.5f;
        float tagGap = 3.0f;
        float selectedBarWidth = 3.0f;
        float selectedBarHeight = 12.8f;
        bool showSelectedBar = true;
        // false centres the bar on the row's leading edge; true puts it just inside the row.
        bool selectedBarInside = false;
        D2D1_COLOR_F rowFillHover = D2D1::ColorF(0x343434);
        D2D1_COLOR_F rowFillPressed = D2D1::ColorF(0x353535);
        D2D1_COLOR_F rowFillSelected = D2D1::ColorF(0x3E3E3E, 0.725f);
        // Text/label colors for the selected (and pressed) row. Alpha 0 keeps
        // the normal textColor/labelColor; skins that fill the selected row
        // with an opaque accent set these to the contrasting color (e.g. white
        // on the WeChat green first row). rowTextSelected also drives the
        // annotation (辅助码) and translation on that row, matching the CSS
        // where both inherit the color of `.first .text`.
        D2D1_COLOR_F rowTextSelected = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F rowLabelSelected = D2D1::ColorF(0, 0.0f);
        // Tag color for the selected (and pressed) row. Alpha 0 keeps tagColor,
        // so a faint category tag stays faint on the selected row by default.
        D2D1_COLOR_F rowTagSelected = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F selectedBarColor = D2D1::ColorF(0x6B69D6);
        D2D1_COLOR_F labelColor = D2D1::ColorF(0xE9E8E8, 0.616f);
        // Faint prefix tag drawn between the label and the text.
        D2D1_COLOR_F tagColor = D2D1::ColorF(0xE9E8E8, 0.40f);
        D2D1_COLOR_F textColor = D2D1::ColorF(0xE9E8E8);
        D2D1_COLOR_F annotationColor = D2D1::ColorF(0xE9E8E8);
        // Alpha 0 derives the translation color from the annotation color
        // (including the selected-row override) at 62% opacity, like the
        // CSS default. Otherwise it is drawn as-is on every row.
        D2D1_COLOR_F translationColor = D2D1::ColorF(0, 0.0f);
        // Translation color for the selected (and pressed) row. Alpha 0
        // falls back to translationColor, then to the derived color above.
        D2D1_COLOR_F rowTranslationSelected = D2D1::ColorF(0, 0.0f);
        // Colors of the Secondary / Delimiter translation runs. Alpha 0 uses
        // the translation color of that row. The *Selected variants apply to
        // the selected (and pressed) row and fall back to the normal ones.
        D2D1_COLOR_F translationSecondaryColor = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F translationDelimiterColor = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F rowTranslationSecondarySelected = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F rowTranslationDelimiterSelected = D2D1::ColorF(0, 0.0f);
        // Translation font size as a fraction of fontSize. 0.78 matches the CSS
        // .cand-translation scale; hosts like the expansion panel raise it a bit.
        float translationFontSizeScale = 0.78f;
    };

    using SelectionChangedHandler = std::function<void(size_t selectedIndex)>;
    using ItemActivatedHandler = std::function<void(size_t selectedIndex)>;
    using ContextMenuHandler = std::function<void(size_t selectedIndex, const POINT &clientPoint)>;

    explicit CandidateList(float itemHeight = 40.0f);

    void AddItem(Item item);
    void SetItems(std::vector<Item> items);
    void ClearItems();
    void SetSelectedIndex(size_t index);
    size_t GetSelectedIndex() const;
    const Item *GetItem(size_t index) const;
    // 候选项相对窗口的最终矩形（含铺满后的行宽），供命中测试与布局校验使用。
    RectF GetItemBounds(size_t index) const;
    void SetOnSelectionChanged(SelectionChangedHandler handler);
    void SetOnItemActivated(ItemActivatedHandler handler);
    void SetOnContextMenu(ContextMenuHandler handler);
    void SetAppearance(Appearance appearance);
    // 当前外观（只读）。宿主用它把主列表的字体与字号带到附属列表（如候选译文面板），
    // 再按面板自己的需要覆盖个别字段。
    const Appearance &GetAppearance() const
    {
        return appearance_;
    }
    void SetOrientation(Orientation orientation);
    void SetHoverEnabled(bool enabled);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    bool OnContextMenu(const POINT &point, WPARAM keyState) override;
    void OnMouseLeave() override;
    bool OnKeyDown(WPARAM key, LPARAM lParam) override;
    HCURSOR GetCursor() const override;

  private:
    // 均使用逻辑单位：bounds 相对列表，文字矩形相对候选项；测量、绘制与命中测试共用。
    struct ItemGeometry
    {
        RectF bounds;
        RectF label;
        RectF tag;
        RectF text;
        RectF annotation;
        RectF translation;
    };

    float MeasureTextHeight(const std::wstring &text, float fontSize, float width) const;
    ItemGeometry MeasureItem(size_t index, float width) const;
    // The layout Render draws `text` with in `box`. Layouts are kept across
    // frames by text, size and box rather than by item: a selection move or a
    // late addition redraws the same strings, and the next keystroke's page
    // usually repeats many of them at other indices.
    Microsoft::WRL::ComPtr<IDWriteTextLayout> TextLayoutFor(IDWriteFactory *factory, const std::wstring &fontFamily,
                                                            const std::wstring &text, float fontSize, const RectF &box);
    size_t HitTestItem(const PointF &point) const;
    float EstimateTextWidth(const std::wstring &text, float fontSize) const;
    RectF ItemRect(size_t index) const;
    const std::wstring &ResolvedFontFamily() const;
    // Results of EstimateTextWidth / MeasureTextHeight. A frame measures the
    // list several times (natural size, sticky size, arrange at the final
    // width) and each measurement used to build a fresh DirectWrite layout per
    // string; the metrics depend only on the text, size, width and font, so
    // they are memoized until the font changes.
    bool LookupTextMetric(wchar_t kind, const std::wstring &text, float fontSize, float width, float &value) const;
    void StoreTextMetric(wchar_t kind, const std::wstring &text, float fontSize, float width, float value) const;

    std::vector<Item> items_;
    std::vector<ItemGeometry> itemGeometry_;
    mutable std::unordered_map<std::wstring, float> textMetricCache_;
    mutable std::wstring textMetricFamily_;
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<IDWriteTextLayout>> textLayoutCache_;
    std::wstring textLayoutFamily_;
    float layoutWidth_ = 0.0f;
    // True while the last Measure produced a column-major vertical layout;
    // ItemRect then keeps each item at its own column width instead of
    // stretching it across the list.
    bool columnMajor_ = false;
    Appearance appearance_{};
    Orientation orientation_ = Orientation::Vertical;
    bool focused_ = false;
    bool pressed_ = false;
    bool hoverEnabled_ = true;
    size_t pressedIndex_ = static_cast<size_t>(-1);
    size_t hoveredIndex_ = static_cast<size_t>(-1);
    size_t selectedIndex_ = 0;
    SelectionChangedHandler onSelectionChanged_;
    ItemActivatedHandler onItemActivated_;
    ContextMenuHandler onContextMenu_;
};

// A pair of small "previous" / "next" buttons side by side, e.g. to page a list.
// The arrows are drawn as geometry (stroked chevrons or filled triangles), so they
// do not depend on any font. A disabled button keeps its slot (the layout does
// not jump) and ignores clicks. An optional vertical divider sits before them.
class PagerArrows : public Visual
{
  public:
    enum class Part
    {
        None,
        Previous,
        Next,
    };

    enum class Glyph
    {
        Chevron,
        Triangle,
    };

    struct Appearance
    {
        // Size of one button; the control is two buttons plus gap wide.
        float buttonWidth = 12.0f;
        float buttonHeight = 14.0f;
        float gap = 0.0f;
        Glyph glyph = Glyph::Chevron;
        // Glyph height, centred in its button. strokeWidth is the chevron's line width, or for a
        // triangle how far each corner is rounded off along its edges.
        float glyphSize = 7.0f;
        float strokeWidth = 1.3f;
        float cornerRadius = 3.0f;
        // A vertical rule as tall as the buttons, dividerGap before the previous
        // button. dividerWidth 0 draws nothing and takes no space.
        float dividerWidth = 0.0f;
        float dividerGap = 0.0f;
        D2D1_COLOR_F dividerColor = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F glyphColor = D2D1::ColorF(0xE9E8E8, 0.8f);
        D2D1_COLOR_F disabledGlyphColor = D2D1::ColorF(0xE9E8E8, 0.25f);
        D2D1_COLOR_F hoverFill = D2D1::ColorF(0x414141);
        D2D1_COLOR_F pressedFill = D2D1::ColorF(0x353535);

        // Horizontal extent of one glyph, so callers can line its tip up with neighbouring content.
        float GlyphWidth() const
        {
            return glyph == Glyph::Triangle ? glyphSize * kTriangleAspect : glyphSize * 0.5f * 0.55f;
        }

        // Width of a triangle over its height.
        static constexpr float kTriangleAspect = 0.84f;
    };

    using ClickHandler = std::function<void(Part part)>;

    PagerArrows() = default;

    void SetAppearance(Appearance appearance);
    void SetEnabled(bool previous, bool next);
    bool IsPartEnabled(Part part) const;
    void SetOnClick(ClickHandler handler);
    // Hover feedback can be switched off, e.g. until the pointer really moves.
    void SetHoverEnabled(bool enabled);
    // Which button a point (DIPs, window space) falls on.
    Part HitTestPart(const PointF &point) const;
    RectF GetPartBounds(Part part) const;

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    void OnMouseLeave() override;
    HCURSOR GetCursor() const override;

  private:
    Appearance appearance_{};
    bool previousEnabled_ = true;
    bool nextEnabled_ = true;
    bool hoverEnabled_ = true;
    Part hovered_ = Part::None;
    Part pressed_ = Part::None;
    ClickHandler onClick_;
};

class TreeView : public Visual
{
  public:
    struct Node
    {
        std::wstring title;
        std::wstring subtitle;
        bool expanded = true;
        std::vector<Node> children;
    };

    using SelectionChangedHandler = std::function<void(const std::wstring &selectedTitle)>;

    explicit TreeView(float itemHeight = 62.0f);

    void AddRoot(Node node);
    void Clear();
    void SetOnSelectionChanged(SelectionChangedHandler handler);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    struct VisibleNode
    {
        Node *node = nullptr;
        size_t depth = 0;
        RectF rowRect = {};
        RectF expanderRect = {};
        float titleWidth = -1.0f;
        float subtitleWidth = -1.0f;
        std::wstring fontFamily;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> titleLayout;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> subtitleLayout;
    };

    void BuildVisibleNodes();
    void AppendVisibleNodes(Node &node, size_t depth);
    void InvalidateLayoutCache();
    VisibleNode *HitTestVisibleNode(const PointF &point);
    const VisibleNode *HitTestVisibleNode(const PointF &point) const;
    void SelectNode(Node *node);

    std::vector<Node> roots_;
    std::vector<VisibleNode> visibleNodes_;
    float itemHeight_ = 62.0f;
    bool focused_ = false;
    bool pressed_ = false;
    Node *pressedNode_ = nullptr;
    bool pressedExpander_ = false;
    Node *selectedNode_ = nullptr;
    SelectionChangedHandler onSelectionChanged_;
};

class TabControl : public Visual
{
  public:
    struct Tab
    {
        std::wstring title;
        std::shared_ptr<Visual> content;
    };

    using SelectionChangedHandler = std::function<void(size_t selectedIndex)>;

    explicit TabControl(float headerHeight = 46.0f);

    void AddTab(std::wstring title, std::shared_ptr<Visual> content);
    void ClearTabs();
    void SetSelectedIndex(size_t index);
    size_t GetSelectedIndex() const;
    void SetOnSelectionChanged(SelectionChangedHandler handler);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;
    bool HitTest(const PointF &point) const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    size_t HitTestHeader(const PointF &point) const;

    std::vector<Tab> tabs_;
    std::vector<RectF> headerRects_;
    float headerHeight_ = 46.0f;
    size_t selectedIndex_ = 0;
    bool pressed_ = false;
    size_t pressedIndex_ = static_cast<size_t>(-1);
    SelectionChangedHandler onSelectionChanged_;
};

class Accordion : public Visual
{
  public:
    struct Section
    {
        std::wstring title;
        std::shared_ptr<Visual> content;
        bool expanded = true;
    };

    explicit Accordion(float headerHeight = 48.0f);

    void AddSection(std::wstring title, std::shared_ptr<Visual> content, bool expanded = true);
    void ClearSections();
    void SetAllowMultipleExpanded(bool allowMultipleExpanded);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;
    bool HitTest(const PointF &point) const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    void CollapseOtherSections(size_t keepExpandedIndex);
    size_t HitTestHeader(const PointF &point) const;

    std::vector<Section> sections_;
    std::vector<RectF> headerRects_;
    std::vector<RectF> contentRects_;
    float headerHeight_ = 48.0f;
    bool allowMultipleExpanded_ = true;
    bool pressed_ = false;
    size_t pressedIndex_ = static_cast<size_t>(-1);
};
} // namespace msimeui
