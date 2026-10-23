#include "settings/native/native_page_help_widgets.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

namespace native_settings
{
namespace
{
bool Hit(msimeui::Window *window, const msimeui::Visual &visual, const POINT &point)
{
    return window && visual.HitTest(window->ClientPixelsToDips(point));
}
} // namespace

TextStyle DocStyle(float size, DWRITE_FONT_WEIGHT weight, float lineHeight, bool wrap)
{
    TextStyle style;
    style.size = size;
    style.weight = weight;
    style.lineHeight = lineHeight;
    style.wrap = wrap;
    return style;
}

TextStyle MonoStyle(float size, DWRITE_FONT_WEIGHT weight)
{
    TextStyle style;
    style.size = size;
    style.weight = weight;
    style.family = L"Consolas";
    return style;
}

std::shared_ptr<Label> MakeLabel(std::wstring text, TextStyle style, TextRole role)
{
    return std::make_shared<Label>(std::move(text), std::move(style), role);
}

VisualPtr MakeDocumentHero(const std::wstring &eyebrow, const std::wstring &title, const std::wstring &text)
{
    auto column = std::make_shared<Column>(0.0f);
    TextStyle eyebrowStyle = DocStyle(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    eyebrowStyle.letterSpacing = 11.0f * 0.08f;
    auto eyebrowLabel = MakeLabel(eyebrow, eyebrowStyle, TextRole::Accent);
    eyebrowLabel->SetMargin({0.0f, 0.0f, 0.0f, 7.0f});
    column->Add(eyebrowLabel);
    column->Add(MakeLabel(title, DocStyle(18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), TextRole::Primary));
    auto paragraph = MakeLabel(text, DocStyle(12.0f, DWRITE_FONT_WEIGHT_NORMAL, 1.7f, true), TextRole::Muted);
    paragraph->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
    column->Add(paragraph);
    return column;
}

// ---------------------------------------------------------------------------------
PaddedSection::PaddedSection(VisualPtr child, Thickness padding) : Section(std::move(child)), cssPadding_(padding)
{
}

SizeF PaddedSection::Measure(const SizeF &availableSize)
{
    const float border = CssBorder();
    padding_ = {cssPadding_.left + border, cssPadding_.top + border, cssPadding_.right + border,
                cssPadding_.bottom + border};
    return msimeui::Container::Measure(availableSize);
}

// ---------------------------------------------------------------------------------
CssButton::CssButton(std::wstring text, Style style)
    : text_(std::move(text)), style_(style), fill_(style.transitionMs), border_(style.transitionMs),
      text_color_(style.transitionMs)
{
}

void CssButton::SetText(std::wstring text)
{
    if (text == text_)
        return;
    text_ = std::move(text);
    InvalidateMeasure();
    InvalidateVisual();
}

void CssButton::SetOnClick(std::function<void()> handler)
{
    onClick_ = std::move(handler);
}

void CssButton::SetDisabled(bool disabled)
{
    if (disabled_ == disabled)
        return;
    disabled_ = disabled;
    if (disabled)
        pressed_ = false;
    InvalidateVisual();
}

SizeF CssButton::Measure(const SizeF &availableSize)
{
    const TextStyle style = DocStyle(style_.fontSize);
    const float border = CssBorder();
    const float width =
        std::max(style_.minWidth, MeasureTextWidth(text_, style) + style_.paddingX * 2.0f + border * 2.0f);
    const float height = LineHeight(style) + style_.paddingY * 2.0f + border * 2.0f;
    return {std::min(width, availableSize.width), height};
}

void CssButton::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void CssButton::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    const bool hot = hovered_ && !disabled_;
    D2D1_COLOR_F fill = p.buttonSecondaryBg;
    D2D1_COLOR_F border = style_.controlBorder ? p.controlBorder : p.border;
    D2D1_COLOR_F text = p.text;
    switch (style_.look)
    {
    case Look::Secondary:
        fill = hot ? p.buttonSecondaryHover : p.buttonSecondaryBg;
        if (hot && style_.transitionMs > 0.0 && !style_.controlBorder)
            border = p.borderStrong;
        text = style_.controlBorder ? p.buttonSecondaryText : p.text;
        break;
    case Look::Accent:
        fill = hot ? p.accent : p.accentStrong;
        border = D2D1::ColorF(0, 0.0f);
        text = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
        break;
    case Look::StatsPrimary:
        fill = hot ? p.accentStrong : p.accent;
        border = p.accent;
        text = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
        break;
    case Look::StatsDanger:
        fill = hot ? p.buttonSecondaryHover : p.buttonSecondaryBg;
        border = Rgba(255, 119, 119, hot ? 0.55f : 0.35f);
        text = hot ? p.dangerHover : p.dangerText;
        break;
    }
    fill_.SetTarget(fill, window_);
    border_.SetTarget(border, window_);
    text_color_.SetTarget(text, window_);

    if (disabled_)
        PushOpacity(dr, style_.disabledOpacity);
    RectF box = bounds_;
    if (style_.pressShift && pressed_ && hovered_)
        box.y += 1.0f;
    const float radius = std::min(style_.radius, box.height * 0.5f);
    FillRounded(dr, box, radius, fill_.Value(window_));
    StrokeRounded(dr, box, radius, border_.Value(window_), 1.0f);
    DrawTextInRect(dr, text_, DocStyle(style_.fontSize), box, text_color_.Value(window_), true);
    if (disabled_)
        PopOpacity(dr);
}

bool CssButton::IsFocusable() const
{
    return !disabled_;
}

bool CssButton::OnMouseDown(const POINT &point, WPARAM)
{
    pressed_ = !disabled_ && Hit(window_, *this, point);
    InvalidateVisual();
    return pressed_;
}

bool CssButton::OnMouseUp(const POINT &point, WPARAM)
{
    if (!pressed_)
        return false;
    pressed_ = false;
    InvalidateVisual();
    if (!disabled_ && Hit(window_, *this, point) && onClick_)
    {
        // A handler that closes the dialog destroys this button and onClick_ mid-call.
        const auto handler = onClick_;
        handler();
    }
    return true;
}

void CssButton::OnMouseEnter()
{
    hovered_ = true;
    InvalidateVisual();
}

void CssButton::OnMouseLeave()
{
    hovered_ = false;
    pressed_ = false;
    InvalidateVisual();
}

HCURSOR CssButton::GetCursor() const
{
    return LoadCursor(nullptr, disabled_ ? IDC_ARROW : IDC_HAND);
}

// ---------------------------------------------------------------------------------
RichParagraph::RichParagraph(std::vector<Run> runs, TextStyle style, std::function<D2D1_COLOR_F()> color,
                             std::function<void(const std::string &url)> onLink)
    : color_(std::move(color)), onLink_(std::move(onLink))
{
    std::wstring text;
    for (const Run &run : runs)
    {
        if (!run.url.empty())
            links_.push_back({static_cast<UINT32>(text.size()), static_cast<UINT32>(run.text.size()), run.url});
        text += run.text;
    }
    style.wrap = true;
    layout_.Set(std::move(text), std::move(style));
}

SizeF RichParagraph::Measure(const SizeF &availableSize)
{
    const SizeF size = layout_.Measure(availableSize.width);
    return {std::min(size.width, availableSize.width), size.height};
}

void RichParagraph::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void RichParagraph::Render(msimeui::DeviceResources &dr)
{
    IDWriteTextLayout *layout = layout_.Get(bounds_.width);
    if (!layout)
        return;
    const Palette &p = CurrentPalette();
    for (size_t i = 0; i < links_.size(); ++i)
    {
        const DWRITE_TEXT_RANGE range = {links_[i].start, links_[i].length};
        const bool hot = static_cast<int>(i) == hovered_;
        if (ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(hot ? p.accentStrong : p.accent))
            layout->SetDrawingEffect(brush, range);
        layout->SetUnderline(hot ? TRUE : FALSE, range);
    }
    layout_.Draw(dr, {bounds_.x, bounds_.y}, color_ ? color_() : p.textSecondary, bounds_.width);
    for (const Range &link : links_)
        layout->SetDrawingEffect(nullptr, {link.start, link.length});
}

int RichParagraph::LinkAt(const POINT &point)
{
    if (!window_)
        return -1;
    IDWriteTextLayout *layout = layout_.Get(bounds_.width);
    if (!layout)
        return -1;
    const PointF dip = window_->ClientPixelsToDips(point);
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestPoint(dip.x - bounds_.x, dip.y - bounds_.y, &trailing, &inside, &metrics)) || !inside)
        return -1;
    for (size_t i = 0; i < links_.size(); ++i)
    {
        if (metrics.textPosition >= links_[i].start && metrics.textPosition < links_[i].start + links_[i].length)
            return static_cast<int>(i);
    }
    return -1;
}

bool RichParagraph::OnMouseMove(const POINT &point, WPARAM)
{
    const int link = LinkAt(point);
    if (link != hovered_)
    {
        hovered_ = link;
        InvalidateVisual();
    }
    return true;
}

void RichParagraph::OnMouseLeave()
{
    if (hovered_ != -1)
    {
        hovered_ = -1;
        InvalidateVisual();
    }
}

bool RichParagraph::OnMouseDown(const POINT &point, WPARAM)
{
    pressed_ = LinkAt(point);
    return pressed_ >= 0;
}

bool RichParagraph::OnMouseUp(const POINT &point, WPARAM)
{
    const int link = LinkAt(point);
    const bool clicked = link >= 0 && link == pressed_;
    pressed_ = -1;
    if (clicked && onLink_)
    {
        const auto handler = onLink_;
        handler(links_[static_cast<size_t>(link)].url);
    }
    return true;
}

HCURSOR RichParagraph::GetCursor() const
{
    return LoadCursor(nullptr, hovered_ >= 0 ? IDC_HAND : IDC_ARROW);
}

// ---------------------------------------------------------------------------------
TitleNoteRow::TitleNoteRow(std::wstring title, std::wstring note)
{
    title_ = MakeTitle(std::move(title));
    TextStyle style = DocStyle(12.0f);
    style.wrap = true;
    note_ = MakeLabel(std::move(note), style, TextRole::Subtle);
    AddChild(title_);
    AddChild(note_);
}

SizeF TitleNoteRow::Measure(const SizeF &availableSize)
{
    titleSize_ = title_->MeasureInLayout(availableSize);
    const float room = std::max(availableSize.width - titleSize_.width - 10.0f, 0.0f);
    noteSize_ = note_->MeasureInLayout({room, availableSize.height});
    // A flex item that had to wrap shrinks to the room it got, not to its longest line.
    if (noteSize_.height > LineHeight(DocStyle(12.0f)) + 0.5f)
        noteSize_.width = room;
    return {availableSize.width, std::max(titleSize_.height, noteSize_.height)};
}

void TitleNoteRow::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    title_->ArrangeInLayout({finalRect.x, finalRect.y + (finalRect.height - titleSize_.height) * 0.5f, titleSize_.width,
                             titleSize_.height});
    note_->ArrangeInLayout({finalRect.x + finalRect.width - noteSize_.width,
                            finalRect.y + (finalRect.height - noteSize_.height) * 0.5f, noteSize_.width,
                            noteSize_.height});
}

void TitleNoteRow::Render(msimeui::DeviceResources &dr)
{
    title_->Render(dr);
    note_->Render(dr);
}

// ---------------------------------------------------------------------------------
StyledInput::StyledInput(std::wstring placeholder, float fontSize) : fontSize_(fontSize)
{
    box_ = std::make_shared<msimeui::TextBox>(30.0f, std::move(placeholder));
    box_->SetFontSize(fontSize);
    box_->SetPlaceholderFontSize(fontSize);
    box_->SetOnFocusChanged([this](bool focused) {
        focused_ = focused;
        appliedTheme_ = -1;
        if (!focused)
        {
            const std::wstring text = box_->GetText();
            if (text != committed_)
            {
                committed_ = text;
                if (onCommit_)
                {
                    const auto handler = onCommit_;
                    handler(text);
                }
            }
        }
        InvalidateVisual();
    });
    box_->SetOnSubmit([this]() {
        const std::wstring text = box_->GetText();
        if (text != committed_)
        {
            committed_ = text;
            if (onCommit_)
            {
                const auto handler = onCommit_;
                handler(text);
            }
        }
    });
    SetChild(box_);
}

void StyledInput::SetText(const std::wstring &text)
{
    if (focused_)
        return;
    committed_ = text;
    box_->SetText(text);
}

std::wstring StyledInput::GetText() const
{
    return box_->GetText();
}

bool StyledInput::IsFocused() const
{
    return focused_;
}

void StyledInput::SetDisabled(bool disabled)
{
    disabled_ = disabled;
    box_->SetEnabled(!disabled);
    InvalidateVisual();
}

void StyledInput::SetInvalid(bool invalid)
{
    if (invalid_ == invalid)
        return;
    invalid_ = invalid;
    appliedTheme_ = -1;
    InvalidateVisual();
}

void StyledInput::SetPlaceholder(std::wstring placeholder)
{
    box_->SetPlaceholderText(std::move(placeholder));
}

void StyledInput::SetOnCommit(std::function<void(const std::wstring &)> handler)
{
    onCommit_ = std::move(handler);
}

void StyledInput::ApplyAppearance()
{
    const Palette &p = CurrentPalette();
    appliedTheme_ = p.light ? 1 : 0;
    msimeui::TextBox::Appearance appearance;
    appearance.cornerRadius = 6.0f;
    appearance.borderWidth = CssBorder();
    appearance.paddingX = 12.0f;
    appearance.paddingY = 10.0f;
    appearance.fill = focused_ ? p.controlBg : p.surfaceSunken;
    appearance.border = invalid_ ? p.dangerText : p.controlBorder;
    appearance.focusBorder = p.accent;
    appearance.text = p.text;
    appearance.selection = WithAlpha(p.accent, 0.45f);
    appearance.placeholder = p.textSubtle;
    appearance.fontFamily = UiFontFamily();
    box_->SetAppearance(appearance);
}

SizeF StyledInput::Measure(const SizeF &availableSize)
{
    const float height = LineHeight(DocStyle(fontSize_)) + 20.0f + CssBorder() * 2.0f;
    box_->SetPreferredHeight(height);
    box_->MeasureInLayout({availableSize.width, height});
    return {availableSize.width, height};
}

void StyledInput::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    box_->ArrangeInLayout(finalRect);
}

void StyledInput::Render(msimeui::DeviceResources &dr)
{
    if (appliedTheme_ != (CurrentPalette().light ? 1 : 0))
        ApplyAppearance();
    if (disabled_)
        PushOpacity(dr, 0.45f);
    if (focused_)
    {
        // box-shadow: 0 0 0 2px color-mix(in srgb, var(--accent-color) 20%, transparent)
        FillRounded(dr, Inflate(bounds_, 2.0f), 8.0f, WithAlpha(CurrentPalette().accent, 0.2f));
        // The ring is outside the box only; restore the card under the translucent fill.
        FillRounded(dr, bounds_, 6.0f, Over(CurrentPalette().cardBg, CurrentPalette().chromeBg));
    }
    box_->Render(dr);
    if (disabled_)
        PopOpacity(dr);
}

// ---------------------------------------------------------------------------------
namespace
{
std::map<UINT_PTR, UiTimer *> &Timers()
{
    static std::map<UINT_PTR, UiTimer *> timers;
    return timers;
}
} // namespace

UiTimer::UiTimer(HWND hwnd, UINT intervalMs, std::function<void()> tick) : hwnd_(hwnd), tick_(std::move(tick))
{
    const UINT_PTR id = reinterpret_cast<UINT_PTR>(this);
    Timers()[id] = this;
    SetTimer(hwnd_, id, intervalMs, &UiTimer::Proc);
}

UiTimer::~UiTimer()
{
    const UINT_PTR id = reinterpret_cast<UINT_PTR>(this);
    KillTimer(hwnd_, id);
    Timers().erase(id);
}

void CALLBACK UiTimer::Proc(HWND, UINT, UINT_PTR id, DWORD)
{
    const auto it = Timers().find(id);
    if (it == Timers().end())
        return;
    // Copied: the tick may destroy the timer.
    const std::function<void()> tick = it->second->tick_;
    if (tick)
        tick();
}

namespace
{
struct BackgroundJob
{
    std::string result;
    std::function<void(const std::string &)> callback;
};

// Only touched on the window thread; the worker hands its job back by id.
std::map<WPARAM, std::shared_ptr<BackgroundJob>> &Jobs()
{
    static std::map<WPARAM, std::shared_ptr<BackgroundJob>> jobs;
    return jobs;
}
} // namespace

UINT BackgroundJobMessage()
{
    static const UINT message = RegisterWindowMessageW(L"MetasequoiaImeSettings.BackgroundJobDone");
    return message;
}

void CompleteBackgroundJob(WPARAM id)
{
    const auto it = Jobs().find(id);
    if (it == Jobs().end())
        return;
    const std::shared_ptr<BackgroundJob> job = it->second;
    Jobs().erase(it);
    if (job->callback)
        job->callback(job->result);
}

void RunInBackground(HWND hwnd, std::function<std::string()> work, std::function<void(const std::string &)> done)
{
    static WPARAM nextId = 0;
    const WPARAM id = ++nextId;
    auto job = std::make_shared<BackgroundJob>();
    job->callback = std::move(done);
    Jobs()[id] = job;
    // The result comes back as a posted message rather than through a polling timer:
    // Windows holds back WM_TIMER for a window that is covered or minimized, which left
    // the about page's update check hanging until the next input arrived.
    std::thread([hwnd, id, job = std::move(job), work = std::move(work)]() {
        std::string result;
        try
        {
            result = work();
        }
        catch (...)
        {
        }
        job->result = std::move(result);
        PostMessageW(hwnd, BackgroundJobMessage(), id, 0);
    }).detach();
}
} // namespace native_settings
