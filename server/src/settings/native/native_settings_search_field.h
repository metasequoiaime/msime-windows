#pragma once

// The sidebar's search box, split out of native_settings_window.cpp so the settings UI tests can
// drive the shipped control through a real msimeui::Window.

#include "settings/native/native_settings_controls.h"
#include "settings/native/native_settings_theme.h"

#include "msimeui/Controls.h"
#include "msimeui/Layout.h"

#include <functional>
#include <memory>
#include <string>

namespace native_settings
{
// WinUI 3 control colors for the search box and its suggestions. The search box follows the
// Fluent AutoSuggestBox rather than the form controls ported from the web page, so it takes the
// system's token values (ControlFill*, ControlStroke*, SubtleFill*) instead of the CSS palette.
struct FluentColors
{
    D2D1_COLOR_F fillRest;
    D2D1_COLOR_F fillHover;
    D2D1_COLOR_F fillFocused;
    D2D1_COLOR_F stroke;
    // The darker bottom edge that gives a text box its sunken look.
    D2D1_COLOR_F strokeBottom;
    D2D1_COLOR_F subtleHover;
    D2D1_COLOR_F subtlePressed;
    D2D1_COLOR_F glyph;
    D2D1_COLOR_F flyoutFill;
    D2D1_COLOR_F flyoutStroke;
};

const FluentColors &Fluent(const Palette &p);

// A TextBox that reports the pointer coming and going: the search field draws the chrome around
// it, and its hover look has to follow the pointer over the text too.
class HoverTextBox : public msimeui::TextBox
{
  public:
    using msimeui::TextBox::TextBox;

    void SetOnHoverChanged(std::function<void(bool hovered)> handler);
    void OnMouseEnter() override;
    void OnMouseLeave() override;
    // Clicking into the box keeps its suggestion flyout open.
    bool KeepsPopupsOpenOnClick() const override;

  private:
    std::function<void(bool hovered)> onHoverChanged_;
};

// The Fluent AutoSuggestBox field: a 4px-rounded box with the sunken bottom edge, an accent
// underline while focused, the search glyph on the right and a clear button in front of it while
// there is text to clear.
class SearchField : public msimeui::Visual
{
  public:
    SearchField();

    void SetOnChanged(std::function<void(const std::wstring &)> handler);
    void SetOnSubmit(std::function<void()> handler);
    void SetOnFocusChanged(std::function<void(bool focused)> handler);

    std::wstring GetText() const;
    // Clears the text and reports it, the same as the clear button.
    void Clear();
    bool IsFocused() const;
    void Focus();

    // Where the clear button is, in DIPs. It only takes clicks while ClearVisible().
    RectF ClearRect() const;
    bool ClearVisible() const;

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    void Attach(msimeui::Window *window) override;
    msimeui::Visual *FindVisualAt(const PointF &point) override;
    msimeui::Visual *FindFocusableAt(const PointF &point) override;
    msimeui::Visual *FindFirstFocusableDescendant() override;
    bool OnMouseDown(const POINT &point, WPARAM) override;
    bool OnMouseUp(const POINT &point, WPARAM) override;
    bool OnMouseMove(const POINT &point, WPARAM) override;
    void OnMouseEnter() override;
    void OnMouseLeave() override;
    HCURSOR GetCursor() const override;
    bool KeepsPopupsOpenOnClick() const override;

  private:
    RectF GlyphRect() const;
    void RenderBottomEdge(msimeui::DeviceResources &dr, const D2D1_COLOR_F &color, float thickness);

    std::shared_ptr<HoverTextBox> box_;
    std::function<void(const std::wstring &)> onChanged_;
    std::function<void()> onSubmit_;
    std::function<void(bool focused)> onFocusChanged_;
    int themedLight_ = -1;
    bool boxHovered_ = false;
    bool selfHovered_ = false;
    bool clearHovered_ = false;
    bool clearPressed_ = false;
};
} // namespace native_settings
