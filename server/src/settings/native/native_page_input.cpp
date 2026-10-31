#include "settings/native/native_page_voice_fields.h"
#include "settings/native/native_settings_pages.h"

#include "config/ai_provider.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/Window.h"

#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <regex>

// partials/input.html + modules/input.ts (+ the input parts of config-sync.ts, shared.ts,
// credential-test.ts) and styles/modules/input.css.
namespace native_settings
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr float kUnbounded = 100000.0f;

bool HitSelf(msimeui::Window *window, const msimeui::Visual &visual, const POINT &point)
{
    return window && visual.HitTest(window->ClientPixelsToDips(point));
}

// ---------------------------------------------------------------------------------
// One-shot / repeating timers on the window thread (TIMERPROC, so the window's own
// WM_TIMER handling is not involved).
// ---------------------------------------------------------------------------------
class PageTimers
{
  public:
    static UINT_PTR Start(HWND hwnd, UINT ms, bool repeat, std::function<void()> callback)
    {
        const UINT_PTR id = next_++;
        entries_[id] = {hwnd, repeat, std::move(callback)};
        SetTimer(hwnd, id, ms, &PageTimers::Proc);
        return id;
    }

    static void Stop(UINT_PTR &id)
    {
        if (id == 0)
            return;
        auto it = entries_.find(id);
        if (it != entries_.end())
        {
            KillTimer(it->second.hwnd, id);
            entries_.erase(it);
        }
        id = 0;
    }

  private:
    struct Entry
    {
        HWND hwnd;
        bool repeat;
        std::function<void()> callback;
    };

    static void CALLBACK Proc(HWND hwnd, UINT, UINT_PTR id, DWORD)
    {
        auto it = entries_.find(id);
        if (it == entries_.end())
        {
            KillTimer(hwnd, id);
            return;
        }
        std::function<void()> callback = it->second.callback;
        if (!it->second.repeat)
        {
            KillTimer(hwnd, id);
            entries_.erase(it);
        }
        callback();
    }

    static inline UINT_PTR next_ = 0x7100;
    static inline std::map<UINT_PTR, Entry> entries_;
};

// ---------------------------------------------------------------------------------
// Inline text with differently styled runs: `<code>` in descriptions, the model name
// with its meta span and the 浏览器下载 link.
// ---------------------------------------------------------------------------------
struct Span
{
    std::wstring text;
    float size = kFontCaption;
    std::function<D2D1_COLOR_F()> color;
    std::wstring family;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    std::function<void()> onClick;
    float marginLeft = 0.0f;
};

class RichText : public msimeui::Visual
{
  public:
    RichText(float baseSize, float lineHeight, DWRITE_WORD_WRAPPING wrapping)
        : baseSize_(baseSize), lineHeight_(lineHeight), wrapping_(wrapping)
    {
    }

    void SetSpans(std::vector<Span> spans)
    {
        spans_ = std::move(spans);
        layout_.Reset();
        InvalidateMeasure();
        InvalidateVisual();
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        IDWriteTextLayout *layout = Layout(availableSize.width);
        if (!layout)
            return {};
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        return {std::min(std::ceil(metrics.widthIncludingTrailingWhitespace), availableSize.width), metrics.height};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        IDWriteTextLayout *layout = Layout(bounds_.width);
        if (!target || !layout)
            return;
        UINT32 start = 0;
        for (size_t i = 0; i < spans_.size(); ++i)
        {
            const UINT32 length = static_cast<UINT32>(Text(i).size());
            const D2D1_COLOR_F color = spans_[i].color ? spans_[i].color() : CurrentPalette().text;
            if (ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(color))
                layout->SetDrawingEffect(brush, {start, length});
            layout->SetUnderline(static_cast<int>(i) == hovered_ && spans_[i].onClick, {start, length});
            start += length;
        }
        if (ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(CurrentPalette().text))
            target->DrawTextLayout(D2D1::Point2F(bounds_.x, bounds_.y), layout, brush,
                                   D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }

    bool OnMouseMove(const POINT &point, WPARAM) override
    {
        const int span = SpanAt(point);
        if (span != hovered_)
        {
            hovered_ = span;
            InvalidateVisual();
        }
        return false;
    }

    void OnMouseLeave() override
    {
        if (hovered_ >= 0)
        {
            hovered_ = -1;
            InvalidateVisual();
        }
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = SpanAt(point);
        return pressed_ >= 0 && spans_[pressed_].onClick;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        const int span = SpanAt(point);
        const bool clicked = span >= 0 && span == pressed_ && spans_[span].onClick;
        pressed_ = -1;
        if (clicked)
            spans_[span].onClick();
        return clicked;
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, hovered_ >= 0 && spans_[hovered_].onClick ? IDC_HAND : IDC_ARROW);
    }

  private:
    // A margin-left becomes leading spaces' worth of advance via a narrow no-break space run.
    std::wstring Text(size_t i) const
    {
        return spans_[i].text;
    }

    int SpanAt(const POINT &point)
    {
        if (!window_ || !layout_)
            return -1;
        const PointF dip = window_->ClientPixelsToDips(point);
        BOOL trailing = FALSE;
        BOOL inside = FALSE;
        DWRITE_HIT_TEST_METRICS metrics{};
        layout_->HitTestPoint(dip.x - bounds_.x, dip.y - bounds_.y, &trailing, &inside, &metrics);
        if (!inside)
            return -1;
        UINT32 start = 0;
        for (size_t i = 0; i < spans_.size(); ++i)
        {
            const UINT32 length = static_cast<UINT32>(Text(i).size());
            if (metrics.textPosition >= start && metrics.textPosition < start + length)
                return static_cast<int>(i);
            start += length;
        }
        return -1;
    }

    IDWriteTextLayout *Layout(float maxWidth)
    {
        const float width = wrapping_ == DWRITE_WORD_WRAPPING_NO_WRAP ? kUnbounded : std::max(maxWidth, 1.0f);
        if (layout_ && width_ == width)
            return layout_.Get();
        layout_.Reset();
        width_ = width;
        IDWriteFactory *factory = DWriteFactory();
        if (!factory)
            return nullptr;
        std::wstring text;
        for (size_t i = 0; i < spans_.size(); ++i)
            text += Text(i);
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(factory->CreateTextFormat(UiFontFamily().c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, baseSize_, L"zh-cn",
                                             format.GetAddressOf())))
            return nullptr;
        msimeui::ApplyUiFontFallback(factory, format.Get());
        if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), width,
                                             kUnbounded, layout_.GetAddressOf())))
            return nullptr;
        layout_->SetWordWrapping(wrapping_);
        UINT32 start = 0;
        for (size_t i = 0; i < spans_.size(); ++i)
        {
            const DWRITE_TEXT_RANGE range = {start, static_cast<UINT32>(Text(i).size())};
            layout_->SetFontSize(spans_[i].size, range);
            layout_->SetFontWeight(spans_[i].weight, range);
            if (!spans_[i].family.empty())
                layout_->SetFontFamilyName(spans_[i].family.c_str(), range);
            start += range.length;
        }
        if (lineHeight_ > 0.0f)
        {
            DWRITE_LINE_METRICS line{};
            UINT32 count = 0;
            layout_->GetLineMetrics(&line, 1, &count);
            if (count > 0)
            {
                const float spacing = baseSize_ * lineHeight_;
                layout_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, spacing,
                                        line.baseline + (spacing - line.height) * 0.5f);
            }
        }
        return layout_.Get();
    }

    float baseSize_ = kFontCaption;
    float lineHeight_ = 0.0f;
    DWRITE_WORD_WRAPPING wrapping_ = DWRITE_WORD_WRAPPING_WRAP;
    std::vector<Span> spans_;
    float width_ = -1.0f;
    ComPtr<IDWriteTextLayout> layout_;
    int hovered_ = -1;
    int pressed_ = -1;
};

std::function<D2D1_COLOR_F()> Muted()
{
    return [] { return CurrentPalette().textMuted; };
}

// `<code>` inside a 12px description: the default monospace font for zh-CN pages
// (NSimSun) at the inherited 12px.
Span Code(std::wstring text)
{
    Span span{std::move(text), kFontCaption, Muted(), L"NSimSun"};
    return span;
}

Span Plain(std::wstring text)
{
    return Span{std::move(text), kFontCaption, Muted()};
}

// ---------------------------------------------------------------------------------
// Small layout helpers
// ---------------------------------------------------------------------------------

// `.section-item-row`: padding 12px 0 with a 1px divider on top.
class ItemRow : public msimeui::Container
{
  public:
    ItemRow(VisualPtr child, bool last) : msimeui::Container(std::move(child)), last_(last)
    {
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        padding_ = {0.0f, 12.0f + CssBorder(), 0.0f, last_ ? 0.0f : 12.0f};
        return msimeui::Container::Measure(availableSize);
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        FillRect(dr, {bounds_.x, bounds_.y, bounds_.width, CssBorder()}, CurrentPalette().divider);
        msimeui::Container::Render(dr);
    }

  private:
    bool last_ = false;
};

// border-top separated block (`.word-to-character-keys-row`: padding-top 16px).
class TopRuled : public msimeui::Container
{
  public:
    TopRuled(VisualPtr child, float paddingTop) : msimeui::Container(std::move(child)), paddingTop_(paddingTop)
    {
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        padding_ = {0.0f, paddingTop_ + CssBorder(), 0.0f, 0.0f};
        return msimeui::Container::Measure(availableSize);
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        FillRect(dr, {bounds_.x, bounds_.y, bounds_.width, CssBorder()}, CurrentPalette().divider);
        msimeui::Container::Render(dr);
    }

  private:
    float paddingTop_ = 0.0f;
};

// Plain padded box.
VisualPtr Padded(VisualPtr child, Thickness padding)
{
    auto box = std::make_shared<msimeui::Container>(std::move(child));
    box->SetPadding(padding);
    return box;
}

// `opacity` without blocking input.
class OpacityBox : public msimeui::Container
{
  public:
    explicit OpacityBox(VisualPtr child) : msimeui::Container(std::move(child))
    {
    }

    void SetDimmed(bool dimmed)
    {
        if (dimmed_ == dimmed)
            return;
        dimmed_ = dimmed;
        InvalidateVisual();
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        if (!dimmed_)
        {
            msimeui::Container::Render(dr);
            return;
        }
        PushOpacity(dr, 0.45f);
        msimeui::Container::Render(dr);
        PopOpacity(dr);
    }

  private:
    bool dimmed_ = false;
};

// `.input-mode-options`: grid 1fr auto 1fr, gap 12px (the divider in the auto track is empty).
class TwoColumns : public msimeui::Panel
{
  public:
    TwoColumns(VisualPtr left, VisualPtr right)
    {
        AddChild(std::move(left));
        AddChild(std::move(right));
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        const float column = std::max((availableSize.width - 24.0f) * 0.5f, 0.0f);
        float height = 0.0f;
        for (const auto &child : children_)
            height = std::max(height, child->MeasureInLayout({column, availableSize.height}).height);
        height_ = height;
        return {availableSize.width, height};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        const float column = std::max((finalRect.width - 24.0f) * 0.5f, 0.0f);
        for (size_t i = 0; i < children_.size(); ++i)
        {
            const SizeF size = children_[i]->MeasureInLayout({column, finalRect.height});
            children_[i]->ArrangeInLayout({finalRect.x + static_cast<float>(i) * (column + 24.0f),
                                           finalRect.y + (finalRect.height - size.height) * 0.5f, size.width,
                                           size.height});
        }
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        for (const auto &child : children_)
            child->Render(dr);
    }

  private:
    float height_ = 0.0f;
};

// Title (+ description) where the title is a sub item (`.section-item-row .section-title`).
VisualPtr SubBlock(std::wstring title, std::wstring description, bool descriptionMargin = true)
{
    auto column = std::make_shared<Column>(0.0f);
    column->Add(MakeSubTitle(std::move(title), description));
    if (!description.empty())
    {
        auto desc = MakeDescription(std::move(description));
        if (!descriptionMargin)
            desc->SetMargin({0.0f, 0.0f, 0.0f, 0.0f});
        column->Add(desc);
    }
    return column;
}

std::shared_ptr<Label> Description(std::wstring text, float marginTop = 4.0f)
{
    auto label = MakeDescription(std::move(text));
    label->SetMargin({0.0f, marginTop, 0.0f, 0.0f});
    return label;
}

// `.helpcode-custom-button`: transparent, muted 12px text, 30px tall, 6px radius.
class GhostButton : public msimeui::Visual
{
  public:
    explicit GhostButton(std::wstring text) : text_(std::move(text))
    {
        SetMargin({0.0f, 2.0f, 0.0f, 0.0f});
    }

    void SetOnClick(std::function<void()> handler)
    {
        onClick_ = std::move(handler);
    }

    SizeF Measure(const SizeF &) override
    {
        return {MeasureTextWidth(text_, Style()) + 20.0f + CssBorder() * 2.0f, 30.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        if (hovered_)
            FillRounded(dr, bounds_, 6.0f, p.buttonSecondaryBg);
        StrokeRounded(dr, bounds_, 6.0f, p.border, 1.0f);
        DrawTextInRect(dr, text_, Style(), bounds_, hovered_ ? p.text : p.textMuted, true);
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = HitSelf(window_, *this, point);
        return pressed_;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (!pressed_)
            return false;
        pressed_ = false;
        if (HitSelf(window_, *this, point) && onClick_)
        {
            // A handler may tear down the visual that owns onClick_ while it runs.
            const auto handler = onClick_;
            handler();
        }
        return true;
    }

    void OnMouseEnter() override
    {
        hovered_ = true;
        InvalidateVisual();
    }

    void OnMouseLeave() override
    {
        hovered_ = false;
        pressed_ = false;
        InvalidateVisual();
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, IDC_HAND);
    }

  private:
    static TextStyle Style()
    {
        TextStyle style;
        style.size = kFontCaption;
        return style;
    }

    std::wstring text_;
    bool hovered_ = false;
    bool pressed_ = false;
    std::function<void()> onClick_;
};

// `.credential-test-button`: 12px system font, line-height 1.4, padding 4px 10px, 5px radius.
class TestButton : public msimeui::Visual
{
  public:
    explicit TestButton(std::wstring text) : idle_(text), text_(std::move(text))
    {
    }

    void SetOnClick(std::function<void()> handler)
    {
        onClick_ = std::move(handler);
    }

    void SetBusy(bool busy)
    {
        busy_ = busy;
        text_ = busy ? L"测试中…" : idle_;
        InvalidateMeasure();
        InvalidateVisual();
    }

    SizeF Measure(const SizeF &) override
    {
        const float width = std::max(64.0f, MeasureTextWidth(text_, Style()) + 20.0f + CssBorder() * 2.0f);
        return {width, kFontCaption * 1.4f + 8.0f + CssBorder() * 2.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        if (busy_)
            PushOpacity(dr, 0.65f);
        FillRounded(dr, bounds_, 5.0f, hovered_ && !busy_ ? p.buttonSecondaryHover : p.buttonSecondaryBg);
        StrokeRounded(dr, bounds_, 5.0f, p.controlBorder, 1.0f);
        DrawTextInRect(dr, text_, Style(), bounds_, p.buttonSecondaryText, true);
        if (busy_)
            PopOpacity(dr);
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = !busy_ && HitSelf(window_, *this, point);
        return pressed_;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (!pressed_)
            return false;
        pressed_ = false;
        if (HitSelf(window_, *this, point) && onClick_)
        {
            // A handler may tear down the visual that owns onClick_ while it runs.
            const auto handler = onClick_;
            handler();
        }
        return true;
    }

    void OnMouseEnter() override
    {
        hovered_ = true;
        InvalidateVisual();
    }

    void OnMouseLeave() override
    {
        hovered_ = false;
        pressed_ = false;
        InvalidateVisual();
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, busy_ ? IDC_WAIT : IDC_HAND);
    }

  private:
    static TextStyle Style()
    {
        TextStyle style = ControlStyle();
        style.size = kFontCaption;
        style.lineHeight = 1.4f;
        return style;
    }

    std::wstring idle_;
    std::wstring text_;
    bool busy_ = false;
    bool hovered_ = false;
    bool pressed_ = false;
    std::function<void()> onClick_;
};

// A label-less `<input type="radio">` in the model rows. Unlike `.radio-option` the page
// does not restyle it, so it is Chrome's own 13px radio (margin 3px 3px 0 5px) in the
// page's color scheme.
class BareRadio : public msimeui::Visual
{
  public:
    void SetOnSelected(std::function<void()> handler)
    {
        onSelected_ = std::move(handler);
    }

    void SetState(bool checked, bool disabled)
    {
        if (checked_ == checked && disabled_ == disabled)
            return;
        checked_ = checked;
        disabled_ = disabled;
        InvalidateVisual();
    }

    SizeF Measure(const SizeF &) override
    {
        return {5.0f + 13.0f + 3.0f, 3.0f + 13.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const bool light = CurrentPalette().light;
        // Chrome's form-control palette: accent #0075ff (light) / #99c8ff (dark).
        const D2D1_COLOR_F accent = light ? Hex(0x0075FF) : Hex(0x99C8FF);
        const D2D1_COLOR_F ring = light ? Hex(0x767676) : Hex(0x858585);
        const D2D1_COLOR_F fill = light ? Hex(0xFFFFFF) : Hex(0x3B3B3B);
        if (disabled_)
            PushOpacity(dr, 0.5f);
        const RectF box = {bounds_.x + 5.0f, bounds_.y + (bounds_.height - 16.0f) * 0.5f + 3.0f, 13.0f, 13.0f};
        FillRounded(dr, box, 6.5f, checked_ ? Over(WithAlpha(fill, 1.0f), CurrentPalette().chromeBg) : fill);
        StrokeRounded(dr, box, 6.5f, checked_ ? accent : ring, checked_ ? 1.5f : 1.0f);
        if (checked_)
            FillRounded(dr, {box.x + 3.5f, box.y + 3.5f, 6.0f, 6.0f}, 3.0f, accent);
        if (disabled_)
            PopOpacity(dr);
    }
    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = !disabled_ && HitSelf(window_, *this, point);
        return pressed_;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (!pressed_)
            return false;
        pressed_ = false;
        if (HitSelf(window_, *this, point) && !checked_ && onSelected_)
        {
            const auto handler = onSelected_;
            handler();
        }
        return true;
    }

  private:
    bool checked_ = false;
    bool disabled_ = false;
    bool pressed_ = false;
    std::function<void()> onSelected_;
};

// ---------------------------------------------------------------------------------
// `.translation-api-field`: label over a Consolas input on --surface-sunken, with the
// eye toggle for secrets.
// ---------------------------------------------------------------------------------
const char *kEyeShow =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" )"
    R"(stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">)"
    R"(<path d="M2.5 12s3.5-6 9.5-6 9.5 6 9.5 6-3.5 6-9.5 6-9.5-6-9.5-6Z"/><circle cx="12" cy="12" r="2.6"/></svg>)";
const char *kEyeHide =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" )"
    R"(stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">)"
    R"(<path d="m3 3 18 18"/><path d="M10.6 6.1A9.6 9.6 0 0 1 12 6c6 0 9.5 6 9.5 6a16 16 0 0 1-2.2 3"/>)"
    R"(<path d="M6.2 6.3C3.8 8.1 2.5 12 2.5 12s3.5 6 9.5 6a9 9 0 0 0 3-.5"/></svg>)";

class ApiField : public msimeui::Panel
{
  public:
    ApiField(std::wstring label, std::wstring placeholder, bool password, bool readOnly)
        : password_(password), readOnly_(readOnly), placeholder_(placeholder)
    {
        TextStyle labelStyle;
        labelStyle.size = kFontCaption;
        labelStyle.weight = DWRITE_FONT_WEIGHT_MEDIUM;
        label_ = std::make_shared<Label>(std::move(label), labelStyle, TextRole::Secondary);
        AddChild(label_);
        if (!readOnly_)
        {
            box_ = std::make_shared<msimeui::TextBox>(InputHeight(), placeholder);
            box_->SetChromeVisible(false);
            box_->SetPasswordMode(password_);
            box_->SetFontSize(13.0f);
            box_->SetPlaceholderFontSize(13.0f);
            box_->SetOnFocusChanged([this](bool focused) {
                if (!focused)
                    Commit();
                InvalidateVisual();
            });
            box_->SetOnSubmit([this]() { Commit(); });
            AddChild(box_);
        }
    }

    void SetText(const std::wstring &text)
    {
        if (box_ && box_->IsFocused())
            return;
        value_ = text;
        committed_ = text;
        if (box_)
            box_->SetText(text);
        InvalidateVisual();
    }

    // What the input holds: the live text while editing, else the last set/committed value
    // (the box only takes text once it is attached to the window).
    std::wstring GetText() const
    {
        return box_ && box_->IsFocused() ? box_->GetText() : value_;
    }

    void SetOnCommit(std::function<void(const std::wstring &)> handler)
    {
        onCommit_ = std::move(handler);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        ApplyAppearance();
        const SizeF label = label_->MeasureInLayout(availableSize);
        labelHeight_ = label.height;
        if (box_)
            box_->MeasureInLayout({InputWidth(availableSize.width), InputHeight()});
        return {availableSize.width, labelHeight_ + 7.0f + InputHeight()};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        label_->ArrangeInLayout({finalRect.x, finalRect.y, finalRect.width, labelHeight_});
        if (box_)
            box_->ArrangeInLayout({finalRect.x, InputRect().y, InputWidth(finalRect.width), InputHeight()});
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        if (appliedLight_ != CurrentPalette().light)
            ApplyAppearance();
        const Palette &p = CurrentPalette();
        label_->Render(dr);
        const RectF input = InputRect();
        const bool focused = box_ && box_->IsFocused();
        if (focused)
        {
            // box-shadow: 0 0 0 2px color-mix(accent 20%)
            FillRounded(dr, Inflate(input, 2.0f), 8.0f, WithAlpha(p.accent, 0.2f));
        }
        FillRounded(dr, input, 6.0f, Over(focused ? p.controlBg : p.surfaceSunken, Over(p.cardBg, p.chromeBg)));
        StrokeRounded(dr, input, 6.0f, focused ? p.accent : p.controlBorder, 1.0f);
        if (box_)
        {
            box_->Render(dr);
        }
        else
        {
            const RectF text = {input.x + 12.0f + CssBorder(), input.y, input.width - 24.0f, input.height};
            if (value_.empty())
                DrawTextInRect(dr, placeholder_, InputStyle(), text, p.textSubtle);
            else
                DrawTextInRect(dr, value_, InputStyle(), text, p.text);
        }
        if (password_)
        {
            const RectF eye = EyeRect();
            if (eyeHot_)
                FillRounded(dr, eye, 5.0f, WithAlpha(p.text, 0.08f));
            const D2D1_COLOR_F color = eyeHot_ ? p.text : p.textMuted;
            const std::string markup = TintSvg(revealed_ ? kEyeHide : kEyeShow, color);
            DrawSvg(dr, markup, {eye.x + 7.0f, eye.y + 7.0f, 18.0f, 18.0f});
        }
    }

    msimeui::Visual *FindVisualAt(const PointF &point) override
    {
        if (!HitTest(point))
            return nullptr;
        if (password_ && Contains(EyeRect(), point))
            return this;
        if (box_ && box_->HitTest(point))
            return box_.get();
        return this;
    }

    msimeui::Visual *FindFocusableAt(const PointF &point) override
    {
        if (password_ && Contains(EyeRect(), point))
            return nullptr;
        return box_ && box_->HitTest(point) ? box_.get() : nullptr;
    }

    bool OnMouseMove(const POINT &point, WPARAM) override
    {
        const bool hot = password_ && window_ && Contains(EyeRect(), window_->ClientPixelsToDips(point));
        if (hot != eyeHot_)
        {
            eyeHot_ = hot;
            InvalidateVisual();
        }
        return true;
    }

    void OnMouseLeave() override
    {
        eyeHot_ = false;
        InvalidateVisual();
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        return password_ && window_ && Contains(EyeRect(), window_->ClientPixelsToDips(point));
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (password_ && window_ && Contains(EyeRect(), window_->ClientPixelsToDips(point)))
        {
            revealed_ = !revealed_;
            box_->SetPasswordMode(!revealed_);
            InvalidateVisual();
            return true;
        }
        return false;
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, eyeHot_ ? IDC_HAND : IDC_ARROW);
    }

  private:
    static TextStyle InputStyle()
    {
        TextStyle style;
        style.size = 13.0f;
        style.family = L"Consolas";
        return style;
    }

    static float InputHeight()
    {
        // padding 10px 12px, 13px Consolas, 1px border
        return LineHeight(InputStyle()) + 20.0f + CssBorder() * 2.0f;
    }

    // The secret input reserves 42px on the right for the eye button.
    float InputWidth(float width) const
    {
        return std::max(width - (password_ ? 30.0f : 0.0f), 0.0f);
    }

    RectF InputRect() const
    {
        return {bounds_.x, bounds_.y + labelHeight_ + 7.0f, bounds_.width, InputHeight()};
    }

    RectF EyeRect() const
    {
        const RectF input = InputRect();
        return {input.x + input.width - 5.0f - 32.0f, input.y + (input.height - 32.0f) * 0.5f, 32.0f, 32.0f};
    }

    void ApplyAppearance()
    {
        if (!box_)
            return;
        const Palette &p = CurrentPalette();
        appliedLight_ = p.light;
        msimeui::TextBox::Appearance appearance;
        appearance.cornerRadius = 6.0f;
        appearance.borderWidth = CssBorder();
        appearance.paddingX = 12.0f;
        appearance.paddingY = 10.0f;
        appearance.text = p.text;
        appearance.selection = WithAlpha(p.accent, 0.45f);
        appearance.placeholder = p.textSubtle;
        appearance.fontFamily = L"Consolas";
        box_->SetAppearance(appearance);
    }

    void Commit()
    {
        std::wstring text = box_->GetText();
        const auto first = text.find_first_not_of(L" \t\r\n");
        const auto last = text.find_last_not_of(L" \t\r\n");
        text = first == std::wstring::npos ? std::wstring() : text.substr(first, last - first + 1);
        if (text != box_->GetText())
            box_->SetText(text);
        value_ = text;
        if (text == committed_)
            return;
        committed_ = text;
        if (onCommit_)
        {
            const auto handler = onCommit_;
            handler(text);
        }
    }

    bool password_ = false;
    bool readOnly_ = false;
    bool revealed_ = false;
    bool eyeHot_ = false;
    bool appliedLight_ = false;
    std::wstring placeholder_;
    std::wstring value_;
    std::wstring committed_;
    float labelHeight_ = 0.0f;
    std::shared_ptr<Label> label_;
    std::shared_ptr<msimeui::TextBox> box_;
    std::function<void(const std::wstring &)> onCommit_;
};

// `.translation-api-grid`: two columns, gap 16px; wide fields span both.
class FieldGrid : public msimeui::Panel
{
  public:
    void Add(VisualPtr field, bool wide)
    {
        wide_.push_back(wide);
        AddChild(std::move(field));
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        Place(availableSize.width, false, 0.0f, 0.0f);
        return {availableSize.width, height_};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        Place(finalRect.width, true, finalRect.x, finalRect.y);
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        for (const auto &child : children_)
            child->Render(dr);
    }

  private:
    void Place(float width, bool arrange, float x0, float y0)
    {
        const float column = std::max((width - 16.0f) * 0.5f, 0.0f);
        float y = 0.0f;
        size_t i = 0;
        height_ = 0.0f;
        while (i < children_.size())
        {
            if (wide_[i])
            {
                const SizeF size = children_[i]->MeasureInLayout({width, kUnbounded});
                if (arrange)
                    children_[i]->ArrangeInLayout({x0, y0 + y, width, size.height});
                y += size.height + 16.0f;
                ++i;
                continue;
            }
            float rowHeight = 0.0f;
            size_t count = 0;
            while (i + count < children_.size() && !wide_[i + count] && count < 2)
            {
                rowHeight = std::max(rowHeight, children_[i + count]->MeasureInLayout({column, kUnbounded}).height);
                ++count;
            }
            for (size_t k = 0; k < count && arrange; ++k)
                children_[i + k]->ArrangeInLayout(
                    {x0 + static_cast<float>(k) * (column + 16.0f), y0 + y, column, rowHeight});
            y += rowHeight + 16.0f;
            i += count;
        }
        height_ = std::max(y - 16.0f, 0.0f);
    }

    std::vector<bool> wide_;
    float height_ = 0.0f;
};

// ---------------------------------------------------------------------------------
// Credential test (credential-test.ts)
// ---------------------------------------------------------------------------------
using ConfigMap = std::map<std::string, std::string>;

class CredentialTest
{
  public:
    CredentialTest(PageContext &context, std::function<std::string()> service, std::function<ConfigMap()> config,
                   std::function<ConfigMap()> snapshot)
        : context_(context), service_(std::move(service)), config_(std::move(config)), snapshot_(std::move(snapshot))
    {
        button_ = std::make_shared<TestButton>(L"测试配置");
        TextStyle style = CaptionStyle();
        style.lineHeight = 1.45f;
        style.wrap = true;
        status_ = std::make_shared<Label>(L"", style, TextRole::Muted);
        statusBox_ = std::make_shared<Hideable>(status_, false);
        auto row = std::make_shared<Row>(12.0f);
        row->Add(button_);
        row->Add(statusBox_, 1.0f);
        row->SetMargin({0.0f, 14.0f, 0.0f, 0.0f});
        row_ = row;
        button_->SetOnClick([this]() { Start(); });
        context.OnMessage("apiCredentialTestResult", [this](const Json &message) { OnResult(message); });
    }

    VisualPtr View() const
    {
        return row_;
    }

  private:
    void SetStatus(const std::wstring &text, int kind)
    {
        status_->SetText(text);
        if (kind > 0)
            status_->SetColor(Hex(0x4CAF72));
        else if (kind < 0)
            status_->SetColor(Hex(0xD96767));
        else
            status_->SetRole(TextRole::Muted);
        statusBox_->SetVisible(!text.empty());
    }

    void Finish()
    {
        PageTimers::Stop(timer_);
        requestId_.clear();
        button_->SetBusy(false);
    }

    void Start()
    {
        if (!requestId_.empty())
            return;
        requestId_ = context_.model.NewRequestId("credential");
        pendingService_ = service_();
        pendingSnapshot_ = snapshot_();
        Json config = Json::object();
        for (const auto &[key, value] : config_())
            config[key] = value;
        const std::string id = requestId_;
        timer_ = PageTimers::Start(context_.host.Hwnd(), 30000, false, [this, id]() {
            if (requestId_ != id)
                return;
            timer_ = 0;
            Finish();
            SetStatus(L"测试超时，请重试", -1);
        });
        button_->SetBusy(true);
        SetStatus(L"正在连接服务，请稍候…", 0);
        context_.model.Post("apiCredentialTest",
                            Json{{"requestId", requestId_}, {"service", pendingService_}, {"config", config}});
    }

    void OnResult(const Json &message)
    {
        if (requestId_.empty() || message.value("requestId", std::string()) != requestId_)
            return;
        const bool matches = pendingService_ == service_() && pendingSnapshot_ == snapshot_();
        Finish();
        if (!matches)
        {
            SetStatus(L"配置已变更，请重新测试", 0);
            return;
        }
        SetStatus(Widen(message.value("message", std::string())), message.value("ok", false) ? 1 : -1);
    }

    PageContext &context_;
    std::function<std::string()> service_;
    std::function<ConfigMap()> config_;
    std::function<ConfigMap()> snapshot_;
    std::shared_ptr<TestButton> button_;
    std::shared_ptr<Label> status_;
    std::shared_ptr<Hideable> statusBox_;
    VisualPtr row_;
    std::string requestId_;
    std::string pendingService_;
    ConfigMap pendingSnapshot_;
    UINT_PTR timer_ = 0;
};

// ---------------------------------------------------------------------------------
// Model rows (shared.ts: seedCollocationRows / applyCollocationModelStatus /
// applyNeuralModelStatus)
// ---------------------------------------------------------------------------------
bool IsBusy(const std::string &state)
{
    return state == "downloading" || state == "importing";
}

bool UpdatePending(const Json *status)
{
    return status && status->is_object() && status->value("updatePending", false);
}

bool UpdateAvailable(const Json *status)
{
    return status && status->is_object() && status->value("updateAvailable", false);
}

std::wstring StatusText(const Json *status)
{
    if (UpdatePending(status))
        return L"新版本已下载，点「重启输入法」生效";
    const std::string state = status && status->is_object() ? status->value("state", std::string("absent")) : "absent";
    if (state == "ready")
        return UpdateAvailable(status) ? L"有新版本可用" : L"模型已就绪";
    if (state == "downloading")
    {
        const int progress =
            status->contains("progress") && (*status)["progress"].is_number() ? (*status)["progress"].get<int>() : 0;
        return L"下载中 " + std::to_wstring(progress) + L"%";
    }
    if (state == "importing")
        return L"导入并校验中…";
    if (state == "error")
    {
        std::string error = status->value("error", std::string());
        return L"下载失败：" + (error.empty() ? std::wstring(L"未知原因") : Widen(error));
    }
    return L"未下载";
}

std::string StateOf(const Json *status)
{
    return status && status->is_object() ? status->value("state", std::string("absent")) : "absent";
}

struct ModelRow
{
    std::string id;
    std::string url;
    std::shared_ptr<RichText> name;
    std::shared_ptr<Label> status;
    std::shared_ptr<BareRadio> radio;
    std::shared_ptr<Button> download;
    std::function<void()> downloadAction;
    std::function<void()> restartAction;
    std::shared_ptr<Hideable> importBox;
    std::shared_ptr<Button> import;
    std::shared_ptr<Hideable> removeBox;
    VisualPtr view;
};

std::vector<Span> NameSpans(const std::wstring &name, const std::wstring &meta, bool showLink,
                            std::function<void()> onLink)
{
    std::vector<Span> spans;
    spans.push_back({name, 13.0f, [] { return CurrentPalette().text; }});
    if (!meta.empty())
        spans.push_back({L" " + meta, kFontCaption, Muted()});
    if (showLink)
        spans.push_back({L" 浏览器下载",
                         kFontCaption,
                         [] { return CurrentPalette().accentStrong; },
                         {},
                         DWRITE_FONT_WEIGHT_NORMAL,
                         std::move(onLink)});
    return spans;
}

ModelRow MakeModelRow(PageContext &context, const std::string &id, const std::wstring &name, const std::wstring &meta,
                      const std::string &url, bool withRadio, const char *downloadType, const char *importType,
                      const char *deleteType)
{
    ModelRow row;
    row.id = id;
    row.url = url;
    SettingsModel *model = &context.model;
    row.name = std::make_shared<RichText>(13.0f, 0.0f, DWRITE_WORD_WRAPPING_WRAP);
    const std::string link = url;
    row.name->SetSpans(NameSpans(name, meta, !link.empty(), [model, link]() { model->Post("openExternalUrl", link); }));
    auto info = std::make_shared<Column>(0.0f);
    info->Add(row.name);
    row.status = Description(L"未下载");
    info->Add(row.status);

    auto actions = std::make_shared<Row>(8.0f);
    const bool hasId = !id.empty();
    const Json data = hasId ? Json{{"modelId", id}} : Json(nullptr);
    if (withRadio)
    {
        row.radio = std::make_shared<BareRadio>();
        row.radio->SetOnSelected([model, id]() { model->Update("association.sentence_collocation_model", id); });
        actions->Add(row.radio);
    }
    row.download = std::make_shared<Button>(L"下载", ButtonKind::Secondary);
    row.downloadAction = [model, downloadType, data]() { model->Post(downloadType, data); };
    // 待重启时同一个按钮换成「重启」，直接走设置页已有的 restartServer 动作。
    row.restartAction = [model]() { model->Post("restartServer"); };
    row.download->SetOnClick(row.downloadAction);
    actions->Add(row.download);
    row.import = std::make_shared<Button>(L"导入", ButtonKind::Secondary);
    row.import->SetOnClick([model, importType, data]() { model->Post(importType, data); });
    row.importBox = std::make_shared<Hideable>(row.import, true);
    actions->Add(row.importBox);
    auto remove = std::make_shared<Button>(L"删除", ButtonKind::Secondary);
    remove->SetOnClick([model, deleteType, data]() { model->Post(deleteType, data); });
    row.removeBox = std::make_shared<Hideable>(remove, false);
    actions->Add(row.removeBox);

    auto line = std::make_shared<Row>(12.0f);
    line->Add(info, 1.0f);
    line->Add(actions);
    row.view = line;
    return row;
}

// ---------------------------------------------------------------------------------
// Page state
// ---------------------------------------------------------------------------------
struct InputPage
{
    PageContext *context = nullptr;

    // Input mode
    std::vector<std::shared_ptr<Hideable>> chineseSections;
    std::shared_ptr<Hideable> japaneseSection;
    std::shared_ptr<RadioGroup> modeGroup = std::make_shared<RadioGroup>();
    std::shared_ptr<RadioGroup> schemeGroup = std::make_shared<RadioGroup>();
    std::shared_ptr<RadioGroup> japaneseGroup = std::make_shared<RadioGroup>();
    std::shared_ptr<Dropdown> shuangpin;
    std::shared_ptr<Label> shuangpinDirectory;
    std::shared_ptr<Hideable> shuangpinDirectoryBox;
    UINT_PTR shuangpinRefreshTimer = 0;
    bool shuangpinRefreshPending = false;

    // Word to character
    std::shared_ptr<RadioGroup> wordKeys = std::make_shared<RadioGroup>();
    std::shared_ptr<RadioOption> wordKeysBrackets;
    std::shared_ptr<RadioOption> wordKeysMinusEqual;
    std::shared_ptr<RadioGroup> punctuationLock = std::make_shared<RadioGroup>();

    // Fuzzy / smart punctuation
    std::vector<std::shared_ptr<Dimmable>> fuzzyRules;
    std::shared_ptr<Hideable> fuzzyHint;
    std::vector<std::shared_ptr<ToggleSwitch>> smartOptions;
    std::shared_ptr<Hideable> smartHint;

    // Translation
    std::string provider = "tencent";
    std::shared_ptr<ToggleSwitch> translationsToggle;
    std::shared_ptr<OpacityBox> translationOptions;
    std::vector<std::shared_ptr<Dimmable>> translationInputs;
    std::shared_ptr<Dimmable> showPosBlock;
    std::shared_ptr<Dimmable> singleSenseBlock;
    std::map<std::string, std::shared_ptr<Hideable>> providerFields;
    std::map<std::string, std::shared_ptr<ApiField>> fields;
    std::shared_ptr<ApiField> aiSummary;
    std::shared_ptr<Label> warning;
    std::shared_ptr<Hideable> warningBox;
    std::unique_ptr<CredentialTest> credentialTest;

    // Candidate word expansion panel (categories: translation, emoji, ...)
    std::shared_ptr<ToggleSwitch> panelToggle;
    std::shared_ptr<ToggleSwitch> panelTranslationToggle;
    std::shared_ptr<ToggleSwitch> panelEmojiToggle;
    std::shared_ptr<ToggleSwitch> panelKaomojiToggle;
    std::shared_ptr<ToggleSwitch> panelSymbolsToggle;
    std::shared_ptr<OpacityBox> panelOptions;
    std::vector<std::shared_ptr<Dimmable>> panelInputs;

    // Models
    ModelRow lattice;
    std::shared_ptr<ToggleSwitch> latticeToggle;
    UINT_PTR latticePoll = 0;
    bool latticeLinkShown = false;
    std::string latticeUrl;
    ModelRow neural;
    std::shared_ptr<ToggleSwitch> neuralToggle;
    UINT_PTR neuralPoll = 0;
    bool neuralLinkShown = false;
    std::string neuralUrl;
    std::shared_ptr<Column> collocationList;
    std::vector<ModelRow> collocationRows;
    std::shared_ptr<ToggleSwitch> collocationToggle;
    UINT_PTR collocationPoll = 0;

    // 与「AI 与云联想」页共用同一份解析：分槽优先、单值键只在它就是当前服务商时作数，
    // 可用性判定与后端 AiTranslation::IsUsableConfig 同一口径。
    voice_fields::ActiveAiConfig AiState() const
    {
        return voice_fields::ReadActiveAiConfig(context->model);
    }

    std::string Field(const char *key) const
    {
        auto it = fields.find(key);
        if (it == fields.end())
            return {};
        std::wstring text = it->second->GetText();
        const auto first = text.find_first_not_of(L" \t\r\n");
        const auto last = text.find_last_not_of(L" \t\r\n");
        return Narrow(first == std::wstring::npos ? std::wstring() : text.substr(first, last - first + 1));
    }

    ConfigMap TestConfig() const
    {
        if (provider == "ai")
            return {};
        if (provider == "aliyun")
            return {{"accessKeyId", Field("aliyunAccessKeyId")}, {"accessKeySecret", Field("aliyunAccessKeySecret")}};
        if (provider == "niutrans")
            return {{"appId", Field("niutransAppId")}, {"apiKey", Field("niutransApiKey")}};
        if (provider == "custom")
            return {{"endpoint", Field("customTranslationEndpoint")}, {"apiKey", Field("customTranslationApiKey")}};
        return {{"secretId", Field("tencentTmtSecretId")}, {"secretKey", Field("tencentTmtSecretKey")}};
    }

    void SyncWarning()
    {
        std::wstring text;
        bool ok = false;
        if (provider == "ai")
        {
            const voice_fields::ActiveAiConfig state = AiState();
            // 选择器模式下这里的 provider 已经解析成「生成式上次用的那家」，所以不再是 jev。
            // AI 联想总开关与这里无关，只要凭据齐就能用。
            text = L"请先在「AI 与云联想」页面填写当前服务商的接口地址、模型和 Token（Ollama 不需要）";
            ok = state.usable;
        }
        else if (provider == "custom")
        {
            text = L"请填写以 http:// 或 https:// 开头的完整接口地址";
            static const std::regex url(R"(^https?://\S+$)", std::regex::icase);
            ok = std::regex_match(Field("customTranslationEndpoint"), url);
        }
        else if (provider == "aliyun")
        {
            text = L"请填写 AccessKey ID 和 AccessKey Secret 后使用阿里云翻译";
            ok = !Field("aliyunAccessKeyId").empty() && !Field("aliyunAccessKeySecret").empty();
        }
        else if (provider == "niutrans")
        {
            text = L"请填写 APP ID 和 API Key 后使用小牛翻译";
            ok = !Field("niutransAppId").empty() && !Field("niutransApiKey").empty();
        }
        else
        {
            text = L"请填写 SecretId 和 SecretKey 后使用云端翻译";
            ok = !Field("tencentTmtSecretId").empty() && !Field("tencentTmtSecretKey").empty();
        }
        warning->SetText(text);
        warningBox->SetVisible(!ok);
    }

    void SyncProviderView()
    {
        for (auto &[name, box] : providerFields)
            box->SetVisible(name == provider);
        const voice_fields::ActiveAiConfig state = AiState();
        // 摘要用显示名而不是内部 id。
        if (state.usable)
            aiSummary->SetText(
                voice_fields::AiProviderLabel(state.provider, context->model.Find("ai_assistant.providers")) + L" · " +
                Widen(state.model));
        else
            aiSummary->SetText(std::wstring());
        SyncWarning();
    }

    // setCandidateTranslationOptionsDisabled: the block greys out, its rows and fields stop
    // taking input (the test button keeps working), 显示词性 and 只显示单条释义 turn aria-disabled.
    void SetTranslationsEnabled(bool enabled)
    {
        translationOptions->SetDimmed(!enabled);
        for (auto &dim : translationInputs)
            dim->SetDisabled(!enabled);
        showPosBlock->SetDisabled(!enabled);
        singleSenseBlock->SetDisabled(!enabled);
    }

    void SetPanelEnabled(bool enabled)
    {
        panelOptions->SetDimmed(!enabled);
        for (auto &dim : panelInputs)
            dim->SetDisabled(!enabled);
    }

    void SyncMode(const std::string &mode)
    {
        const bool japanese = mode == "japanese";
        for (auto &section : chineseSections)
            section->SetVisible(!japanese);
        japaneseSection->SetVisible(japanese);
    }
};

// Section card that can be hidden (`hidden` attribute) without leaving its margin behind.
std::shared_ptr<Hideable> AddHideableSection(PageBuilder &page, VisualPtr content, bool visible = true)
{
    auto section = std::make_shared<Section>(std::move(content));
    section->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
    auto box = std::make_shared<Hideable>(section, visible);
    page.AddRaw(box, 0.0f);
    return box;
}

// `.content-page-option`: column, margin-top 8px, items separated by .input-option-divider.
std::shared_ptr<Column> OptionList(const std::vector<VisualPtr> &items, float marginTop = 8.0f)
{
    auto column = std::make_shared<Column>(0.0f);
    column->SetMargin({0.0f, marginTop, 0.0f, 0.0f});
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i > 0)
            column->Add(std::make_shared<Divider>(Divider::Kind::Option));
        column->Add(items[i]);
    }
    return column;
}

std::shared_ptr<RadioOption> PaddedRadio(std::shared_ptr<RadioOption> radio)
{
    radio->SetInnerPadding({4.0f, 8.0f, 4.0f, 8.0f});
    return radio;
}

std::shared_ptr<Label> DisabledHint()
{
    TextStyle style;
    style.size = kFontCaption;
    auto label = std::make_shared<Label>(L"开启总开关后生效", style, TextRole::Muted);
    label->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
    return label;
}

// `.toggle-section-details .section-header { padding: 2px 0 }`
VisualPtr DetailsRow(std::wstring title, std::wstring description, VisualPtr control)
{
    return Padded(
        std::make_shared<SettingRow>(SubBlock(std::move(title), std::move(description), false), std::move(control)),
        {0.0f, 2.0f, 0.0f, 2.0f});
}

// `.frequency-option-row { padding: 10px 0 }`
VisualPtr FrequencyRow(VisualPtr left, VisualPtr control)
{
    return Padded(std::make_shared<SettingRow>(std::move(left), std::move(control)), {0.0f, 10.0f, 0.0f, 10.0f});
}

// `.translation-language-row { padding: 14px 0; gap: 16px }`
VisualPtr LanguageRow(std::wstring title, std::wstring description, VisualPtr control)
{
    auto left = SubBlock(std::move(title), std::move(description));
    left->SetMargin({0.0f, 0.0f, 6.0f, 0.0f});
    return Padded(std::make_shared<SettingRow>(left, std::move(control)), {0.0f, 14.0f, 0.0f, 14.0f});
}

// The expandable header (`.toggle-section-header`) with its master switch, and the details.
struct ToggleSection
{
    std::shared_ptr<Expander> expander;
    std::shared_ptr<Hideable> details;
};

ToggleSection MakeToggleSection(std::wstring title, VisualPtr detailsContent)
{
    ToggleSection section;
    section.expander = MakeExpander(std::move(title));
    detailsContent->SetMargin({18.0f, 4.0f, 0.0f, 0.0f});
    section.details = std::make_shared<Hideable>(detailsContent, false);
    BindExpander(section.expander, section.details);
    return section;
}
} // namespace

namespace
{
std::shared_ptr<Column> Col(std::initializer_list<VisualPtr> items, float gap = 0.0f)
{
    auto column = std::make_shared<Column>(gap);
    for (const auto &item : items)
        column->Add(item);
    return column;
}

std::shared_ptr<ToggleSwitch> Toggle(PageContext &context, const char *path)
{
    return BindToggle(context, path);
}

VisualPtr WithMarginTop(VisualPtr visual, float top)
{
    visual->SetMargin({0.0f, top, 0.0f, 0.0f});
    return visual;
}

std::vector<DropdownOption> BuiltinShuangpin()
{
    return Options({{"xiaohe", L"小鹤双拼"},
                    {"ziranma", L"自然码双拼"},
                    {"shoudao", L"首道双拼"},
                    {"microsoft", L"微软双拼"},
                    {"sogou", L"搜狗双拼"},
                    {"ziguang", L"紫光双拼"},
                    {"zhinengabc", L"智能ABC"},
                    {"guobiao", L"国标双拼"},
                    {"pinyinjiajia", L"拼音加加"}});
}

VisualPtr ItemSetting(std::wstring title, std::wstring description, VisualPtr control, bool first, bool last)
{
    auto row = std::make_shared<ItemRow>(
        std::make_shared<SettingRow>(SubBlock(std::move(title), std::move(description)), std::move(control)), last);
    if (first)
        row->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
    return row;
}
} // namespace

VisualPtr BuildInputPage(PageContext &context)
{
    auto state = std::make_shared<InputPage>();
    state->context = &context;
    InputPage *s = state.get();
    SettingsModel *model = &context.model;
    HostServices *host = &context.host;
    PageBuilder page(L"输入");

    // ---- 输入模式 ----
    {
        auto chinese = s->modeGroup->Add(L"中文", "chinese");
        auto japanese = s->modeGroup->Add(L"日文", "japanese");
        s->modeGroup->SetValue("chinese");
        s->modeGroup->SetOnChanged([s, model](const std::string &mode) {
            s->SyncMode(mode);
            model->Update("input.mode", mode);
        });
        auto grid = std::make_shared<TwoColumns>(chinese, japanese);
        page.AddSection(Col({MakeTitleBlock(L"输入模式", L"切换中文或日文输入，并保留各模式上次选择的方案"),
                             WithMarginTop(grid, 12.0f)}));
    }

    // ---- 输入方案 ----
    {
        auto quanpin = s->schemeGroup->Add(L"全拼", "quanpin");
        auto shuangpin = s->schemeGroup->Add(L"双拼", "shuangpin");
        auto wubi = s->schemeGroup->Add(L"五笔", "wubi");
        s->schemeGroup->SetValue("quanpin");
        s->schemeGroup->SetOnChanged([model](const std::string &value) { model->Update("input.schema", value); });
        // .input-option-content: margin-top 12px, gap 8px
        auto options = Col({quanpin, std::make_shared<Divider>(Divider::Kind::Option), shuangpin,
                            std::make_shared<Divider>(Divider::Kind::Option), wubi},
                           8.0f);
        options->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
        s->chineseSections.push_back(AddHideableSection(page, Col({MakeTitle(L"输入方案"), options})));
    }

    // ---- 双拼方案 ----
    {
        Dropdown::Options settings;
        settings.paddingLeft = 9.0f;
        settings.minWidth = 110.0f;
        settings.menuMinWidth = 110.0f;
        s->shuangpin = BindDropdown(context, "input.shuangpin_schema", BuiltinShuangpin(), settings);
        s->chineseSections.push_back(AddHideableSection(page, MakeRow(L"双拼方案", L"", s->shuangpin)));
    }

    // ---- 自定义双拼 ----
    {
        // The NSimSun runs make each line box taller than 1.6 x 12px (31 device px at 150%).
        auto description = std::make_shared<RichText>(kFontCaption, 20.67f / kFontCaption, DWRITE_WORD_WRAPPING_WRAP);
        description->SetSpans(
            {Plain(L"把 UTF-8 编码的 "), Code(L".toml"),
             Plain(L" 双拼方案文件放进下面的文件夹，点击“刷新方案”后即可在上方双拼方案中选择。 文件里用 "),
             Code(L"zero_initial_rule"), Plain(L" 选择零声母规则，在 "), Code(L"[initials]"), Plain(L" 和 "),
             Code(L"[finals]"), Plain(L" 中写声母、韵母对应的按键，完整写法见项目文档中的 "),
             Code(L"docs/custom-shuangpin.example.toml"),
             Plain(L"。 "
                   L"有错误的方案会显示为灰色，鼠标悬停可查看原因。修改正在使用的方案文件后，需要切换一次方案或重启输入"
                   L"法才会生效。")});
        description->SetMargin({0.0f, 4.0f, 0.0f, 0.0f});
        s->shuangpinDirectory = std::make_shared<Label>(L"", CaptionStyle(), TextRole::Muted);
        s->shuangpinDirectory->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        s->shuangpinDirectoryBox = std::make_shared<Hideable>(s->shuangpinDirectory, false);
        auto left = Col({MakeTitle(L"自定义双拼"), description, s->shuangpinDirectoryBox});

        auto open = std::make_shared<GhostButton>(L"打开目录");
        open->SetOnClick([model]() { model->Post("openShuangpinDirectory"); });
        auto refresh = std::make_shared<GhostButton>(L"刷新方案");
        refresh->SetOnClick([s, model, host]() {
            // Wait for the rescanned snapshot; report a failure if it does not come back.
            PageTimers::Stop(s->shuangpinRefreshTimer);
            s->shuangpinRefreshPending = true;
            s->shuangpinRefreshTimer = PageTimers::Start(host->Hwnd(), 5000, false, [s, host]() {
                s->shuangpinRefreshTimer = 0;
                if (!s->shuangpinRefreshPending)
                    return;
                s->shuangpinRefreshPending = false;
                host->ShowToast(L"刷新失败，请稍后重试", false);
            });
            model->Post("configRequest");
        });
        auto actions = std::make_shared<Row>(8.0f);
        actions->SetAlign(Row::Align::Start);
        actions->Add(open);
        actions->Add(refresh);
        // .section-header:has(> .helpcode-custom-actions) { align-items: flex-start; gap: 16px }
        auto row = std::make_shared<Row>(16.0f);
        row->SetAlign(Row::Align::Start);
        row->Add(left, 1.0f);
        row->Add(actions);
        s->chineseSections.push_back(AddHideableSection(page, row));
    }

    // ---- 五笔方案 ----
    {
        Dropdown::Options settings;
        settings.paddingLeft = 9.0f;
        settings.minWidth = 110.0f;
        settings.menuMinWidth = 110.0f;
        auto scheme = BindDropdown(context, "input.wubi_schema", Options({{"wubi86", L"86 五笔"}}), settings);
        // z 键只剩通配两态，配置值仍是字符串 off | wildcard。
        auto zMode = std::make_shared<ToggleSwitch>(false);
        zMode->SetOnChanged([model](bool on) { model->Update("input.wubi_z_mode", on ? "wildcard" : "off"); });
        std::weak_ptr<ToggleSwitch> weakZ = zMode;
        context.OnConfig([weakZ, model]() {
            if (auto toggle = weakZ.lock())
                toggle->SetChecked(model->GetString("input.wubi_z_mode") == "wildcard");
        });
        s->chineseSections.push_back(AddHideableSection(
            page,
            Col({MakeRow(L"五笔方案", L"", scheme),
                 ItemSetting(L"拼音混输", L"同一串字母同时给五笔码表和全拼，五笔候选在前",
                             Toggle(context, "input.wubi_mixed_pinyin"), true, false),
                 ItemSetting(L"z 键通配", L"开启后 z 可匹配码表中该位置的任意字母（含首位），常用于首码不确定时起头",
                             zMode, false, false),
                 ItemSetting(L"四码唯一时自动上屏",
                             L"敲满四码且码表只给一个候选时第四键直接上屏，不必再按空格；关闭后候选窗停留，等你选",
                             Toggle(context, "input.wubi_four_code_auto_commit"), false, false),
                 ItemSetting(L"第五码将首选上屏",
                             L"完整四码后再敲字母时上屏首选，并把该字母留作下一组合开头；关闭只在拼音混输开启时生效，"
                             L"这个字母并入混输串；混输关闭时仍按顶字上屏，不会丢字",
                             Toggle(context, "input.wubi_fifth_code_top_commit"), false, true)})));
    }

    // ---- 日语方案 ----
    {
        auto romaji = s->japaneseGroup->Add(L"罗马音", "romaji");
        s->japaneseGroup->SetValue("romaji");
        s->japaneseGroup->SetOnChanged([model](const std::string &value) {
            if (value == "romaji")
                model->Update("input.japanese_schema", value);
        });
        auto options = Col({romaji}, 8.0f);
        options->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
        s->japaneseSection =
            AddHideableSection(page,
                               Col({MakeTitle(L"日语方案"), options,
                                    Description(L"直接输入罗马音，提供平假名、片假名及日语词库候选", 10.0f)}),
                               false);
    }

    // ---- 输入纠错 ----
    page.AddSection(
        Col({MakeTitleBlock(
                 L"输入纠错",
                 L"全拼输错时按误键类型给出纠正候选，默认关闭；开启后预编辑始终显示原始输入，纠正结果仅出现在候选中"),
             ItemSetting(L"乱序纠错", L"相邻字母顺序打错时纠正，如 sahng → shang（上）；默认关闭",
                         Toggle(context, "quanpin.autocorrect_transposition"), true, false),
             ItemSetting(L"邻键纠错", L"误触相邻按键时纠正，如 shabg → shang（上）；默认关闭",
                         Toggle(context, "quanpin.autocorrect_neighbor"), false, false),
             ItemSetting(L"漏字纠错", L"漏敲一个字母时纠正，如 shng → shang（上）；默认关闭",
                         Toggle(context, "quanpin.autocorrect_deletion"), false, false),
             ItemSetting(L"多字纠错", L"多敲一个相邻按键时纠正，如 sjhang → shang（上）；默认关闭",
                         Toggle(context, "quanpin.autocorrect_insertion"), false, false),
             ItemSetting(L"显示纠错标记", L"纠错得到的候选末尾显示 * 号，便于和正常候选区分；不影响上屏文字",
                         Toggle(context, "quanpin.autocorrect_marker"), false, true)}));

    // ---- 模糊音 ----
    {
        auto master = BindToggle(context, "input.fuzzy_pinyin");
        const std::vector<std::pair<const wchar_t *, const char *>> initialRules = {
            {L"z ⇄ zh", "fuzzy_z_zh"}, {L"c ⇄ ch", "fuzzy_c_ch"}, {L"s ⇄ sh", "fuzzy_s_sh"},
            {L"n ⇄ l", "fuzzy_n_l"},   {L"f ⇄ h", "fuzzy_f_h"},   {L"r ⇄ l", "fuzzy_r_l"}};
        const std::vector<std::pair<const wchar_t *, const char *>> finalRules = {{L"an ⇄ ang", "fuzzy_an_ang"},
                                                                                  {L"en ⇄ eng", "fuzzy_en_eng"},
                                                                                  {L"in ⇄ ing", "fuzzy_in_ing"},
                                                                                  {L"ian ⇄ iang", "fuzzy_ian_iang"},
                                                                                  {L"uan ⇄ uang", "fuzzy_uan_uang"}};
        std::vector<VisualPtr> initials;
        for (const auto &[label, key] : initialRules)
            initials.push_back(BindCheck(context, label, std::string("input.") + key));
        std::vector<VisualPtr> finals;
        for (const auto &[label, key] : finalRules)
            finals.push_back(BindCheck(context, label, std::string("input.") + key));
        auto initialList = OptionList(initials);
        auto finalList = OptionList(finals);
        s->fuzzyRules = {std::make_shared<Dimmable>(initialList), std::make_shared<Dimmable>(finalList)};
        s->fuzzyHint = std::make_shared<Hideable>(DisabledHint(), false);
        auto details =
            Col({Description(L"开启后两个读音互相匹配，如输入 zhang 也可命中 zang 开头的词；全拼与双拼均生效", 0.0f),
                 Padded(MakeSubTitle(L"声母"), {0.0f, 2.0f, 0.0f, 2.0f}), s->fuzzyRules[0],
                 Padded(MakeSubTitle(L"韵母"), {0.0f, 2.0f, 0.0f, 2.0f}), s->fuzzyRules[1], s->fuzzyHint});
        auto section = MakeToggleSection(L"模糊音", details);
        page.AddSection(Col({std::make_shared<SettingRow>(section.expander, master), section.details}));
    }

    // ---- 候选词翻译 ----
    {
        auto master = std::make_shared<ToggleSwitch>(false);
        auto showPos = BindToggle(context, "general.candidate_translation_show_pos");
        s->showPosBlock = std::make_shared<Dimmable>(showPos, 1.0f);
        auto singleSense = BindToggle(context, "general.candidate_translation_single_sense");
        s->singleSenseBlock = std::make_shared<Dimmable>(singleSense, 1.0f);
        Dropdown::Options language;
        language.minWidth = 140.0f;
        language.menuMinWidth = 140.0f;
        auto provider = BindDropdown(context, "translation.provider",
                                     Options({{"tencent", L"腾讯云"},
                                              {"aliyun", L"阿里云"},
                                              {"niutrans", L"小牛翻译"},
                                              {"custom", L"自定义 DeepLX"},
                                              {"ai", L"AI 辅助"}}),
                                     language, ValueKind::String, [s](const std::string &value) {
                                         s->provider = value;
                                         s->SyncProviderView();
                                     });
        auto target = BindDropdown(context, "tencent_tmt.target_language",
                                   Options({{"en", L"英语"},
                                            {"fr", L"法语"},
                                            {"ja", L"日语"},
                                            {"es", L"西班牙语"},
                                            {"ru", L"俄语"},
                                            {"de", L"德语"},
                                            {"ko", L"韩语"},
                                            {"th", L"泰语"},
                                            {"vi", L"越南语"},
                                            {"it", L"意大利语"}}),
                                   language);
        // pointer-events: none on the rows and grids while the master switch is off.
        auto blockInput = [s](VisualPtr visual) {
            auto dim = std::make_shared<Dimmable>(std::move(visual), 1.0f);
            s->translationInputs.push_back(dim);
            return dim;
        };
        auto heading = [](std::wstring title, std::wstring description) {
            auto block = MakeTitleBlock(std::move(title), std::move(description));
            block->SetMargin({0.0f, 14.0f, 0.0f, 0.0f});
            return block;
        };
        auto field = [s, model](const char *id, const char *path, std::wstring label, std::wstring placeholder,
                                bool secret) {
            auto f = std::make_shared<ApiField>(std::move(label), std::move(placeholder), secret, false);
            const std::string configPath = path;
            f->SetOnCommit([s, model, configPath](const std::wstring &text) {
                model->Update(configPath, Narrow(text));
                s->SyncWarning();
            });
            s->fields[id] = f;
            return f;
        };
        auto grid = [](std::vector<std::pair<VisualPtr, bool>> items) {
            auto g = std::make_shared<FieldGrid>();
            for (const auto &[item, wide] : items)
                g->Add(item, wide);
            g->SetMargin({0.0f, 14.0f, 0.0f, 0.0f});
            return g;
        };

        auto tencent =
            Col({heading(L"腾讯云 API 凭据", L"用于在线机器翻译，凭据仅保存在本机配置文件中"),
                 blockInput(grid(
                     {{field("tencentTmtSecretId", "tencent_tmt.secret_id", L"SecretId", L"AKID...", false), false},
                      {field("tencentTmtSecretKey", "tencent_tmt.secret_key", L"SecretKey", L"SecretKey", true),
                       false}}))});
        auto aliyun =
            Col({heading(L"阿里云 API 凭据", L"在阿里云控制台「AccessKey 管理」中查看，凭据仅保存在本机配置文件中"),
                 blockInput(grid(
                     {{field("aliyunAccessKeyId", "aliyun_mt.secret_id", L"AccessKey ID", L"LTAI...", false), false},
                      {field("aliyunAccessKeySecret", "aliyun_mt.secret_key", L"AccessKey Secret", L"AccessKey Secret",
                             true),
                       false}}))});
        auto niutrans =
            Col({heading(L"小牛翻译 API 凭据", L"在“控制台—>API应用”中查看，凭据仅保存在本机配置文件中"),
                 blockInput(grid({{field("niutransAppId", "niutrans.app_id", L"APP ID", L"应用唯一标识", false), false},
                                  {field("niutransApiKey", "niutrans.apikey", L"API Key", L"apikey", true), false}}))});
        auto custom = Col({heading(L"自定义 DeepLX 服务", L"填写完整的 POST 接口地址；API Key 可留空"),
                           blockInput(grid({{field("customTranslationEndpoint", "custom_translation.endpoint",
                                                   L"接口地址", L"https://example.com/translate", false),
                                             true},
                                            {field("customTranslationApiKey", "custom_translation.api_key",
                                                   L"API Key（可选，以 Bearer Token 发送）", L"留空表示不鉴权", true),
                                             true}}))});
        s->aiSummary = std::make_shared<ApiField>(L"当前服务商与模型", L"未配置", false, true);
        auto ai = Col({heading(L"复用 AI 辅助配置",
                               L"使用「AI 与云联想」页生成式服务商的接口地址、Token 与模型，无需在此重复填写。"
                               L"那边选了「从候选里挑」也照样能用，翻译会用生成式那一家。"
                               L"不依赖「启用 AI 联想」开关，凭据填好就能用于翻译。"),
                       blockInput(grid({{s->aiSummary, true}}))});
        s->providerFields["tencent"] = std::make_shared<Hideable>(tencent, true);
        s->providerFields["aliyun"] = std::make_shared<Hideable>(aliyun, false);
        s->providerFields["niutrans"] = std::make_shared<Hideable>(niutrans, false);
        s->providerFields["custom"] = std::make_shared<Hideable>(custom, false);
        s->providerFields["ai"] = std::make_shared<Hideable>(ai, false);

        TextStyle warningStyle;
        warningStyle.size = kFontCaption;
        s->warning = std::make_shared<Label>(L"请填写 SecretId 和 SecretKey 后使用云端翻译", warningStyle);
        s->warning->SetColor(Hex(0xD7A64A));
        s->warning->SetMargin({0.0f, 10.0f, 0.0f, 0.0f});
        s->warningBox = std::make_shared<Hideable>(s->warning, true);
        s->credentialTest = std::make_unique<CredentialTest>(
            context, [s]() { return "translation." + s->provider; }, [s]() { return s->TestConfig(); },
            [s]() -> Json {
                if (s->provider != "ai")
                    return s->TestConfig();
                // 把当前生效的服务商配置摊平成页面字段，凭据测试据此判断「测试期间配置是否改过」，
                // 宿主再用同一份字段去发请求。
                const voice_fields::ActiveAiConfig state = s->AiState();
                return Json{{"provider", state.provider},
                            {"endpoint", state.endpoint},
                            {"token", state.token},
                            {"model", state.model}};
            });

        auto options = Col(
            {std::make_shared<Divider>(Divider::Kind::Option),
             blockInput(LanguageRow(L"显示词性", L"显示译文中的 n.、v. 等词性标记，关闭后仅显示释义", s->showPosBlock)),
             std::make_shared<Divider>(Divider::Kind::Option),
             blockInput(LanguageRow(L"只显示单条释义", L"每个候选只显示第一条译文释义；Ctrl+Enter 仍可选择全部释义上屏",
                                    s->singleSenseBlock)),
             std::make_shared<Divider>(Divider::Kind::Option),
             blockInput(LanguageRow(L"在线翻译服务",
                                    L"可使用腾讯云、阿里云、小牛翻译、自己部署的 DeepLX 兼容接口，或复用 AI 辅助的配置",
                                    provider)),
             std::make_shared<Divider>(Divider::Kind::Option),
             blockInput(LanguageRow(L"目标语种", L"英语优先使用本地词库；本地未命中时使用所选在线服务", target)),
             std::make_shared<Divider>(Divider::Kind::Option), s->providerFields["tencent"],
             s->providerFields["aliyun"], s->providerFields["niutrans"], s->providerFields["custom"],
             s->providerFields["ai"], s->warningBox, s->credentialTest->View()});
        s->translationOptions = std::make_shared<OpacityBox>(options);
        s->translationOptions->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        master->SetOnChanged([s, model](bool on) {
            model->Update("general.candidate_translations", on);
            s->SetTranslationsEnabled(on);
        });
        page.AddSection(
            Col({MakeRow(L"候选词翻译", L"在候选窗口中显示中文与所选语种的互译，每项最多两个简短释义", master),
                 s->translationOptions}));
        s->translationsToggle = master;
    }

    // ---- 候选词展开面板 ----
    {
        auto master = Toggle(context, "general.candidate_expansion_panel_enabled");
        s->panelToggle = master;
        auto translationCat = BindToggle(context, "general.candidate_expansion_panel_translation");
        s->panelTranslationToggle = translationCat;
        auto emojiCat = BindToggle(context, "general.candidate_expansion_panel_emoji");
        s->panelEmojiToggle = emojiCat;
        auto kaomojiCat = BindToggle(context, "general.candidate_expansion_panel_kaomoji");
        s->panelKaomojiToggle = kaomojiCat;
        auto symbolsCat = BindToggle(context, "general.candidate_expansion_panel_symbols");
        s->panelSymbolsToggle = symbolsCat;
        auto dim = [s](VisualPtr visual) -> VisualPtr {
            auto dimmed = std::make_shared<Dimmable>(std::move(visual), 1.0f);
            s->panelInputs.push_back(dimmed);
            return dimmed;
        };
        auto options = Col(
            {std::make_shared<Divider>(Divider::Kind::Option),
             LanguageRow(L"翻译",
                         L"在面板中列出高亮候选的译义，与「候选词翻译」使用同一个翻译源（本地词库与所配置的在线服务），"
                         L"最多列到下面设的上限；关闭后面板不再显示译文",
                         dim(translationCat)),
             LanguageRow(L"翻译最多显示", L"「翻译」类别在面板中最多列出的条数（0–9，0 表示不限制）",
                         dim(BindStepper(context, "general.candidate_expansion_panel_translation_max_count", 0, 9))),
             std::make_shared<Divider>(Divider::Kind::Option),
             LanguageRow(L"表情", L"按高亮候选模糊匹配 emoji 关键词，在面板中列出对应 emoji", dim(emojiCat)),
             LanguageRow(L"表情最多显示", L"「表情」类别在面板中最多列出的条数（0–9，0 表示不限制）",
                         dim(BindStepper(context, "general.candidate_expansion_panel_emoji_max_count", 0, 9))),
             std::make_shared<Divider>(Divider::Kind::Option),
             LanguageRow(L"颜文字", L"按高亮候选模糊匹配颜文字关键词，在面板中列出对应颜文字", dim(kaomojiCat)),
             LanguageRow(L"颜文字最多显示", L"「颜文字」类别在面板中最多列出的条数（0–9，0 表示不限制）",
                         dim(BindStepper(context, "general.candidate_expansion_panel_kaomoji_max_count", 0, 9))),
             std::make_shared<Divider>(Divider::Kind::Option),
             LanguageRow(L"符号", L"按高亮候选模糊匹配符号关键词，在面板中列出对应符号", dim(symbolsCat)),
             LanguageRow(L"符号最多显示", L"「符号」类别在面板中最多列出的条数（0–9，0 表示不限制）",
                         dim(BindStepper(context, "general.candidate_expansion_panel_symbols_max_count", 0, 9))),
             std::make_shared<Divider>(Divider::Kind::Option),
             LanguageRow(L"展开面板位置",
                         L"「下方」在选定候选词正下方弹出面板，左缘与高亮候选对齐；「右方」把面板放在候选窗右侧，"
                         L"与候选窗保持间距、顶边跟随高亮候选移动；右侧放不下时自动回到下方；",
                         dim(BindDropdown(context, "general.candidate_expansion_panel_placement",
                                          Options({{"down", L"下方"}, {"right", L"右方"}}))))});
        s->panelOptions = std::make_shared<OpacityBox>(options);
        s->panelOptions->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        master->SetOnChanged([s, model](bool on) {
            model->Update("general.candidate_expansion_panel_enabled", on);
            s->SetPanelEnabled(on);
        });
        page.AddSection(Col(
            {MakeRow(L"候选词展开面板",
                     L"按展开键在候选窗旁弹出面板，选择展开候选词输出对应内容；默认↑↓调整展开候选词，如果您使用输入法竖"
                     L"排模式，建议您在快捷栏页修改展开候选词的选词快捷键，否则有可能因快捷键冲突给您带来不好的体验",
                     master),
             s->panelOptions}));
    }

    // ---- 以词定字 ----
    {
        s->wordKeysBrackets = PaddedRadio(s->wordKeys->Add(L"[ / ]", "brackets"));
        s->wordKeysMinusEqual = PaddedRadio(s->wordKeys->Add(L"- / =", "minus_equal"));
        s->wordKeys->SetValue("brackets");
        s->wordKeysMinusEqual->SetDisabled(true);
        s->wordKeys->SetOnChanged(
            [model](const std::string &value) { model->Update("input.word_to_character_keys", value); });
        auto keys = std::make_shared<TopRuled>(
            Col({MakeSubTitle(L"以词定字快捷键"), OptionList({s->wordKeysBrackets, s->wordKeysMinusEqual})}), 16.0f);
        keys->SetMargin({0.0f, 18.0f, 0.0f, 0.0f});
        page.AddSection(Col({MakeRow(L"以词定字", L"开启后，按所选键组的左键上屏高亮候选的首个汉字，右键上屏末个汉字",
                                     Toggle(context, "input.word_to_character")),
                             keys}));
    }

    page.AddSection(MakeRow(L"Esc 保留已选词",
                            L"造词时按 Esc 只清除还没选的拼音、保留已选的词，再按一次 Esc 整体取消（小狼毫风格）",
                            Toggle(context, "input.escape_keeps_selected_word")));
    page.AddSection(MakeRow(L"回车上屏英文时入库",
                            L"按回车上屏输入的英文时，把这个单词写入英文词库，之后可作为候选出现",
                            Toggle(context, "input.enter_learns_english_word")));

    // ---- 固定标点 ----
    {
        auto follow = PaddedRadio(s->punctuationLock->Add(L"跟随中英文状态", "follow"));
        auto chinese = PaddedRadio(s->punctuationLock->Add(L"始终使用中文标点", "chinese"));
        auto english = PaddedRadio(s->punctuationLock->Add(L"始终使用英文标点", "english"));
        s->punctuationLock->SetValue("follow");
        s->punctuationLock->SetOnChanged(
            [model](const std::string &value) { model->Update("input.punctuation_lock", value); });
        page.AddSection(Col({MakeTitle(L"固定标点"), Description(L"切换中英文时的标点形态，三者互斥"),
                             OptionList({follow, chinese, english})}));
    }

    // ---- 顿号自定义 ----
    {
        std::vector<VisualPtr> keys;
        for (auto &check : AtLeastOneChecks(context, {{L"\\ 反斜杠", "input.enumeration_comma_backslash"},
                                                      {L"/ 斜杠", "input.enumeration_comma_slash"}}))
            keys.push_back(check);
        page.AddSection(Col({MakeTitle(L"顿号自定义"),
                             Description(L"中文标点状态下用哪些键输出顿号「、」，至少保留一个；未勾选的键按原样输出"),
                             OptionList(keys)}));
    }

    // ---- 智能标点 ----
    {
        auto master = BindToggle(context, "input.smart_punctuation");
        auto option = [s, &context](const char *path) {
            auto toggle = BindToggle(context, path);
            s->smartOptions.push_back(toggle);
            return toggle;
        };
        s->smartHint = std::make_shared<Hideable>(DisabledHint(), false);
        auto details = Col({Description(L"中文标点模式下的标点自动转换", 0.0f),
                            DetailsRow(L"中文标点后按空格转换",
                                       L"。，！？；：、 以及单独出现的 “ ” 【 】 《 》 （ ） 转为对应英文标点",
                                       option("input.smart_punctuation_space_convert")),
                            DetailsRow(L"同键转回中文", L"转换或直出的英文标点，2 秒内再按同一标点键转回中文",
                                       option("input.smart_punctuation_repeat_to_chinese")),
                            DetailsRow(L"数字后直出", L"数字后输入 , . : 直接输出英文标点（如 3.14）",
                                       option("input.smart_punctuation_direct_digit")),
                            DetailsRow(L"字母后直出", L"字母后输入 , . : 直接输出英文标点",
                                       option("input.smart_punctuation_direct_letter")),
                            s->smartHint});
        auto section = MakeToggleSection(L"智能标点", details);
        page.AddSection(Col({std::make_shared<SettingRow>(section.expander, master), section.details}));
    }

    page.AddSection(MakeRow(L"成对标点自动补全", L"输入左侧符号时自动补全右侧符号，并将光标置于中间",
                            Toggle(context, "input.paired_punctuation")));
    page.AddSection(MakeRow(
        L"默认中英文", L"",
        BindDropdown(context, "input.default_ime_mode", Options({{"chinese", L"中文"}, {"english", L"英文"}}))));
    page.AddSection(MakeRow(
        L"中英文状态", L"",
        BindDropdown(context, "input.ime_mode_scope", Options({{"app", L"按应用记忆"}, {"global", L"全局统一"}}))));
    page.AddSection(MakeRow(
        L"简繁输入", L"",
        BindDropdown(context, "input.character_set", Options({{"simplified", L"简体"}, {"traditional", L"繁体"}}))));

    // ---- 整句候选 ----
    {
        // 按需下载的模型：紧贴在各自开关下面，标题带折叠箭头，默认折叠。标题比上面开关的标题小一号，
        // 读起来是那个开关的附属项。
        auto downloadSection = [](std::wstring title, std::wstring description, const ModelRow &row) {
            TextStyle titleStyle = SubItemTitleStyle();
            titleStyle.size = 13.0f;
            auto expander = MakeExpander(std::move(title), titleStyle);
            auto list = Col({row.view}, 10.0f);
            list->SetMargin({0.0f, 6.0f, 0.0f, 0.0f});
            auto body = Col({MakeDescription(std::move(description)), list});
            body->SetMargin({18.0f, 4.0f, 10.0f, 0.0f});
            auto details = std::make_shared<Hideable>(body, false);
            BindExpander(expander, details);
            return Padded(Col({expander, details}), {0.0f, 0.0f, 0.0f, 10.0f});
        };

        s->lattice = MakeModelRow(context, "", L"词格语言模型 sc.lm", L"约 35 MB · LGPL-2.1", "", false,
                                  "latticeModelDownload", "latticeModelImport", "latticeModelDelete");
        auto latticeSection = downloadSection(
            L"Trigram 模型下载",
            L"三元语言模型不随安装包分发，下载后才能开启上面的 Trigram 整句开关，可随时删除；神经网络整句也会用它"
            L"给备选句打分。应用内下载较慢时，可点「浏览器下载」用浏览器下好，再点「导入」选中该 sc.lm 文件；"
            L"也可以在「帮助」页设置网络代理",
            s->lattice);
        s->latticeToggle = BindToggle(context, "association.sentence_wordlattice");

        s->neural = MakeModelRow(context, "", L"chinese-ime-lm 效果优先模型", L"约 25 MB · Apache-2.0", "", false,
                                 "neuralModelDownload", "neuralModelImport", "neuralModelDelete");
        auto neuralSection =
            downloadSection(L"神经网络模型（效果优先）下载",
                            L"效果优先模型不随安装包分发，下载后才能开启上面的整句开关，可随时删除。"
                            L"应用内下载较慢时，可点「浏览器下载」用浏览器下好，再点「导入」选中该 .safetensors 文件；"
                            L"也可以在「帮助」页设置网络代理",
                            s->neural);
        s->neuralToggle = BindToggle(context, "association.sentence_neural_desktop");

        s->collocationList = std::make_shared<Column>(10.0f);
        s->collocationList->SetMargin({0.0f, 6.0f, 0.0f, 0.0f});
        auto collocationBlock =
            Col({MakeSubTitle(L"octagram 语法模型（八股文，需先下载）"),
                 MakeDescription(
                     L"内置目录里的模型包各自下载、状态独立，激活其中一个用于整句加成；默认未选择任何模型"
                     L"（整句加成保持关闭），下载后单选激活，可随时切换或删除。应用内下载较慢时，可点「浏览器下载」"
                     L"用浏览器下好，再点「导入」选中该 .gram 文件；也可以在「帮助」页设置网络代理"),
                 s->collocationList});
        collocationBlock->SetMargin({0.0f, 0.0f, 10.0f, 0.0f});
        s->collocationToggle = BindToggle(context, "association.sentence_collocation_enabled");

        auto content = Col(
            {FrequencyRow(SubBlock(L"Trigram 模型整句", L"本地生成，下载模型后可用：使用三元语言模型生成整句候选"),
                          s->latticeToggle),
             latticeSection,
             FrequencyRow(SubBlock(L"Unigram 模型整句", L"本地生成，使用一元语言模型生成整句候选"),
                          Toggle(context, "association.sentence_google")),
             FrequencyRow(
                 SubBlock(
                     L"神经网络模型整句（效果优先）",
                     L"本地生成，下载模型后可用：结合已上屏的前文，从内部生成的最多 12 个备选句中选择，处理时间较长"),
                 s->neuralToggle),
             neuralSection,
             FrequencyRow(SubBlock(L"神经网络模型整句（速度优先）",
                                   L"本地生成，使用轻量模型从最多 12 个备选句中选择；两种模式可同时独立给出结果"),
                          Toggle(context, "association.sentence_neural_keyboard")),
             FrequencyRow(
                 SubBlock(
                     L"上文词库重排（实验）",
                     L"使用轻量神经模型，根据本次输入的上文调整单字和词组顺序，例如“冬天会下大”后输入 xue 优先选“雪”"),
                 Toggle(context, "association.context_candidate_rerank")),
             FrequencyRow(SubBlock(L"去重后补充次选",
                                   L"本地处理，某个模型的首选与已有候选重复时，显示该模型排名最高的下一条不同结果"),
                          Toggle(context, "association.sentence_show_next_on_duplicate")),
             FrequencyRow(SubBlock(L"显示整句来源标签",
                                   L"本地处理，在整句候选后显示〔Trigram〕〔Unigram〕〔神经D〕〔神经K〕〔万象〕"
                                   L"〔八股〕〔墨奇〕等来源标签"),
                          Toggle(context, "association.sentence_source_badge")),
             Padded(collocationBlock, {0.0f, 10.0f, 0.0f, 10.0f}),
             FrequencyRow(
                 SubBlock(L"octagram 语法整句",
                          L"本地生成，下载模型后可用：一并接入该模型的全部能力——解码时按字级搭配分选出更准的首选，"
                          L"并把搭配紧密的整句提到前面，以所选模型自己的来源行显示（万象〔万象〕、八股文〔"
                          L"八股〕、白霜〔墨奇〕）"),
                 s->collocationToggle),
             // 这一节里唯一联网的一项。它跟 AI 联想无关：不要 Token，也不用选服务商，
             // 默认还开着，所以必须留一个能点得到的地方。
             FrequencyRow(
                 SubBlock(L"云候选",
                          L"联网，把当前的全拼（双拼会先还原成全拼，日文输入时是罗马字）发送到 Google 的 inputtools "
                          L"服务换取一个候选。对方会收到拼音、你的 IP 地址和请求元数据，不会收到已上屏的文字、"
                          L"词库或设置。默认开启，无需账号；需要走代理时在「帮助」页设置，详见 PRIVACY.md"),
                 Toggle(context, "general.cloud_candidates"))});
        content->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        page.AddSection(
            Col({MakeTitleBlock(L"整句与云候选", L"输入多个拼音音节时，额外提供整句或云端给出的候选"), content}));
    }

    // ---- 拼音方案调频 ----
    {
        Dropdown::Options frequency;
        frequency.minWidth = 110.0f;
        frequency.menuMinWidth = 110.0f;
        auto content = Col({FrequencyRow(MakeSubTitle(L"调频方式"), BindDropdown(context, "frequency_adjustment.mode",
                                                                                 Options({{"disabled", L"关闭"},
                                                                                          {"pin", L"一次置顶"},
                                                                                          {"halve", L"折半调频"},
                                                                                          {"linear", L"线性调频"},
                                                                                          {"promote", L"一次置前"}}),
                                                                                 frequency)),
                            std::make_shared<Divider>(Divider::Kind::Option),
                            FrequencyRow(MakeSubTitle(L"触发频次(第几次上屏触发)"),
                                         BindDropdown(context, "frequency_adjustment.trigger_count",
                                                      NumberOptions(1, 6), frequency, ValueKind::Integer)),
                            std::make_shared<Divider>(Divider::Kind::Option),
                            FrequencyRow(MakeSubTitle(L"线性调频步长"),
                                         BindDropdown(context, "frequency_adjustment.linear_step", NumberOptions(1, 6),
                                                      frequency, ValueKind::Integer))});
        content->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        page.AddSection(Col({MakeTitle(L"拼音方案调频"), content}));
    }

    // ---- Snapshot -> page (config-sync.ts) ----
    context.OnConfig([s, state, model, host, &context]() {
        (void)state;
        const std::string mode = model->GetString("input.mode") == "japanese" ? "japanese" : "chinese";
        s->modeGroup->SetValue(mode);
        s->SyncMode(mode);
        const std::string schema = model->GetString("input.schema");
        if (schema == "quanpin" || schema == "shuangpin" || schema == "wubi")
            s->schemeGroup->SetValue(schema);
        s->japaneseGroup->SetValue("romaji");

        // Custom shuangpin schemas follow the built-in ones; broken ones are listed greyed out.
        if (const Json *schemas = model->Find("input.custom_shuangpin_schemas"); schemas && schemas->is_array())
        {
            std::vector<DropdownOption> options = BuiltinShuangpin();
            size_t broken = 0;
            size_t count = 0;
            for (const Json &entry : *schemas)
            {
                if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string() ||
                    !entry.contains("name") || !entry["name"].is_string())
                    continue;
                ++count;
                const bool error = !entry.value("error", std::string()).empty();
                broken += error ? 1 : 0;
                options.push_back({Widen(entry["name"].get<std::string>()), entry["id"].get<std::string>(), error, {}});
            }
            if (options.size() != s->shuangpin->GetOptions().size() || count > 0)
            {
                s->shuangpin->SetOptions(std::move(options));
                s->shuangpin->SetValue(model->GetString("input.shuangpin_schema"));
            }
            if (s->shuangpinRefreshPending)
            {
                s->shuangpinRefreshPending = false;
                PageTimers::Stop(s->shuangpinRefreshTimer);
                if (count == 0)
                    host->ShowToast(L"刷新成功，文件夹里还没有自定义双拼方案", true);
                else if (broken > 0)
                    host->ShowToast(L"找到 " + std::to_wstring(count) + L" 个自定义方案，其中 " +
                                        std::to_wstring(broken) + L" 个有错误，悬停在灰色方案上可查看原因",
                                    false);
                else
                    host->ShowToast(L"刷新成功，找到 " + std::to_wstring(count) + L" 个自定义方案", true);
            }
        }
        if (const Json *directory = model->Find("input.custom_shuangpin_directory");
            directory && directory->is_string())
        {
            s->shuangpinDirectory->SetText(L"文件夹：" + Widen(directory->get<std::string>()));
            s->shuangpinDirectoryBox->SetVisible(true);
        }

        // 以词定字: a key pair that pages candidates cannot also pick characters.
        if (const Json *paging = model->Find("general.paging_brackets"); paging && paging->is_boolean())
            s->wordKeysBrackets->SetDisabled(paging->get<bool>());
        if (const Json *paging = model->Find("general.paging_minus_equal"); paging && paging->is_boolean())
            s->wordKeysMinusEqual->SetDisabled(paging->get<bool>());
        if (const Json *keys = model->Find("input.word_to_character_keys"); keys && keys->is_string())
            s->wordKeys->SetValue(keys->get<std::string>());
        const std::string lock = model->GetString("input.punctuation_lock");
        if (lock == "follow" || lock == "chinese" || lock == "english")
            s->punctuationLock->SetValue(lock);

        if (const Json *smart = model->Find("input.smart_punctuation"); smart && smart->is_boolean())
        {
            for (auto &toggle : s->smartOptions)
                toggle->SetDisabled(!smart->get<bool>());
            s->smartHint->SetVisible(!smart->get<bool>());
        }
        if (const Json *fuzzy = model->Find("input.fuzzy_pinyin"); fuzzy && fuzzy->is_boolean())
        {
            for (auto &rules : s->fuzzyRules)
                rules->SetDisabled(!fuzzy->get<bool>());
            s->fuzzyHint->SetVisible(!fuzzy->get<bool>());
        }

        if (const Json *translations = model->Find("general.candidate_translations");
            translations && translations->is_boolean())
        {
            s->translationsToggle->SetChecked(translations->get<bool>());
            s->SetTranslationsEnabled(translations->get<bool>());
        }
        if (const Json *panel = model->Find("general.candidate_expansion_panel_enabled"); panel && panel->is_boolean())
        {
            s->panelToggle->SetChecked(panel->get<bool>());
            s->SetPanelEnabled(panel->get<bool>());
        }
        const std::string provider = model->GetString("translation.provider");
        s->provider = provider == "aliyun" || provider == "niutrans" || provider == "custom" || provider == "ai"
                          ? provider
                          : "tencent";
        s->fields["tencentTmtSecretId"]->SetText(Widen(model->GetString("tencent_tmt.secret_id")));
        s->fields["tencentTmtSecretKey"]->SetText(Widen(model->GetString("tencent_tmt.secret_key")));
        s->fields["aliyunAccessKeyId"]->SetText(Widen(model->GetString("aliyun_mt.secret_id")));
        s->fields["aliyunAccessKeySecret"]->SetText(Widen(model->GetString("aliyun_mt.secret_key")));
        s->fields["niutransAppId"]->SetText(Widen(model->GetString("niutrans.app_id")));
        s->fields["niutransApiKey"]->SetText(Widen(model->GetString("niutrans.apikey")));
        s->fields["customTranslationEndpoint"]->SetText(Widen(model->GetString("custom_translation.endpoint")));
        s->fields["customTranslationApiKey"]->SetText(Widen(model->GetString("custom_translation.api_key")));
        s->SyncProviderView();

        // Lattice language model row. Server 映射着 sc.lm 时删除只留标记（deletePending），
        // 下载按钮换成「重启输入法」，重启后 Server 在映射前把文件删掉。
        {
            const Json *status = model->Find("association.sentence_wordlattice_model_status");
            const std::string stateName = StateOf(status);
            const bool busy = IsBusy(stateName);
            const bool deletePending = status && status->is_object() && status->value("deletePending", false);
            s->lattice.status->SetText(deletePending ? L"已删除，重启输入法后释放文件" : StatusText(status));
            s->lattice.download->SetText(deletePending ? L"重启输入法" : L"下载");
            s->lattice.download->SetOnClick(deletePending ? s->lattice.restartAction : s->lattice.downloadAction);
            s->lattice.download->SetDisabled(!deletePending && (stateName == "ready" || busy));
            s->lattice.importBox->SetVisible(stateName != "ready" && !deletePending);
            s->lattice.import->SetDisabled(busy);
            s->lattice.removeBox->SetVisible(stateName == "ready");
            const std::string url = model->GetString("association.sentence_wordlattice_model_url");
            const bool showLink = !url.empty() && stateName != "ready" && !deletePending;
            if (showLink != s->latticeLinkShown || url != s->latticeUrl)
            {
                s->latticeLinkShown = showLink;
                s->latticeUrl = url;
                s->lattice.name->SetSpans(NameSpans(L"词格语言模型 sc.lm", L"约 35 MB · LGPL-2.1", showLink,
                                                    [model, url]() { model->Post("openExternalUrl", url); }));
            }
            // 模型没就绪时开关置灰并显示为关：Server 那边同样只在模型在时才出 Trigram 整句。
            // 绑定已按配置值设过一次，这里在它之后跑，只需在未就绪时覆盖成关。
            const bool ready = stateName == "ready";
            s->latticeToggle->SetDisabled(!ready);
            if (!ready)
                s->latticeToggle->SetChecked(false);
            // 待重启期间也轮询：点「重启」后 Server 删掉文件，下一份快照就回到「未下载」。
            if ((busy || deletePending) && s->latticePoll == 0)
                s->latticePoll = PageTimers::Start(host->Hwnd(), 2000, true,
                                                   [model]() { model->Post("latticeModelStatusRequest"); });
            else if (!busy && !deletePending)
                PageTimers::Stop(s->latticePoll);
        }

        // Neural model row (applyNeuralModelStatus).
        {
            const Json *status = model->Find("association.sentence_neural_desktop_model_status");
            const std::string stateName = StateOf(status);
            const bool busy = IsBusy(stateName);
            s->neural.status->SetText(StatusText(status));
            s->neural.download->SetDisabled(stateName == "ready" || busy);
            s->neural.importBox->SetVisible(stateName != "ready");
            s->neural.import->SetDisabled(busy);
            s->neural.removeBox->SetVisible(stateName == "ready");
            const std::string url = model->GetString("association.sentence_neural_desktop_model_url");
            const bool showLink = !url.empty() && stateName != "ready";
            if (showLink != s->neuralLinkShown || url != s->neuralUrl)
            {
                s->neuralLinkShown = showLink;
                s->neuralUrl = url;
                s->neural.name->SetSpans(NameSpans(L"chinese-ime-lm 效果优先模型", L"约 25 MB · Apache-2.0", showLink,
                                                   [model, url]() { model->Post("openExternalUrl", url); }));
            }
            // 同 Trigram：未下载时置灰并显示为关，引擎在模型缺席时本来也不重排。
            s->neuralToggle->SetDisabled(stateName != "ready");
            if (stateName != "ready")
                s->neuralToggle->SetChecked(false);
            if (busy && s->neuralPoll == 0)
                s->neuralPoll =
                    PageTimers::Start(host->Hwnd(), 2000, true, [model]() { model->Post("neuralModelStatusRequest"); });
            else if (!busy)
                PageTimers::Stop(s->neuralPoll);
        }

        // Collocation rows (seedCollocationRows + applyCollocationModelStatus).
        {
            const Json *catalog = model->Find("association.sentence_collocation_catalog");
            if (s->collocationRows.empty() && catalog && catalog->is_array() && !catalog->empty())
            {
                for (const Json &entry : *catalog)
                {
                    if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string())
                        continue;
                    const std::string id = entry["id"].get<std::string>();
                    std::wstring meta;
                    for (const char *key : {"sizeHint", "license"})
                    {
                        const std::string part = entry.value(key, std::string());
                        if (part.empty())
                            continue;
                        if (!meta.empty())
                            meta += L" · ";
                        meta += Widen(part);
                    }
                    ModelRow row = MakeModelRow(context, id, Widen(entry.value("displayName", id)), meta,
                                                entry.value("url", std::string()), true, "collocationModelDownload",
                                                "collocationModelImport", "collocationModelDelete");
                    if (msimeui::Window *window = context.host.Window())
                        row.view->Attach(window);
                    s->collocationList->Add(row.view);
                    s->collocationRows.push_back(std::move(row));
                }
            }
            const Json *statuses = model->Find("association.sentence_collocation_model_status");
            const std::string active = model->GetString("association.sentence_collocation_model");
            bool anyBusy = false;
            bool anyPending = false;
            if (statuses && statuses->is_object())
            {
                for (const auto &item : statuses->items())
                {
                    anyBusy = anyBusy || IsBusy(StateOf(&item.value()));
                    anyPending = anyPending || UpdatePending(&item.value());
                }
            }
            for (ModelRow &row : s->collocationRows)
            {
                const Json *status =
                    statuses && statuses->is_object() && statuses->contains(row.id) ? &(*statuses)[row.id] : nullptr;
                const std::string stateName = StateOf(status);
                const bool pending = UpdatePending(status);
                const bool update_available = UpdateAvailable(status);
                row.status->SetText(StatusText(status));
                // 每次快照都重设按钮文本和动作，避免上一次的「更新 / 重启」残留到别的状态。
                // 待重启时按钮直接变成可点的「重启」，点下去走设置页已有的 restartServer。
                row.download->SetText(pending ? L"重启输入法" : (update_available ? L"更新" : L"下载"));
                row.download->SetOnClick(pending ? row.restartAction : row.downloadAction);
                row.download->SetDisabled(pending ? false : ((stateName == "ready" && !update_available) || anyBusy));
                row.importBox->SetVisible(stateName != "ready");
                row.import->SetDisabled(anyBusy);
                row.removeBox->SetVisible(stateName == "ready");
                row.radio->SetState(active == row.id, stateName != "ready");
            }
            const Json *activeStatus =
                statuses && statuses->is_object() && statuses->contains(active) ? &(*statuses)[active] : nullptr;
            s->collocationToggle->SetDisabled(StateOf(activeStatus) != "ready");
            // 待重启期间也轮询：点「重启」后 Server 落位完，下一份快照就能把按钮恢复成「下载」。
            if ((anyBusy || anyPending) && s->collocationPoll == 0)
                s->collocationPoll = PageTimers::Start(host->Hwnd(), 2000, true,
                                                       [model]() { model->Post("collocationModelStatusRequest"); });
            else if (!anyBusy && !anyPending)
                PageTimers::Stop(s->collocationPoll);
        }
    });

    return page.Build();
}
} // namespace native_settings
