#pragma once

#include "settings/native/native_settings_search.h"
#include "settings/native/native_settings_theme.h"

#include "msimeui/Animation.h"
#include "msimeui/Controls.h"
#include "msimeui/Layout.h"
#include "msimeui/ScrollAxis.h"

#include <dwrite.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Controls of the Direct2D settings window. Each one was ported from a component of the
// former web settings page (see native_settings_pages.h); the CSS selector it came from
// is named on the class, and the numbers inside come from that rule. Every color is read from CurrentPalette() at
// render time, so switching the theme only needs a repaint.
//
// Layout units are DIPs, which is what CSS px are at the page's zoom factor 1.0.
namespace native_settings
{
using VisualPtr = std::shared_ptr<msimeui::Visual>;
using msimeui::PointF;
using msimeui::RectF;
using msimeui::SizeF;
using msimeui::Thickness;

// ---------------------------------------------------------------------------------
// Drawing helpers shared by the controls and the pages.
// ---------------------------------------------------------------------------------
IDWriteFactory *DWriteFactory();
D2D1_RECT_F ToD2D(const RectF &rect);
bool Contains(const RectF &rect, const PointF &point);
RectF Inflate(const RectF &rect, float amount);
RectF Deflate(const RectF &rect, const Thickness &thickness);
void FillRect(msimeui::DeviceResources &dr, const RectF &rect, const D2D1_COLOR_F &color);
void FillRounded(msimeui::DeviceResources &dr, const RectF &rect, float radius, const D2D1_COLOR_F &color);
// Stroke drawn inside `rect`, like a CSS border.
void StrokeRounded(msimeui::DeviceResources &dr, const RectF &rect, float radius, const D2D1_COLOR_F &color,
                   float width = 1.0f);
void DrawLine(msimeui::DeviceResources &dr, PointF from, PointF to, const D2D1_COLOR_F &color, float width = 1.0f);
// CSS `border-right + border-bottom` chevron rotated by `degrees` around its center.
void DrawChevron(msimeui::DeviceResources &dr, PointF center, float size, float degrees, const D2D1_COLOR_F &color,
                 float strokeWidth = 2.0f);
// Wraps PushLayer for CSS `opacity` on a subtree. Pair with PopOpacity.
void PushOpacity(msimeui::DeviceResources &dr, float opacity);
void PopOpacity(msimeui::DeviceResources &dr);
// Draws an SVG into `box`, rounded to whole device pixels so the rasterized bitmap is
// copied 1:1 instead of being resampled (which blurs thin strokes).
void DrawSvg(msimeui::DeviceResources &dr, const std::string &markup, const RectF &box, float opacity = 1.0f);

// Text antialiasing for a block of drawing; restored on scope exit.
class ScopedTextAntialias
{
  public:
    ScopedTextAntialias(msimeui::DeviceResources &dr, D2D1_TEXT_ANTIALIAS_MODE mode);
    ~ScopedTextAntialias();
    ScopedTextAntialias(const ScopedTextAntialias &) = delete;
    ScopedTextAntialias &operator=(const ScopedTextAntialias &) = delete;

  private:
    ID2D1RenderTarget *target_ = nullptr;
    D2D1_TEXT_ANTIALIAS_MODE previous_ = D2D1_TEXT_ANTIALIAS_MODE_DEFAULT;
};

// Text style for a run of CSS text.
struct TextStyle
{
    float size = kFontBody;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    // Empty is the page font (UiFontFamily()).
    std::wstring family;
    // CSS line-height as a multiple of the font size; 0 is `normal`.
    float lineHeight = 0.0f;
    bool wrap = false;
    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
    // -webkit-text-stroke width; the page uses 0.25px to fake a Medium weight.
    float stroke = 0.0f;
    float letterSpacing = 0.0f;
};

// A DirectWrite layout cached by text, style and width.
class TextLayout
{
  public:
    void Set(std::wstring text, TextStyle style);
    void SetText(std::wstring text);
    const std::wstring &Text() const
    {
        return text_;
    }
    const TextStyle &Style() const
    {
        return style_;
    }
    // Layout for a maximum width (ignored for unwrapped text).
    IDWriteTextLayout *Get(float maxWidth);
    SizeF Measure(float maxWidth);
    void Draw(msimeui::DeviceResources &dr, PointF origin, const D2D1_COLOR_F &color, float maxWidth);

  private:
    std::wstring text_;
    TextStyle style_;
    float width_ = -1.0f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout_;
};

// Width of single-line text, rounded up like a CSS box.
float MeasureTextWidth(const std::wstring &text, const TextStyle &style);
float LineHeight(const TextStyle &style);
// Draws single-line text vertically centered in `rect`, clipped to it.
void DrawTextInRect(msimeui::DeviceResources &dr, const std::wstring &text, const TextStyle &style, const RectF &rect,
                    const D2D1_COLOR_F &color, bool center = false);

// A color that eases to its new value, like a CSS `transition` on a color property.
class ColorTransition
{
  public:
    ColorTransition() = default;
    explicit ColorTransition(double durationMs, msimeui::Easing easing = msimeui::Easing::Ease());
    // Snaps on the first call.
    void SetTarget(const D2D1_COLOR_F &target, msimeui::Window *window);
    // Value for this frame; asks the window for another frame while moving.
    D2D1_COLOR_F Value(msimeui::Window *window);

  private:
    D2D1_COLOR_F from_ = {};
    D2D1_COLOR_F to_ = {};
    msimeui::Tween progress_{1.0f};
    double durationMs_ = 200.0;
    msimeui::Easing easing_ = msimeui::Easing::Ease();
    bool initialized_ = false;
};

// ---------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------
enum class TextRole
{
    Primary,   // --text-color
    Secondary, // --text-secondary
    Muted,     // --text-muted
    Subtle,    // --text-subtle
    Sidebar,   // --sidebar-item-color
    Accent,    // --accent-color
    Danger,    // --danger-text
    Custom,
};
D2D1_COLOR_F RoleColor(TextRole role);

class Label : public msimeui::Visual
{
  public:
    Label(std::wstring text, TextStyle style, TextRole role = TextRole::Primary);

    void SetText(std::wstring text);
    const std::wstring &GetText() const;
    void SetRole(TextRole role);
    void SetColor(D2D1_COLOR_F color);
    void SetStyle(TextStyle style);
    void SetOpacity(float opacity);
    // Pointer cursor and click handler, for link-like labels.
    void SetOnClick(std::function<void()> handler);
    void SetHoverColor(D2D1_COLOR_F color);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    void OnMouseEnter() override;
    void OnMouseLeave() override;
    HCURSOR GetCursor() const override;

  private:
    TextLayout layout_;
    TextRole role_ = TextRole::Primary;
    D2D1_COLOR_F color_ = {};
    D2D1_COLOR_F hoverColor_ = {};
    float opacity_ = 1.0f;
    std::function<void()> onClick_;
    bool hovered_ = false;
    bool pressed_ = false;
};

// Page-wide text styles.
TextStyle PageTitleStyle();    // .content-header span
TextStyle SectionTitleStyle(); // .section-title
TextStyle SubItemTitleStyle(); // .section-item-row .section-title and friends
TextStyle BodyStyle();         // 14px text in the page font
TextStyle ControlStyle();      // 14px form-control text in the system font (no `font: inherit`)
TextStyle CaptionStyle();      // the shared *-description rule

// Setting titles. Both enter the search index (see IndexSettingTitle); `searchDescription` is the
// text shown under the title, matched as a secondary hit.
std::shared_ptr<Label> MakeTitle(std::wstring text, std::wstring searchDescription = {});
std::shared_ptr<Label> MakeSubTitle(std::wstring text, std::wstring searchDescription = {});
// Muted 12px / 1.6 description that wraps.
std::shared_ptr<Label> MakeDescription(std::wstring text);

// ---------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------

// Vertical stack with CSS-like margins on its children (msimeui::StackPanel plus
// display:none support through Hideable children).
class Column : public msimeui::StackPanel
{
  public:
    explicit Column(float gap = 0.0f);
    Column &Add(VisualPtr child);
};

// Horizontal flexbox: children keep their natural width except those added with a
// grow factor, which share what is left. Cross axis is centered by default.
class Row : public msimeui::Panel
{
  public:
    enum class Align
    {
        Start,
        Center,
        End,
        Stretch,
    };
    enum class Justify
    {
        Start,
        Center,
        End,
        SpaceBetween,
    };

    explicit Row(float gap = 0.0f);
    Row &Add(VisualPtr child, float grow = 0.0f);
    void SetAlign(Align align);
    void SetJustify(Justify justify);
    // flex-wrap: wrap with `rowGap` between lines.
    void SetWrap(bool wrap, float rowGap = 0.0f);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;

  private:
    struct Line
    {
        size_t begin = 0;
        size_t end = 0;
        float width = 0.0f;
        float height = 0.0f;
    };

    float gap_ = 0.0f;
    float rowGap_ = 0.0f;
    bool wrap_ = false;
    Align align_ = Align::Center;
    Justify justify_ = Justify::Start;
    std::vector<float> grow_;
    std::vector<SizeF> sizes_;
    std::vector<Line> lines_;
};

// Centers its child and caps its width (`max-width` + `margin: 0 auto`).
class CenteredColumn : public msimeui::Container
{
  public:
    CenteredColumn(VisualPtr child, float maxWidth);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;

  private:
    float maxWidth_ = 900.0f;
};

// display: none / block. A hidden child takes no space and gets no input.
class Hideable : public msimeui::Container
{
  public:
    explicit Hideable(VisualPtr child, bool visible = true);
    void SetVisible(bool visible);
    bool IsVisible() const
    {
        return visible_;
    }
    // How something outside the user's click can open this, e.g. the expander that owns it.
    // Without a handler the Hideable stays hidden when asked to reveal: its visibility then
    // follows a setting, and showing it would contradict that setting.
    void SetOnReveal(std::function<void()> handler);
    void Reveal();

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    msimeui::Visual *FindVisualAt(const PointF &point) override;
    msimeui::Visual *FindFocusableAt(const PointF &point) override;
    msimeui::Visual *FindFirstFocusableDescendant() override;

  private:
    bool visible_ = true;
    std::function<void()> onReveal_;
    // The margin given to the Hideable, taken over as inner spacing so that it disappears
    // with the child: `display: none` takes no room, margins included.
    Thickness spacing_ = {};
};

// `opacity: 0.45; pointer-events: none` while disabled, the page's common way of
// greying out a group of controls whose master switch is off.
class Dimmable : public msimeui::Container
{
  public:
    explicit Dimmable(VisualPtr child, float disabledOpacity = 0.45f);
    void SetDisabled(bool disabled);
    bool IsDisabled() const
    {
        return disabled_;
    }

    void Render(msimeui::DeviceResources &dr) override;
    msimeui::Visual *FindVisualAt(const PointF &point) override;
    msimeui::Visual *FindFocusableAt(const PointF &point) override;

  private:
    bool disabled_ = false;
    float opacity_ = 0.45f;
};

// Fixed-size empty box.
class Spacer : public msimeui::Visual
{
  public:
    Spacer(float width, float height);
    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;

  private:
    float width_ = 0.0f;
    float height_ = 0.0f;
};

// ---------------------------------------------------------------------------------
// Page structure
// ---------------------------------------------------------------------------------

// `.section`: rounded card with a hairline border and --card-shadow; `padding: 20px 24px`
// inside the 1px border.
class Section : public msimeui::Container
{
  public:
    explicit Section(VisualPtr child);
    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;

  private:
    msimeui::ShadowCache shadow_;
};

// `.divider` (0.5px, margin 12px 0) or `.input-option-divider` (0.8px, no margin).
class Divider : public msimeui::Visual
{
  public:
    enum class Kind
    {
        Section,
        Option,
    };
    explicit Divider(Kind kind = Kind::Section);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;

  private:
    Kind kind_ = Kind::Section;
};

// `.section-header`: text block on the left, control on the right, `gap: 10px`,
// centered vertically. The title never shrinks; the description wraps instead.
class SettingRow : public msimeui::Panel
{
  public:
    // `left` is usually a Column of title + description; `control` may be null.
    SettingRow(VisualPtr left, VisualPtr control);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;

  private:
    VisualPtr left_;
    VisualPtr control_;
    SizeF leftSize_ = {};
    SizeF controlSize_ = {};
};

// Opens every collapsed group between `visual` and the page root that can be opened, so a row
// found by search is on screen. Returns false when some ancestor stays hidden.
bool RevealInAncestors(msimeui::Visual &visual);

// Title (+ description) block used on the left of a SettingRow.
VisualPtr MakeTitleBlock(std::wstring title, std::wstring description = {}, bool subItem = false);
// The common row: section title, optional description, control on the right.
std::shared_ptr<SettingRow> MakeRow(std::wstring title, std::wstring description, VisualPtr control,
                                    bool subItem = false);

// Puts a setting title into the search index of the page being built. `title` is the visual that
// shows it; a search hit scrolls to and highlights the SettingRow around it, or its block when it
// heads one of its own. MakeTitle, MakeSubTitle and MakeExpander call this, so a setting built from
// them is searchable however its row is put together; a page that draws a title some other way
// calls it directly.
void IndexSettingTitle(const std::shared_ptr<msimeui::Visual> &title, std::wstring text, std::wstring description = {});

// Scope that tells IndexSettingTitle which page is being built, so every title created in it lands
// in that page's slice of the search index. Set by the window around a PageInfo::build call;
// nested scopes restore the previous one. Without it titles still build, they are just not indexed,
// which is what happens in the control tests.
class SettingsIndexScope
{
  public:
    struct Row
    {
        std::wstring title;
        std::wstring description;
        std::weak_ptr<msimeui::Visual> target;
    };

    // `collector` is called once per title created inside the scope, in build order.
    SettingsIndexScope(std::string pageId, std::function<void(const Row &)> collector);
    ~SettingsIndexScope();
    SettingsIndexScope(const SettingsIndexScope &) = delete;
    SettingsIndexScope &operator=(const SettingsIndexScope &) = delete;
};

// One indexed settings row, plus the matcher that ranks it. Defined in native_settings_search.h
// because that header and its .cpp stay free of D2D so they can run in the portable test target.
// (including the header here re-exports them to every page, which is what the pages want)

// `.content-header` + sections, wrapped in the page column (max 900, centered).
class PageBuilder
{
  public:
    explicit PageBuilder(std::wstring title);
    // Adds a `.section` card holding `content`; returns the card.
    std::shared_ptr<Section> AddSection(VisualPtr content);
    // Adds any visual between sections (it gets the same 18px bottom margin).
    void AddRaw(VisualPtr visual, float marginBottom = 18.0f);
    // The finished page content (not yet scrollable; the window scrolls it).
    VisualPtr Build();
    // `.document-page` pages show the title in --text-secondary.
    void SetTitleRole(TextRole role);

  private:
    std::shared_ptr<Column> column_;
    std::shared_ptr<Label> header_;
};

// Rows separated by `.divider`, the most common card body.
std::shared_ptr<Column> MakeRows(const std::vector<VisualPtr> &rows);

// ---------------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------------

// `.ftb-toggle-btn`: 38x19 track, 16px knob, `transition: all 0.2s ease`.
class ToggleSwitch : public msimeui::Visual
{
  public:
    using ChangeHandler = std::function<void(bool on)>;

    explicit ToggleSwitch(bool on = false);
    void SetOnChanged(ChangeHandler handler);
    // Programmatic changes animate too but do not call the handler.
    void SetChecked(bool on);
    bool IsChecked() const;
    // aria-disabled: keeps showing the state, ignores input, greyed by its container.
    void SetDisabled(bool disabled);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    bool IsFocusable() const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnKeyDown(WPARAM key, LPARAM lParam) override;
    HCURSOR GetCursor() const override;

  private:
    void Toggle();

    bool on_ = false;
    bool pressed_ = false;
    bool disabled_ = false;
    msimeui::Tween knob_{0.0f};
    ChangeHandler onChanged_;
};

// The scrollbar of a `.compact-scroll-menu` list (dropdown menus, the AI model list): a
// 4 DIP thumb 2 DIPs from the list's right edge and 4 from each end, at least 24 long. A
// band along the right edge, wider than the thumb, takes presses and hover while the
// content overflows. It owns the list's scroll offset; the list draws its rows at
// GetPlacedOffset and forwards the mouse here first.
//
// The list calls Layout whenever its content or box changes, before any other call.
class CompactMenuScrollbar
{
  public:
    // `contentHeight` covers the rows and their inset. `viewport` is the height shown, or
    // the height the list asks for while it is not arranged yet. `bounds` is the list's
    // box.
    void Layout(float contentHeight, float viewport, const RectF &bounds);
    // Back to the top, with no drag or hover.
    void Reset();

    bool CanScroll() const;
    float GetOffset() const;
    // The offset rounded to whole device pixels at `dpi`: scrolled rows keep the pixel
    // phase they have unscrolled.
    float GetPlacedOffset(float dpi) const;
    // The following return whether the list has to be redrawn.
    bool SetOffset(float offset);
    // Scrolls as little as possible to show content [start, start + length).
    bool Reveal(float start, float length);
    bool Wheel(short delta);

    bool IsOnScrollbar(const PointF &point) const;
    bool IsDragging() const;
    // Drawn in the hot color: the pointer is over the scrollbar, or dragging the thumb
    // wherever it is.
    bool IsHot() const;
    // A press on the scrollbar starts a drag that lasts until Release; the pointer may
    // leave the list meanwhile.
    bool Press(const PointF &point);
    // Pointer moves: drags while dragging with the left button held, otherwise tracks
    // hover.
    bool Move(const PointF &point, WPARAM keyState);
    bool Release(const PointF &point);
    bool Leave();

    // `hotColor` while IsHot.
    void Render(msimeui::DeviceResources &dr, const D2D1_COLOR_F &color, const D2D1_COLOR_F &hotColor) const;

  private:
    msimeui::ScrollExtent Extent() const;
    msimeui::ScrollTrack Track() const;

    float contentHeight_ = 0.0f;
    float viewport_ = 0.0f;
    RectF bounds_ = {};
    float offset_ = 0.0f;
    bool hovered_ = false;
    bool dragging_ = false;
    // Where along the thumb the pointer holds it while dragging.
    float grip_ = 0.0f;
};

// One entry of a dropdown menu.
struct DropdownOption
{
    std::wstring label;
    std::string value;
    bool disabled = false;
    // Font preview of the system-font menu (empty uses the page font).
    std::wstring previewFamily;
};

class DropdownMenu;
class DropdownPopup;

// `.dropdown` + `.dropdown-toggle` + `.dropdown-menu`.
class Dropdown : public msimeui::Visual
{
  public:
    using ChangeHandler = std::function<void(size_t index, const DropdownOption &option)>;

    struct Options
    {
        // `.compact-scroll-menu`: shows six rows and scrolls the rest.
        bool compact = false;
        // Menu as wide as the toggle (left: 0; right: 0) instead of shrink-to-fit,
        // right-aligned to it.
        bool matchToggleWidth = false;
        float minWidth = 100.0f;
        float fixedWidth = 0.0f;
        // `.font-combobox`: an editable search box instead of the label.
        bool searchable = false;
        // Left padding of the toggle (`padding: 4px 6px 4px <left>`).
        float paddingLeft = 12.0f;
        // Right padding before the arrow (`padding: 4px <right> ...`).
        float paddingRight = 6.0f;
        // Shrink-to-fit menu aligned to the toggle's left edge (`left: 0; right: auto`)
        // with these width bounds (0 = none).
        bool menuAlignLeft = false;
        float menuMinWidth = 0.0f;
        float menuMaxWidth = 0.0f;
        // Placeholder of the searchable box while it is empty.
        std::wstring placeholder;
        // font-weight inherited from the surrounding element (toggle label and menu items).
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    };

    Dropdown(std::vector<DropdownOption> options, Options settings);
    Dropdown(std::vector<DropdownOption> options);

    void SetOnChanged(ChangeHandler handler);
    void SetOptions(std::vector<DropdownOption> options);
    const std::vector<DropdownOption> &GetOptions() const;
    // Selects the option with `value`; an unknown value shows the value itself as the
    // label, like applyDropdownValue.
    void SetValue(const std::string &value);
    std::string GetValue() const;
    void SetSelectedIndex(size_t index);
    size_t GetSelectedIndex() const;
    void SetLabelOverride(std::wstring label);
    void SetDisabled(bool disabled);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    void Attach(msimeui::Window *window) override;
    bool IsFocusable() const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    void OnMouseEnter() override;
    void OnMouseLeave() override;
    bool OnKeyDown(WPARAM key, LPARAM lParam) override;
    bool KeepsPopupsOpenOnClick() const override;
    HCURSOR GetCursor() const override;
    msimeui::Visual *FindVisualAt(const PointF &point) override;
    msimeui::Visual *FindFocusableAt(const PointF &point) override;

  private:
    friend class DropdownMenu;
    void Open();
    void Close();
    void Commit(size_t index);
    std::wstring CurrentLabel() const;
    TextStyle LabelStyle() const;
    void ApplySearch(const std::wstring &query);

    std::vector<DropdownOption> options_;
    Options settings_;
    size_t selected_ = static_cast<size_t>(-1);
    std::wstring unknownLabel_;
    std::wstring labelOverride_;
    float maxTextWidth_ = -1.0f;
    bool hovered_ = false;
    bool pressed_ = false;
    bool open_ = false;
    bool disabled_ = false;
    ColorTransition background_{200.0};
    ColorTransition borderColor_{200.0};
    ChangeHandler onChanged_;
    std::shared_ptr<DropdownMenu> menu_;
    std::shared_ptr<DropdownPopup> popup_;
    std::shared_ptr<msimeui::TextBox> search_;
    int searchThemedLight_ = -1;
};

// `.check-option`: 18px box with a 2px border, label at 14px, `padding: 8px 4px`.
class CheckOption : public msimeui::Visual
{
  public:
    using ChangeHandler = std::function<void(bool checked)>;

    CheckOption(std::wstring text, bool checked = false);
    void SetOnChanged(ChangeHandler handler);
    void SetChecked(bool checked);
    bool IsChecked() const;
    void SetDisabled(bool disabled);
    void SetText(std::wstring text);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    bool IsFocusable() const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    void OnMouseEnter() override;
    void OnMouseLeave() override;
    bool OnKeyDown(WPARAM key, LPARAM lParam) override;

  private:
    TextLayout text_;
    bool checked_ = false;
    bool hovered_ = false;
    bool pressed_ = false;
    bool disabled_ = false;
    ChangeHandler onChanged_;
};

// `.radio-option`: 20px ring with a 10px dot, label at 14px, `gap: 10px`.
class RadioOption : public msimeui::Visual
{
  public:
    RadioOption(std::wstring text, bool checked = false);
    void SetOnSelected(std::function<void()> handler);
    void SetChecked(bool checked);
    bool IsChecked() const;
    void SetDisabled(bool disabled);
    // Extra padding around the option (several pages use `padding: 8px 4px`).
    void SetInnerPadding(Thickness padding);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    bool IsFocusable() const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;

  private:
    TextLayout text_;
    Thickness inner_ = {};
    bool checked_ = false;
    bool pressed_ = false;
    bool disabled_ = false;
    std::function<void()> onSelected_;
};

// A set of RadioOptions bound to one value.
class RadioGroup
{
  public:
    using ChangeHandler = std::function<void(const std::string &value)>;

    std::shared_ptr<RadioOption> Add(std::wstring text, std::string value);
    void SetValue(const std::string &value);
    const std::string &GetValue() const;
    void SetOnChanged(ChangeHandler handler);
    void SetDisabled(bool disabled);

  private:
    std::vector<std::pair<std::string, std::shared_ptr<RadioOption>>> options_;
    std::string value_;
    ChangeHandler onChanged_;
};

// Button looks used across the page.
enum class ButtonKind
{
    Secondary, // .restart-server-button: dropdown-bg, 1px border, min-width 100
    Primary,   // .dict-button.primary: accent fill, white text, 34px tall
    Neutral,   // .dict-button.secondary
    Danger,    // .dict-button.danger
    Soft,      // .cand-color-reset: 12px text, 6px radius
    Link,      // text-only accent link
};

class Button : public msimeui::Visual
{
  public:
    Button(std::wstring text, ButtonKind kind = ButtonKind::Secondary);

    void SetOnClick(std::function<void()> handler);
    void SetText(std::wstring text);
    void SetDisabled(bool disabled);
    bool IsDisabled() const
    {
        return disabled_;
    }
    // `.cand-color-reset.is-active` and similar toggled states.
    void SetActive(bool active);
    void SetMinWidth(float width);
    void SetHeight(float height);
    void SetFontSize(float size);
    // Leading SVG icon (markup with currentColor), `gap` before the text.
    void SetIcon(std::string svgMarkup, float size, float gap = 6.0f);
    void SetTooltip(std::wstring tooltip);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    bool IsFocusable() const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    void OnMouseEnter() override;
    void OnMouseLeave() override;
    bool OnKeyDown(WPARAM key, LPARAM lParam) override;
    HCURSOR GetCursor() const override;

  private:
    D2D1_COLOR_F FillColor() const;
    D2D1_COLOR_F BorderColor() const;
    D2D1_COLOR_F TextColor() const;

    std::wstring text_;
    ButtonKind kind_ = ButtonKind::Secondary;
    float minWidth_ = -1.0f;
    float height_ = -1.0f;
    float fontSize_ = -1.0f;
    std::string icon_;
    float iconSize_ = 0.0f;
    float iconGap_ = 0.0f;
    bool hovered_ = false;
    bool pressed_ = false;
    bool disabled_ = false;
    bool active_ = false;
    ColorTransition fill_{200.0};
    ColorTransition border_{200.0};
    std::function<void()> onClick_;
};

// Single-line input or textarea in the page's control look (border --border-color,
// --control-bg fill, 8px radius, accent border while focused). Wraps msimeui's TSF
// text box, so the IME works inside it.
class TextField : public msimeui::Container
{
  public:
    struct Options
    {
        float height = 30.0f;
        bool multiline = false;
        bool password = false;
        // Eye button that reveals a password, like `.ai-secret-icon`.
        bool revealButton = false;
        float fontSize = kFontBody;
        float paddingX = 10.0f;
        float paddingY = 5.0f;
        float radius = 8.0f;
        std::wstring placeholder;
        bool monospace = false;
    };

    explicit TextField(Options options);

    void SetText(const std::wstring &text);
    std::wstring GetText() const;
    // Every edit.
    void SetOnChanged(std::function<void(const std::wstring &)> handler);
    // Focus left or Enter pressed with a changed value: the moment the page posts its
    // `change` event and writes the config.
    void SetOnCommit(std::function<void(const std::wstring &)> handler);
    void SetDisabled(bool disabled);
    void SetPlaceholder(std::wstring placeholder);
    const std::shared_ptr<msimeui::TextBox> &Box() const
    {
        return box_;
    }

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    msimeui::Visual *FindVisualAt(const PointF &point) override;
    msimeui::Visual *FindFocusableAt(const PointF &point) override;

  private:
    void ApplyAppearance();
    RectF RevealRect() const;

    class RevealButton;
    Options options_;
    std::shared_ptr<msimeui::TextBox> box_;
    std::shared_ptr<RevealButton> reveal_;
    std::wstring committed_;
    std::function<void(const std::wstring &)> onChanged_;
    std::function<void(const std::wstring &)> onCommit_;
    bool revealed_ = false;
    bool appliedLight_ = false;
};

// `.number-stepper`: − [value] +, 30px tall, values clamped to [min, max].
class NumberStepper : public msimeui::Visual
{
  public:
    NumberStepper(int minimum, int maximum, int step = 1);

    void SetValue(int value);
    int GetValue() const;
    void SetOnChanged(std::function<void(int)> handler);
    void SetSuffix(std::wstring suffix);
    void SetDisabled(bool disabled);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    void OnMouseLeave() override;
    bool OnMouseWheel(const POINT &point, short delta, WPARAM keyState) override;
    bool IsFocusable() const override;
    bool OnKeyDown(WPARAM key, LPARAM lParam) override;

  private:
    int HitPart(const PointF &point) const; // -1 minus, 1 plus, 0 value
    void Change(int value);

    int minimum_ = 0;
    int maximum_ = 100;
    int step_ = 1;
    int value_ = 0;
    std::wstring suffix_;
    int hovered_ = 2;
    int pressed_ = 2;
    bool disabled_ = false;
    std::function<void(int)> onChanged_;
};

// Clickable header with the rotating chevron (`.toggle-section-expand` /
// `.theme-surface-expand`) and the details it opens (`margin: 4px 0 0 18px`).
class Expander : public msimeui::Visual
{
  public:
    explicit Expander(std::wstring title, bool subItem = false);
    Expander(std::wstring title, const TextStyle &style);
    void SetOnToggled(std::function<void(bool expanded)> handler);
    void SetExpanded(bool expanded);
    bool IsExpanded() const;

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    TextLayout title_;
    bool expanded_ = false;
    bool pressed_ = false;
    msimeui::Tween rotation_{0.0f};
    std::function<void(bool)> onToggled_;
};

// An expander whose title is indexed for search.
std::shared_ptr<Expander> MakeExpander(std::wstring title, bool subItem = false);
std::shared_ptr<Expander> MakeExpander(std::wstring title, const TextStyle &style);

// Wires an expander to the details it opens: a click toggles them, and a search hit inside
// them expands both, so the chevron never disagrees with what is shown.
void BindExpander(const std::shared_ptr<Expander> &expander, const std::shared_ptr<Hideable> &details);

// `.shortcut-keys` / <kbd>: key caps joined by "+".
class KeyCaps : public msimeui::Visual
{
  public:
    explicit KeyCaps(std::vector<std::wstring> keys);
    void SetKeys(std::vector<std::wstring> keys);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;

  private:
    std::vector<std::wstring> keys_;
    std::vector<float> widths_;
    float plusWidth_ = 0.0f;
};

// An SVG drawn at a fixed size. Markup may use `currentColor`; `tint` replaces it.
class SvgImage : public msimeui::Visual
{
  public:
    SvgImage(std::string markup, float width, float height);
    void SetMarkup(std::string markup);
    // Draws `currentColor` and every fill in this color (alpha 0 keeps the markup).
    void SetTint(std::function<D2D1_COLOR_F()> tint);
    void SetOpacity(float opacity);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;

  private:
    std::string markup_;
    std::string tinted_;
    D2D1_COLOR_F tintedFor_ = {};
    float width_ = 0.0f;
    float height_ = 0.0f;
    float opacity_ = 1.0f;
    std::function<D2D1_COLOR_F()> tint_;
};

// Bitmap file (PNG/JPEG) drawn at a fixed size with high-quality scaling.
class BitmapImage : public msimeui::Visual
{
  public:
    BitmapImage(std::wstring path, float width, float height, float radius = 0.0f);
    void SetPath(std::wstring path);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;

  private:
    std::wstring path_;
    float width_ = 0.0f;
    float height_ = 0.0f;
    float radius_ = 0.0f;
};

// Reads an SVG asset of the settings window (<data dir>\assets\settings\<relative>).
std::string LoadAssetSvg(const std::wstring &relativePath);
std::wstring AssetPath(const std::wstring &relativePath);
// CSS `filter: invert(1) brightness(b)` applied to every color literal of an SVG,
// which is how the page turns its white icons dark in the light theme.
std::string InvertSvgColors(const std::string &markup, float brightness);
// Replaces `currentColor` and fill/stroke colors with `color`.
std::string TintSvg(const std::string &markup, const D2D1_COLOR_F &color);
} // namespace native_settings
