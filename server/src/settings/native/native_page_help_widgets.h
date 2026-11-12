#pragma once

#include "settings/native/native_settings_pages.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Widgets shared by the document-style pages (help, about, feedback) and the statistics
// page. They reproduce rules of styles/modules/document-settings.css and stats.css that
// the shared controls do not cover.
namespace native_settings
{
// A `.section` card with its own padding (`.document-hero`, `.about-links`, ...).
class PaddedSection : public Section
{
  public:
    PaddedSection(VisualPtr child, Thickness padding);
    SizeF Measure(const SizeF &availableSize) override;

  private:
    Thickness cssPadding_ = {};
};

// A <button> styled by one of the page rules: `.about-update-button`, `.feedback-action`,
// `.default-ime-button`, `.stats-btn` and friends.
class CssButton : public msimeui::Visual
{
  public:
    enum class Look
    {
        Secondary,    // --button-secondary-bg, --border-color
        Accent,       // .default-ime-primary / .about-dialog-primary: accent-strong -> accent
        StatsPrimary, // .stats-btn.primary: accent -> accent-strong
        StatsDanger,  // .stats-btn.danger
    };
    struct Style
    {
        Look look = Look::Secondary;
        float paddingX = 13.0f;
        float paddingY = 7.0f;
        float radius = 6.0f;
        float fontSize = 12.0f;
        float minWidth = 0.0f;
        // .stats-btn uses --control-border instead of --border-color.
        bool controlBorder = false;
        // transition on background / border (0 = none).
        double transitionMs = 0.0;
        // `:active { transform: translateY(1px) }`.
        bool pressShift = false;
        float disabledOpacity = 0.5f;
    };

    CssButton(std::wstring text, Style style);
    void SetText(std::wstring text);
    void SetOnClick(std::function<void()> handler);
    void SetDisabled(bool disabled);
    bool IsDisabled() const
    {
        return disabled_;
    }

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    bool IsFocusable() const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    void OnMouseEnter() override;
    void OnMouseLeave() override;
    HCURSOR GetCursor() const override;

  private:
    std::wstring text_;
    Style style_;
    bool hovered_ = false;
    bool pressed_ = false;
    bool disabled_ = false;
    ColorTransition fill_;
    ColorTransition border_;
    ColorTransition text_color_;
    std::function<void()> onClick_;
};

// A paragraph with inline links (`<a class="help-link">` inside a <p>).
class RichParagraph : public msimeui::Visual
{
  public:
    struct Run
    {
        std::wstring text;
        std::string url; // empty for plain text
    };

    RichParagraph(std::vector<Run> runs, TextStyle style, std::function<D2D1_COLOR_F()> color,
                  std::function<void(const std::string &url)> onLink);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    void OnMouseLeave() override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    int LinkAt(const POINT &point);

    struct Range
    {
        UINT32 start = 0;
        UINT32 length = 0;
        std::string url;
    };
    TextLayout layout_;
    std::vector<Range> links_;
    std::function<D2D1_COLOR_F()> color_;
    std::function<void(const std::string &)> onLink_;
    int hovered_ = -1;
    int pressed_ = -1;
};

// `.section-header` holding a title and a `.stats-note`: the title keeps its width, the
// note takes what is left (wrapping) and sits at the right end when it is short.
class TitleNoteRow : public msimeui::Panel
{
  public:
    TitleNoteRow(std::wstring title, std::wstring note);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;

  private:
    std::shared_ptr<Label> title_;
    std::shared_ptr<Label> note_;
    SizeF titleSize_ = {};
    SizeF noteSize_ = {};
};

// `.network-proxy-server input`: surface-sunken box, 6px radius, padding 10px 12px,
// accent border and a 2px accent ring while focused.
class StyledInput : public msimeui::Container
{
  public:
    StyledInput(std::wstring placeholder, float fontSize);

    void SetText(const std::wstring &text);
    std::wstring GetText() const;
    bool IsFocused() const;
    void SetDisabled(bool disabled);
    void SetInvalid(bool invalid);
    void SetPlaceholder(std::wstring placeholder);
    // `change`: focus left or Enter with an edited value.
    void SetOnCommit(std::function<void(const std::wstring &)> handler);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;

  private:
    void ApplyAppearance();

    std::shared_ptr<msimeui::TextBox> box_;
    float fontSize_ = 12.0f;
    bool disabled_ = false;
    bool invalid_ = false;
    bool focused_ = false;
    int appliedTheme_ = -1;
    std::wstring committed_;
    std::function<void(const std::wstring &)> onCommit_;
};

// Page root that owns the page's state, so the state lives exactly as long as the page.
// `onFirstAttach` runs once, when the page is actually shown: the indexer builds every page up
// front for the search index, so a builder must not do work the user has not asked for yet.
class OwnerBox : public msimeui::Container
{
  public:
    OwnerBox(VisualPtr child, std::shared_ptr<void> owned, std::function<void()> onFirstAttach = {})
        : msimeui::Container(std::move(child)), owned_(std::move(owned)), onFirstAttach_(std::move(onFirstAttach))
    {
    }

    void Attach(msimeui::Window *window) override
    {
        msimeui::Container::Attach(window);
        if (!onFirstAttach_)
            return;
        // Move the handler out first: it may show or hide things that attach again.
        const auto handler = std::move(onFirstAttach_);
        onFirstAttach_ = nullptr;
        handler();
    }

  private:
    std::shared_ptr<void> owned_;
    std::function<void()> onFirstAttach_;
};

// A thread-affine timer on the settings window that calls `tick` until destroyed.
class UiTimer
{
  public:
    UiTimer(HWND hwnd, UINT intervalMs, std::function<void()> tick);
    ~UiTimer();
    UiTimer(const UiTimer &) = delete;
    UiTimer &operator=(const UiTimer &) = delete;

  private:
    static void CALLBACK Proc(HWND hwnd, UINT message, UINT_PTR id, DWORD time);
    HWND hwnd_ = nullptr;
    std::function<void()> tick_;
};

// Runs `work` on a background thread and `done` back on the window thread. The result
// arrives as a posted BackgroundJobMessage(); the window passes it to
// CompleteBackgroundJob.
void RunInBackground(HWND hwnd, std::function<std::string()> work, std::function<void(const std::string &)> done);
UINT BackgroundJobMessage();
void CompleteBackgroundJob(WPARAM id);

// Text styles of the document pages.
TextStyle DocStyle(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL, float lineHeight = 0.0f,
                   bool wrap = false);
TextStyle MonoStyle(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);
// Label whose color follows the palette through `color`.
std::shared_ptr<Label> MakeLabel(std::wstring text, TextStyle style, TextRole role);
// `.document-hero` content: eyebrow, title and paragraph.
VisualPtr MakeDocumentHero(const std::wstring &eyebrow, const std::wstring &title, const std::wstring &text);
} // namespace native_settings
