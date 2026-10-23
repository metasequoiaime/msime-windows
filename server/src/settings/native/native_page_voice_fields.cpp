#include "settings/native/native_page_voice_fields.h"

#include "ai/ai_provider_store.h"
#include "config/ai_provider.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <map>

namespace native_settings::voice_fields
{
namespace
{
// .voice-secret-icon-show / -hide, drawn with the page's stroke settings.
constexpr char kEyeShowSvg[] =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" )"
    R"(stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">)"
    R"(<path d="M2.5 12s3.5-6 9.5-6 9.5 6 9.5 6-3.5 6-9.5 6-9.5-6-9.5-6Z"/><circle cx="12" cy="12" r="2.75"/></svg>)";
constexpr char kEyeHideSvg[] =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" )"
    R"(stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M3 3l18 18"/>)"
    R"(<path d="M10.6 6.1A9.2 9.2 0 0 1 12 6c6 0 9.5 6 9.5 6a16.7 16.7 0 0 1-2.6 3.2M6.2 6.2C3.8 8 2.5 12 2.5 12s3.5 6 9.5 6a9 9 0 0 0 3-.5"/>)"
    R"(<path d="M9.8 9.8a3 3 0 0 0 4.4 4.4"/></svg>)";

bool Hit(msimeui::Window *window, const msimeui::Visual &visual, const POINT &point)
{
    return window && visual.HitTest(window->ClientPixelsToDips(point));
}

// color-mix(in srgb, <color> <amount>, transparent)
D2D1_COLOR_F Mix(D2D1_COLOR_F color, float amount)
{
    color.a *= amount;
    return color;
}

std::map<UINT_PTR, std::function<void()>> &Timeouts()
{
    static std::map<UINT_PTR, std::function<void()>> timeouts;
    return timeouts;
}

void CALLBACK TimeoutProc(HWND, UINT, UINT_PTR id, DWORD)
{
    KillTimer(nullptr, id);
    auto &timeouts = Timeouts();
    const auto it = timeouts.find(id);
    if (it == timeouts.end())
        return;
    auto callback = std::move(it->second);
    timeouts.erase(it);
    if (callback)
        callback();
}

// `line-height: normal` of an <input>: Blink rounds the primary font's ascent and descent
// to whole device pixels, so at fractional scales the box is up to a pixel taller than
// the DIP-rounded line height of ordinary text. 0 when the font can't be resolved.
float InputLineHeight(const TextStyle &style)
{
    std::wstring family = style.family.empty() ? UiFontFamily() : style.family;
    DWRITE_FONT_WEIGHT weight = style.weight;
    if (family == L"Noto Sans SC Medium")
    {
        family = L"Noto Sans SC";
        weight = DWRITE_FONT_WEIGHT_MEDIUM;
    }
    Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts;
    Microsoft::WRL::ComPtr<IDWriteFontFamily> fontFamily;
    Microsoft::WRL::ComPtr<IDWriteFont> font;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (!DWriteFactory() || FAILED(DWriteFactory()->GetSystemFontCollection(fonts.GetAddressOf())) ||
        FAILED(fonts->FindFamilyName(family.c_str(), &index, &exists)) || !exists ||
        FAILED(fonts->GetFontFamily(index, fontFamily.GetAddressOf())) ||
        FAILED(fontFamily->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                font.GetAddressOf())))
        return 0.0f;
    DWRITE_FONT_METRICS metrics{};
    font->GetMetrics(&metrics);
    if (metrics.designUnitsPerEm == 0)
        return 0.0f;
    const float scale = std::max(LayoutScale(), 1.0f);
    const float unit = style.size * scale / static_cast<float>(metrics.designUnitsPerEm);
    return (std::round(metrics.ascent * unit) + std::round(metrics.descent * unit) +
            std::round(metrics.lineGap * unit)) /
           scale;
}
} // namespace

TextStyle FieldTextStyle(float size, bool medium)
{
    TextStyle style;
    style.size = size;
    if (medium)
    {
        // font-weight: 500. The TSF editor only knows regular and bold, so the Medium face
        // is named directly; DirectWrite resolves it the same way for labels.
        static const bool hasMedium = IsFontInstalled(L"Noto Sans SC Medium");
        if (hasMedium && UiFontFamily() == L"Noto Sans SC")
            style.family = L"Noto Sans SC Medium";
        else
            style.weight = DWRITE_FONT_WEIGHT_MEDIUM;
    }
    return style;
}

TextStyle MonospaceStyle(float size)
{
    TextStyle style;
    style.size = size;
    style.family = IsFontInstalled(L"Consolas") ? L"Consolas" : L"Cascadia Mono";
    return style;
}

// ---------------------------------------------------------------------------------
// InputBox
// ---------------------------------------------------------------------------------
class InputBox::Reveal : public msimeui::Visual
{
  public:
    explicit Reveal(InputBox *owner) : owner_(owner)
    {
    }
    SizeF Measure(const SizeF &) override
    {
        return {32.0f, 32.0f};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        if (hovered_)
            FillRounded(dr, bounds_, 5.0f, Mix(p.text, 0.08f));
        const D2D1_COLOR_F color = hovered_ ? p.text : p.textMuted;
        const std::string markup = TintSvg(owner_->revealed_ ? kEyeHideSvg : kEyeShowSvg, color);
        DrawSvg(dr, markup, {bounds_.x + 7.0f, bounds_.y + 7.0f, 18.0f, 18.0f});
    }
    bool OnMouseDown(const POINT &, WPARAM) override
    {
        return true;
    }
    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (Hit(window_, *this, point))
        {
            owner_->revealed_ = !owner_->revealed_;
            owner_->box_->SetPasswordMode(!owner_->revealed_);
            InvalidateVisual();
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
        InvalidateVisual();
    }
    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, IDC_HAND);
    }

  private:
    InputBox *owner_ = nullptr;
    bool hovered_ = false;
};

// Scrollbar of a multi-line box, plus the `resize: vertical` grip. The text box only
// scrolls; this draws where it is and turns clicks and drags into offsets.
class InputBox::Scroller : public msimeui::Visual
{
  public:
    explicit Scroller(InputBox *owner) : owner_(owner)
    {
    }
    SizeF Measure(const SizeF &) override
    {
        return {};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    bool HitTest(const PointF &point) const override
    {
        if (!Contains(bounds_, point))
            return false;
        return Overflowing() || (owner_->options_.resizable && Contains(ResizerRect(), point));
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        if (Overflowing())
        {
            const RectF thumb = ThumbRect();
            if (Fluent())
            {
                // Chrome's Fluent bar: rounded triangles in the buttons, a 9px pill thumb.
                const D2D1_COLOR_F color = FluentColor(p, hovered_ || dragging_ == Drag::Thumb);
                FillRounded(dr, thumb, thumb.width * 0.5f, color);
                DrawArrow(dr, UpButtonRect(), true, color);
                DrawArrow(dr, DownButtonRect(), false, color);
            }
            else
            {
                FillRounded(dr, thumb, 2.0f,
                            hovered_ || dragging_ == Drag::Thumb ? p.scrollbarThumbHover : p.scrollbarThumb);
            }
        }
        if (owner_->options_.resizable)
        {
            // Two diagonal strokes, each a light line over a dark one, in the corner.
            const RectF r = ResizerRect();
            const float right = r.x + r.width - 2.0f;
            const float bottom = r.y + r.height - 2.0f;
            const D2D1_COLOR_F light = p.light ? D2D1::ColorF(0x6E6E6E) : D2D1::ColorF(0xC8C8C8);
            const D2D1_COLOR_F dark = p.light ? D2D1::ColorF(0xFFFFFF) : D2D1::ColorF(0x101010);
            for (const float length : {9.0f, 5.0f})
            {
                DrawLine(dr, {right - length + 1.0f, bottom + 1.0f}, {right + 1.0f, bottom - length + 1.0f}, dark);
                DrawLine(dr, {right - length, bottom}, {right, bottom - length}, light);
            }
        }
    }
    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        const PointF at = window_->ClientPixelsToDips(point);
        if (owner_->options_.resizable && Contains(ResizerRect(), at))
        {
            dragging_ = Drag::Resize;
            grab_ = at.y;
            grabValue_ = owner_->Height();
            return true;
        }
        if (!Overflowing())
            return true;
        const msimeui::TextBox::ScrollInfo info = owner_->box_->GetScrollInfo();
        const RectF thumb = ThumbRect();
        if (Contains(thumb, at))
        {
            dragging_ = Drag::Thumb;
            grab_ = at.y;
            grabValue_ = info.offset;
        }
        else if (Fluent() && Contains(UpButtonRect(), at))
        {
            owner_->box_->SetScrollOffset(info.offset - 40.0f);
        }
        else if (Fluent() && Contains(DownButtonRect(), at))
        {
            owner_->box_->SetScrollOffset(info.offset + 40.0f);
        }
        else
        {
            // Track: a page toward the click.
            const float page = std::max(info.viewportHeight * 0.875f, 1.0f);
            owner_->box_->SetScrollOffset(info.offset + (at.y < thumb.y ? -page : page));
        }
        InvalidateVisual();
        return true;
    }
    bool OnMouseMove(const POINT &point, WPARAM) override
    {
        const PointF at = window_->ClientPixelsToDips(point);
        if (dragging_ == Drag::Resize)
        {
            owner_->resizedHeight_ = std::max(grabValue_ + at.y - grab_, owner_->options_.height);
            owner_->InvalidateMeasure();
            return true;
        }
        if (dragging_ == Drag::Thumb)
        {
            const msimeui::TextBox::ScrollInfo info = owner_->box_->GetScrollInfo();
            const RectF track = TrackRect();
            const float travel = track.height - ThumbRect().height;
            if (travel > 0.0f)
                owner_->box_->SetScrollOffset(grabValue_ + (at.y - grab_) / travel * info.maxOffset);
            return true;
        }
        resizerHover_ = owner_->options_.resizable && Contains(ResizerRect(), at);
        const bool hovered = Overflowing() && Contains(ThumbRect(), at);
        if (hovered != hovered_)
        {
            hovered_ = hovered;
            InvalidateVisual();
        }
        return true;
    }
    bool OnMouseUp(const POINT &, WPARAM) override
    {
        dragging_ = Drag::None;
        InvalidateVisual();
        return true;
    }
    void OnMouseLeave() override
    {
        resizerHover_ = false;
        if (hovered_)
        {
            hovered_ = false;
            InvalidateVisual();
        }
    }
    bool OnMouseWheel(const POINT &point, short delta, WPARAM keyState) override
    {
        return owner_->box_->OnMouseWheel(point, delta, keyState);
    }
    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, dragging_ == Drag::Resize || resizerHover_ ? IDC_SIZENS : IDC_ARROW);
    }
    // Width the bar takes from the text while it shows.
    static float Gutter(Options::Scrollbar style)
    {
        return style == Options::Scrollbar::Fluent ? 15.0f : 8.0f;
    }

  private:
    // The arrow buttons are shorter than the 15px bar is wide.
    static constexpr float kFluentButton = 12.0f;

    enum class Drag
    {
        None,
        Thumb,
        Resize,
    };

    bool Fluent() const
    {
        return owner_->options_.scrollbar == Options::Scrollbar::Fluent;
    }
    bool Overflowing() const
    {
        return owner_->box_->GetScrollInfo().maxOffset > 0.0f;
    }
    static D2D1_COLOR_F FluentColor(const Palette &p, bool hovered)
    {
        if (p.light)
            return hovered ? D2D1::ColorF(0x5C5C5C) : D2D1::ColorF(0x868686);
        return hovered ? D2D1::ColorF(0xBEBEBE) : D2D1::ColorF(0x9F9F9F);
    }
    // The 15px corner square under the bar.
    RectF ResizerRect() const
    {
        const float size = 15.0f;
        return {bounds_.x + bounds_.width - size, bounds_.y + bounds_.height - size, size, size};
    }
    float BarBottom() const
    {
        return bounds_.y + bounds_.height - (owner_->options_.resizable ? 15.0f : 0.0f);
    }
    RectF UpButtonRect() const
    {
        return {bounds_.x, bounds_.y, bounds_.width, kFluentButton};
    }
    RectF DownButtonRect() const
    {
        return {bounds_.x, BarBottom() - kFluentButton, bounds_.width, kFluentButton};
    }
    RectF TrackRect() const
    {
        const float buttons = Fluent() ? kFluentButton : 0.0f;
        const float top = bounds_.y + buttons;
        return {bounds_.x, top, bounds_.width, std::max(BarBottom() - buttons - top, 0.0f)};
    }
    RectF ThumbRect() const
    {
        const msimeui::TextBox::ScrollInfo info = owner_->box_->GetScrollInfo();
        const RectF track = TrackRect();
        const float ratio = info.contentHeight > 0.0f ? info.viewportHeight / info.contentHeight : 1.0f;
        const float length = std::min(track.height, std::max(track.height * ratio, Fluent() ? 17.0f : 20.0f));
        const float progress = info.maxOffset > 0.0f ? info.offset / info.maxOffset : 0.0f;
        const float y = track.y + (track.height - length) * progress;
        if (Fluent())
        {
            // 9px wide and centered.
            return {bounds_.x + (bounds_.width - 9.0f) * 0.5f, y, 9.0f, length};
        }
        return {bounds_.x, y, bounds_.width, length};
    }
    static void DrawArrow(msimeui::DeviceResources &dr, const RectF &button, bool up, const D2D1_COLOR_F &color)
    {
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(color);
        if (!target || !brush)
            return;
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        target->GetFactory(factory.GetAddressOf());
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (!factory || FAILED(factory->CreatePathGeometry(path.GetAddressOf())) ||
            FAILED(path->Open(sink.GetAddressOf())))
            return;
        // A 9 x 5 triangle whose corners the round-joined stroke softens.
        const float cx = button.x + button.width * 0.5f;
        const float cy = button.y + button.height * 0.5f;
        const float half = 4.5f;
        const float rise = 3.0f;
        const float tipY = up ? cy - rise : cy + rise;
        const float baseY = up ? cy + rise : cy - rise;
        sink->BeginFigure(D2D1::Point2F(cx, tipY), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(cx + half, baseY));
        sink->AddLine(D2D1::Point2F(cx - half, baseY));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round;
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                               D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                   nullptr, 0, round.GetAddressOf());
        target->FillGeometry(path.Get(), brush);
        target->DrawGeometry(path.Get(), brush, 1.0f, round.Get());
    }

    InputBox *owner_ = nullptr;
    Drag dragging_ = Drag::None;
    float grab_ = 0.0f;
    float grabValue_ = 0.0f;
    bool hovered_ = false;
    bool resizerHover_ = false;
};

InputBox::InputBox(Options options) : options_(std::move(options))
{
    box_ = std::make_shared<msimeui::TextBox>(std::max(options_.height, 20.0f), options_.placeholder);
    box_->SetChromeVisible(false);
    box_->SetMultiline(options_.multiline);
    box_->SetPasswordMode(options_.password);
    box_->SetFontSize(options_.font.size);
    box_->SetPlaceholderFontSize(options_.font.size);
    auto commit = [this]() {
        const std::wstring text = box_->GetText();
        if (text == committed_)
            return;
        committed_ = text;
        if (onCommit_)
        {
            const auto handler = onCommit_;
            handler(text);
        }
    };
    box_->SetOnFocusChanged([this, commit](bool focused) {
        if (!focused)
            commit();
        InvalidateVisual();
    });
    box_->SetOnSubmit(commit);
    AdoptChild(box_);
    if (options_.reveal)
    {
        reveal_ = std::make_shared<Reveal>(this);
        AdoptChild(reveal_);
    }
    if (options_.multiline)
    {
        scroller_ = std::make_shared<Scroller>(this);
        AdoptChild(scroller_);
    }
}

void InputBox::ApplyAppearance()
{
    const Palette &p = CurrentPalette();
    appliedLight_ = p.light;
    appearanceApplied_ = true;
    msimeui::TextBox::Appearance appearance;
    appearance.cornerRadius = options_.radius;
    appearance.borderWidth = options_.bare ? 0.0f : CssBorder();
    appearance.paddingX = options_.bare ? 0.0f : options_.paddingX;
    appearance.paddingY = options_.bare ? 0.0f : options_.paddingY;
    appearance.text = p.text;
    appearance.selection = WithAlpha(p.accent, 0.45f);
    appearance.placeholder = p.textSubtle;
    appearance.fontFamily = options_.font.family.empty() ? UiFontFamily() : options_.font.family;
    appearance.fontWeight = options_.font.weight;
    appearance.letterSpacing = options_.letterSpacing;
    if (options_.multiline)
    {
        appearance.lineHeight = options_.lineHeight * options_.font.size;
        appearance.scrollbarGutter = Scroller::Gutter(options_.scrollbar);
    }
    box_->SetAppearance(appearance);
}

void InputBox::SetText(const std::wstring &text)
{
    if (box_->IsFocused())
        return;
    committed_ = text;
    box_->SetText(text);
}

std::wstring InputBox::GetText() const
{
    return box_->GetText();
}

void InputBox::SetPlaceholder(const std::wstring &placeholder)
{
    box_->SetPlaceholderText(placeholder);
}

void InputBox::SetOnCommit(std::function<void(const std::wstring &)> handler)
{
    onCommit_ = std::move(handler);
}

bool InputBox::IsFocused() const
{
    return box_->IsFocused();
}

void InputBox::Focus()
{
    if (window_)
        window_->FocusVisual(box_.get());
}

float InputBox::Height() const
{
    if (options_.height > 0.0f)
        return std::max(resizedHeight_, options_.height);
    const float line = InputLineHeight(options_.font);
    return (line > 0.0f ? line : LineHeight(options_.font)) + options_.paddingY * 2.0f +
           (options_.bare ? 0.0f : CssBorder() * 2.0f);
}

SizeF InputBox::Measure(const SizeF &availableSize)
{
    if (!appearanceApplied_)
        ApplyAppearance();
    const float height = Height();
    box_->SetPreferredHeight(height);
    box_->MeasureInLayout({availableSize.width, height});
    if (reveal_)
        reveal_->MeasureInLayout(availableSize);
    return {availableSize.width, height};
}

RectF InputBox::RevealRect() const
{
    return {bounds_.x + bounds_.width - 5.0f - 32.0f, bounds_.y + (bounds_.height - 32.0f) * 0.5f, 32.0f, 32.0f};
}

void InputBox::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    // padding-right: 42px leaves room for the eye; the editor ends there.
    const float reserve = reveal_ ? 42.0f - options_.paddingX : 0.0f;
    box_->ArrangeInLayout({finalRect.x, finalRect.y, std::max(finalRect.width - reserve, 0.0f), finalRect.height});
    if (reveal_)
        reveal_->ArrangeInLayout(RevealRect());
    if (scroller_)
    {
        // Inside the border, along the right edge, the full inner height.
        const float border = options_.bare ? 0.0f : CssBorder();
        const float gutter = Scroller::Gutter(options_.scrollbar);
        scroller_->Arrange({finalRect.x + finalRect.width - border - gutter, finalRect.y + border, gutter,
                            std::max(finalRect.height - border * 2.0f, 0.0f)});
    }
}

void InputBox::Render(msimeui::DeviceResources &dr)
{
    if (appliedLight_ != CurrentPalette().light)
        ApplyAppearance();
    const Palette &p = CurrentPalette();
    if (!options_.bare)
    {
        const bool focused = box_->IsFocused();
        if (focused)
            FillRounded(dr, Inflate(bounds_, 2.0f), options_.radius + 2.0f, Mix(p.accent, 0.2f));
        FillRounded(dr, bounds_, options_.radius, focused ? p.controlBg : p.surfaceSunken);
        StrokeRounded(dr, bounds_, options_.radius, focused ? p.accent : p.controlBorder, 1.0f);
    }
    box_->Render(dr);
    if (reveal_)
        reveal_->Render(dr);
    if (scroller_)
        scroller_->Render(dr);
}

void InputBox::Attach(msimeui::Window *window)
{
    msimeui::Visual::Attach(window);
    box_->Attach(window);
    if (reveal_)
        reveal_->Attach(window);
    if (scroller_)
        scroller_->Attach(window);
}

msimeui::Visual *InputBox::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
        return nullptr;
    if (reveal_ && reveal_->HitTest(point))
        return reveal_.get();
    if (scroller_ && scroller_->HitTest(point))
        return scroller_.get();
    return box_.get();
}

msimeui::Visual *InputBox::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point) || (reveal_ && reveal_->HitTest(point)) || (scroller_ && scroller_->HitTest(point)))
        return nullptr;
    return box_.get();
}

msimeui::Visual *InputBox::FindFirstFocusableDescendant()
{
    return box_.get();
}

// ---------------------------------------------------------------------------------
// Field / FormGrid
// ---------------------------------------------------------------------------------
Field::Field(std::wstring label, VisualPtr control) : Column(7.0f)
{
    label_ = std::make_shared<Label>(std::move(label), FieldTextStyle(), TextRole::Secondary);
    Add(label_);
    Add(std::move(control));
}

void Field::SetLabel(std::wstring label)
{
    label_->SetText(std::move(label));
}

FormGrid::FormGrid(float gap) : gap_(gap)
{
}

FormGrid &FormGrid::Add(VisualPtr child, bool wide)
{
    wide_.push_back(wide);
    AddChild(std::move(child));
    return *this;
}

void FormGrid::Place(float)
{
    placements_.assign(children_.size(), {});
    size_t row = 0;
    size_t column = 0;
    for (size_t i = 0; i < children_.size(); ++i)
    {
        auto *hideable = dynamic_cast<Hideable *>(children_[i].get());
        Placement &placement = placements_[i];
        placement.visible = !hideable || hideable->IsVisible();
        placement.wide = wide_[i];
        if (!placement.visible)
            continue;
        if (placement.wide)
        {
            if (column != 0)
            {
                ++row;
                column = 0;
            }
            placement.row = row++;
            placement.column = 0;
            continue;
        }
        placement.row = row;
        placement.column = column;
        if (++column == 2)
        {
            column = 0;
            ++row;
        }
    }
}

SizeF FormGrid::Measure(const SizeF &availableSize)
{
    Place(availableSize.width);
    const float columnWidth = std::max((availableSize.width - gap_) * 0.5f, 0.0f);
    rowHeights_.clear();
    for (size_t i = 0; i < children_.size(); ++i)
    {
        const Placement &placement = placements_[i];
        if (!placement.visible)
        {
            children_[i]->MeasureInLayout({0.0f, 0.0f});
            continue;
        }
        const SizeF size =
            children_[i]->MeasureInLayout({placement.wide ? availableSize.width : columnWidth, availableSize.height});
        if (rowHeights_.size() <= placement.row)
            rowHeights_.resize(placement.row + 1, 0.0f);
        rowHeights_[placement.row] = std::max(rowHeights_[placement.row], size.height);
    }
    float height = 0.0f;
    for (size_t row = 0; row < rowHeights_.size(); ++row)
        height += rowHeights_[row] + (row > 0 ? gap_ : 0.0f);
    return {availableSize.width, height};
}

void FormGrid::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    const float columnWidth = std::max((finalRect.width - gap_) * 0.5f, 0.0f);
    std::vector<float> rowTops(rowHeights_.size(), finalRect.y);
    for (size_t row = 1; row < rowHeights_.size(); ++row)
        rowTops[row] = rowTops[row - 1] + rowHeights_[row - 1] + gap_;
    for (size_t i = 0; i < children_.size(); ++i)
    {
        const Placement &placement = placements_[i];
        if (!placement.visible || placement.row >= rowHeights_.size())
        {
            children_[i]->ArrangeInLayout({finalRect.x, finalRect.y, 0.0f, 0.0f});
            continue;
        }
        const float x = finalRect.x + (placement.column == 0 ? 0.0f : columnWidth + gap_);
        // Grid items stretch to the row; their content stays at the top.
        children_[i]->ArrangeInLayout(
            {x, rowTops[placement.row], placement.wide ? finalRect.width : columnWidth, rowHeights_[placement.row]});
    }
}

void FormGrid::Render(msimeui::DeviceResources &dr)
{
    for (size_t i = 0; i < children_.size(); ++i)
    {
        if (placements_.size() > i && !placements_[i].visible)
            continue;
        if (dr.IsVisible(Inflate(children_[i]->GetBounds(), 24.0f)))
            children_[i]->Render(dr);
    }
}

// ---------------------------------------------------------------------------------
// PlainButton
// ---------------------------------------------------------------------------------
PlainButton::PlainButton(std::wstring text, Style style) : text_(std::move(text)), style_(std::move(style))
{
}

void PlainButton::SetText(std::wstring text)
{
    if (text == text_)
        return;
    text_ = std::move(text);
    InvalidateMeasure();
    InvalidateVisual();
}

void PlainButton::SetDisabled(bool disabled)
{
    if (disabled_ == disabled)
        return;
    disabled_ = disabled;
    InvalidateVisual();
}

void PlainButton::SetOnClick(std::function<void()> handler)
{
    onClick_ = std::move(handler);
}

SizeF PlainButton::Measure(const SizeF &availableSize)
{
    const float border = style_.border ? CssBorder() : 0.0f;
    const float width =
        std::max(MeasureTextWidth(text_, style_.font) + style_.paddingX * 2.0f + border * 2.0f, style_.minWidth);
    const float height =
        style_.height > 0.0f ? style_.height : LineHeight(style_.font) + style_.paddingY * 2.0f + border * 2.0f;
    return {std::min(width, availableSize.width), height};
}

void PlainButton::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void PlainButton::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    const bool hot = (hovered_ || pressed_) && !disabled_;
    if (disabled_)
        PushOpacity(dr, style_.disabledOpacity);
    auto pick = [&p](D2D1_COLOR_F (*color)(const Palette &)) { return color ? color(p) : D2D1::ColorF(0, 0.0f); };
    const D2D1_COLOR_F fill = hot && style_.hoverFill ? pick(style_.hoverFill) : pick(style_.fill);
    FillRounded(dr, bounds_, style_.radius, fill);
    if (style_.border)
        StrokeRounded(dr, bounds_, style_.radius, pick(style_.border), 1.0f);
    const D2D1_COLOR_F text = hot && style_.hoverText ? pick(style_.hoverText) : pick(style_.text);
    DrawTextInRect(dr, text_, style_.font, bounds_, text, true);
    if (disabled_)
        PopOpacity(dr);
}

bool PlainButton::OnMouseDown(const POINT &point, WPARAM)
{
    pressed_ = !disabled_ && Hit(window_, *this, point);
    InvalidateVisual();
    return pressed_;
}

bool PlainButton::OnMouseUp(const POINT &point, WPARAM)
{
    if (!pressed_)
        return false;
    pressed_ = false;
    InvalidateVisual();
    if (Hit(window_, *this, point) && onClick_ && !disabled_)
    {
        // A handler may tear down the visual that owns onClick_ while it runs.
        const auto handler = onClick_;
        handler();
    }
    return true;
}

void PlainButton::OnMouseEnter()
{
    hovered_ = true;
    InvalidateVisual();
}

void PlainButton::OnMouseLeave()
{
    hovered_ = false;
    pressed_ = false;
    InvalidateVisual();
}

HCURSOR PlainButton::GetCursor() const
{
    return LoadCursor(nullptr, disabled_ ? IDC_WAIT : IDC_HAND);
}

PlainButton::Style CredentialButtonStyle()
{
    PlainButton::Style style;
    style.font = ControlStyle();
    style.font.size = 12.0f;
    style.font.lineHeight = 1.4f;
    style.paddingX = 10.0f;
    style.paddingY = 4.0f;
    style.minWidth = 64.0f;
    style.radius = 5.0f;
    style.fill = [](const Palette &p) { return p.buttonSecondaryBg; };
    style.hoverFill = [](const Palette &p) { return p.buttonSecondaryHover; };
    style.border = [](const Palette &p) { return p.controlBorder; };
    style.text = [](const Palette &p) { return p.buttonSecondaryText; };
    return style;
}

PlainButton::Style ModelFetchButtonStyle()
{
    PlainButton::Style style = CredentialButtonStyle();
    style.font.lineHeight = 1.0f;
    style.paddingX = 12.0f;
    style.paddingY = 0.0f;
    style.minWidth = 0.0f;
    style.radius = 6.0f;
    // align-items: stretch in .ai-model-row: as tall as the 40px combobox.
    style.height = 40.0f;
    return style;
}

PlainButton::Style PromptResetButtonStyle()
{
    PlainButton::Style style;
    style.font = BodyStyle();
    style.font.size = 13.0f;
    style.paddingX = 12.0f;
    style.paddingY = 8.0f;
    style.radius = 6.0f;
    style.hoverFill = [](const Palette &p) { return Mix(p.text, 0.08f); };
    style.border = [](const Palette &p) { return p.controlBorder; };
    style.text = [](const Palette &p) { return p.textSecondary; };
    style.hoverText = [](const Palette &p) { return p.text; };
    return style;
}

// ---------------------------------------------------------------------------------
// StatusText / timeouts / CredentialTest
// ---------------------------------------------------------------------------------
StatusText::StatusText() : Hideable(nullptr, false)
{
    TextStyle style;
    style.size = 12.0f;
    style.lineHeight = 1.45f;
    style.wrap = true;
    label_ = std::make_shared<Label>(L"", style, TextRole::Muted);
    SetChild(label_);
}

void StatusText::Set(const std::wstring &text, const std::string &kind)
{
    label_->SetText(text);
    if (kind == "success")
        label_->SetColor(Hex(0x4CAF72));
    else if (kind == "error")
        label_->SetColor(Hex(0xD96767));
    else
        label_->SetRole(TextRole::Muted);
    SetVisible(!text.empty());
}

void StatusText::Clear()
{
    label_->SetText(L"");
    label_->SetRole(TextRole::Muted);
    SetVisible(false);
}

UINT_PTR SetTimeout(UINT ms, std::function<void()> callback)
{
    const UINT_PTR id = SetTimer(nullptr, 0, ms, &TimeoutProc);
    if (id)
        Timeouts()[id] = std::move(callback);
    return id;
}

void CancelTimeout(UINT_PTR id)
{
    if (!id)
        return;
    KillTimer(nullptr, id);
    Timeouts().erase(id);
}

CredentialTest::CredentialTest(PageContext &context, std::shared_ptr<PlainButton> button,
                               std::shared_ptr<StatusText> status, std::function<std::string()> service,
                               std::function<Json()> readConfig)
    : context_(&context), button_(std::move(button)), status_(std::move(status)), service_(std::move(service)),
      readConfig_(std::move(readConfig))
{
    idleLabel_ = button_->Text();
    button_->SetOnClick([this]() { Start(); });
    context.OnMessage("apiCredentialTestResult", [this](const Json &message) {
        if (pendingId_.empty() || message.value("requestId", std::string()) != pendingId_)
            return;
        const bool configMatches = pendingService_ == service_() && pendingSnapshot_ == readConfig_();
        Finish();
        if (!configMatches)
        {
            status_->Set(L"配置已变更，请重新测试", "");
            return;
        }
        status_->Set(Widen(message.value("message", std::string())), message.value("ok", false) ? "success" : "error");
    });
}

void CredentialTest::Start()
{
    if (!pendingId_.empty())
        return;
    pendingId_ = context_->model.NewRequestId("credential");
    pendingService_ = service_();
    pendingSnapshot_ = readConfig_();
    const std::string requestId = pendingId_;
    timer_ = SetTimeout(30000, [this, requestId]() {
        timer_ = 0;
        if (pendingId_ != requestId)
            return;
        Finish();
        status_->Set(L"测试超时，请重试", "error");
    });
    button_->SetDisabled(true);
    button_->SetText(L"测试中…");
    status_->Set(L"正在连接服务，请稍候…", "pending");
    context_->model.Post("apiCredentialTest",
                         Json{{"requestId", pendingId_}, {"service", pendingService_}, {"config", pendingSnapshot_}});
}

void CredentialTest::Finish()
{
    CancelTimeout(timer_);
    timer_ = 0;
    pendingId_.clear();
    button_->SetDisabled(false);
    button_->SetText(idleLabel_);
}

// ---------------------------------------------------------------------------------
// PlainExpander / MinHeight
// ---------------------------------------------------------------------------------
PlainExpander::PlainExpander(std::wstring text, TextStyle style)
{
    text_.Set(std::move(text), std::move(style));
}

void PlainExpander::SetOnToggled(std::function<void(bool expanded)> handler)
{
    onToggled_ = std::move(handler);
}

SizeF PlainExpander::Measure(const SizeF &availableSize)
{
    const SizeF text = text_.Measure(availableSize.width);
    return {std::min(text.width + 9.0f + 7.0f, availableSize.width), std::max(text.height, 10.0f)};
}

void PlainExpander::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void PlainExpander::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    const SizeF text = text_.Measure(bounds_.width);
    text_.Draw(dr, {bounds_.x, bounds_.y + (bounds_.height - text.height) * 0.5f}, p.text, bounds_.width);
    const double now = window_ ? window_->GetFrameTime() : msimeui::NowMs();
    const float t = rotation_.Value(now);
    if (rotation_.IsRunning(now) && window_)
        window_->RequestAnimationFrame();
    const PointF center = {bounds_.x + text.width + 9.0f + 3.5f, bounds_.y + bounds_.height * 0.5f - 1.5f + 3.0f * t};
    DrawChevron(dr, center, 7.0f, 45.0f + 180.0f * t, p.textMuted, 2.0f);
}

bool PlainExpander::OnMouseDown(const POINT &point, WPARAM)
{
    pressed_ = Hit(window_, *this, point);
    return pressed_;
}

bool PlainExpander::OnMouseUp(const POINT &point, WPARAM)
{
    if (!pressed_)
        return false;
    pressed_ = false;
    if (Hit(window_, *this, point))
    {
        expanded_ = !expanded_;
        rotation_.AnimateTo(expanded_ ? 1.0f : 0.0f, 160.0, msimeui::Easing::Ease(), msimeui::NowMs());
        if (window_)
            window_->RequestAnimationFrame();
        if (onToggled_)
        {
            const auto handler = onToggled_;
            handler(expanded_);
        }
    }
    return true;
}

HCURSOR PlainExpander::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

MinHeight::MinHeight(VisualPtr child, float minHeight, Thickness padding)
    : msimeui::Container(std::move(child)), minHeight_(minHeight)
{
    padding_ = padding;
}

SizeF MinHeight::Measure(const SizeF &availableSize)
{
    const SizeF size = msimeui::Container::Measure(availableSize);
    return {availableSize.width, std::max(size.height, minHeight_)};
}

void MinHeight::Arrange(const RectF &finalRect)
{
    // `align-items: center`: the child sits in the middle of the min-height box.
    bounds_ = finalRect;
    if (!child_)
    {
        return;
    }
    const RectF content = GetContentRect();
    const float height = std::min(content.height, child_->MeasureInLayout({content.width, content.height}).height);
    const float top = content.y + std::max(0.0f, (content.height - height) * 0.5f);
    child_->ArrangeInLayout({content.x, top, content.width, height});
}

// ---------------------------------------------------------------------------------
// Snapshot helpers
// ---------------------------------------------------------------------------------
std::string JsonString(const Json *value)
{
    return value && value->is_string() ? value->get<std::string>() : std::string();
}

std::map<std::string, std::string> ReadProviderMap(const SettingsModel &model, const std::string &section,
                                                   const std::string &base, const std::string &provider,
                                                   const std::vector<std::string> &providers, bool slots)
{
    std::map<std::string, std::string> result;
    if (const Json *map = model.Find(section + "." + base + "s"); map && map->is_object())
    {
        for (auto it = map->begin(); it != map->end(); ++it)
        {
            if (it.value().is_string())
                result[it.key()] = it.value().get<std::string>();
        }
    }
    const std::string legacy = model.GetString(section + "." + base);
    if (!legacy.empty() && result[provider].empty())
        result[provider] = legacy;
    if (slots)
    {
        for (const auto &name : providers)
        {
            const std::string slot = model.GetString(section + "." + base + "_" + name);
            if (!slot.empty())
                result[name] = slot;
        }
    }
    return result;
}

std::wstring Trim(const std::wstring &text)
{
    const size_t begin = text.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos)
        return {};
    const size_t end = text.find_last_not_of(L" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string AiModeOf(const std::string &provider)
{
    return provider == "jev" ? "select" : "generate";
}

std::string AiProviderForMode(const std::string &mode, const std::string &provider)
{
    if (mode == "select")
        return "jev";
    return provider == "jev" || provider.empty() ? "deepseek" : provider;
}

std::string AiTranslationProvider(const SettingsModel &model, const std::string &provider)
{
    if (provider != "jev")
        return provider;
    const std::string remembered = model.GetString("ai_assistant.generate_provider");
    return remembered.empty() ? std::string("deepseek") : remembered;
}

std::wstring AiDefaultEndpoint(const std::string &provider)
{
    return Widen(AiAssistant::ProviderStore::BuiltinDefaultEndpoint(provider));
}

std::wstring AiDefaultModel(const std::string &provider)
{
    return Widen(AiAssistant::ProviderStore::BuiltinDefaultModel(provider));
}

namespace
{
// 内置服务商的显示名。注册表里内置项的 name 就是 id，所以这里单独维护一份。
const std::map<std::string, std::wstring> &BuiltinAiNames()
{
    static const std::map<std::string, std::wstring> names{
        {"deepseek", L"DeepSeek"},       {"openai", L"OpenAI"},
        {"siliconflow", L"SiliconFlow"}, {"groq", L"Groq"},
        {"minimax", L"MiniMax"},         {"moonshot", L"Moonshot Kimi"},
        {"zhipu", L"智谱 GLM"},          {"dashscope", L"通义千问"},
        {"volcengine", L"火山方舟"},     {"openrouter", L"OpenRouter"},
        {"ollama", L"Ollama"},           {"custom", L"自定义接口"},
        {"jev", L"Jev (TypeSafe)"},
    };
    return names;
}

// 出厂默认值来自 ai_provider_store，配置层与设置页读同一份表。
std::string TrimAscii(std::string text)
{
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// 出厂模板里的占位符与假 token 都算没填。
bool IsPlaceholderToken(const std::string &token)
{
    if (token.empty())
        return true;
    if (token.front() == '<' && token.back() == '>')
        return true;
    return token.rfind("FAKESECRET_", 0) == 0;
}

const Json *FindEntry(const Json *providers, const std::string &provider, const char *field)
{
    if (!providers || !providers->is_array())
        return nullptr;
    for (const Json &entry : *providers)
    {
        if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string())
            continue;
        if (entry["id"].get<std::string>() != provider)
            continue;
        const auto found = entry.find(field);
        return found == entry.end() ? nullptr : &*found;
    }
    return nullptr;
}
} // namespace

std::wstring AiProviderLabel(const std::string &provider, const Json *providers)
{
    if (const Json *name = FindEntry(providers, provider, "name"); name && name->is_string())
    {
        const std::string text = name->get<std::string>();
        // 内置项的 name 是 id 本身，不算显示名。
        if (!text.empty() && text != provider)
            return Widen(text);
    }
    const auto known = BuiltinAiNames().find(provider);
    return known != BuiltinAiNames().end() ? known->second : Widen(provider);
}

std::vector<std::pair<std::string, std::wstring>> AiProviderChoices(const Json *providers, const std::string &mode)
{
    std::vector<std::pair<std::string, std::wstring>> choices;
    const auto skip = [&mode](const std::string &id) {
        // Jev 是另一种联想模式，不参与生成式服务商列表。
        return id == "jev" && mode == "generate";
    };
    for (const auto &[id, name] : BuiltinAiNames())
    {
        if (skip(id))
            continue;
        choices.emplace_back(id, name);
    }
    if (providers && providers->is_array())
    {
        for (const Json &entry : *providers)
        {
            if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string())
                continue;
            const std::string id = entry["id"].get<std::string>();
            if (id.empty() || skip(id) || BuiltinAiNames().count(id) != 0)
                continue;
            choices.emplace_back(id, AiProviderLabel(id, providers));
        }
    }
    return choices;
}

bool AiProviderIsJev(const std::string &provider)
{
    return provider == "jev";
}

ActiveAiConfig ReadActiveAiConfig(const SettingsModel &model)
{
    ActiveAiConfig config;
    const Json *providers = model.Find("ai_assistant.providers");
    // 翻译复用的是 Chat Completions 服务商。AI 联想处在选择器模式时，取生成式上次用的那家，
    // 与 PublishTranslationConfig 的取值口径保持一致。
    const std::string active = model.GetString("ai_assistant.provider", "deepseek");
    config.provider = AiTranslationProvider(model, active);

    const auto readSlot = [&](const std::string &base) {
        const Json *map = model.Find("ai_assistant." + base + "s");
        std::string value;
        if (map && map->is_object())
        {
            const auto found = map->find(config.provider);
            if (found != map->end() && found->is_string())
                value = found->get<std::string>();
        }
        if (value.empty())
        {
            // 用户自建的服务商把地址与模型放在注册表里。
            if (const Json *entry = FindEntry(providers, config.provider, base.c_str()); entry && entry->is_string())
                value = entry->get<std::string>();
        }
        if (value.empty() && active == config.provider)
            value = model.GetString("ai_assistant." + base);
        return TrimAscii(value);
    };

    config.endpoint = readSlot("endpoint");
    config.model = readSlot("model");
    if (config.endpoint.empty())
        config.endpoint = AiAssistant::ProviderStore::BuiltinDefaultEndpoint(config.provider);
    if (config.model.empty())
        config.model = AiAssistant::ProviderStore::BuiltinDefaultModel(config.provider);

    const Json *tokens = model.Find("ai_assistant.tokens");
    if (tokens && tokens->is_object())
    {
        const auto found = tokens->find(config.provider);
        if (found != tokens->end() && found->is_string())
            config.token = TrimAscii(found->get<std::string>());
    }
    // 与 ime_config.cpp 的加载顺序一致：单值 token 只给当前服务商兜底。别家的槽位空着就是没填，
    // 否则翻译页会把「实际发空 token」的配置判成可用。
    if (config.token.empty() && active == config.provider)
        config.token = TrimAscii(model.GetString("ai_assistant.token"));
    config.tokenSet = !IsPlaceholderToken(config.token);

    const bool scheme = config.endpoint.rfind("https://", 0) == 0 || config.endpoint.rfind("http://", 0) == 0;
    // 不用 Token 的服务商（Ollama 的本机服务）存没存、存的是什么都不影响它可不可用。
    const bool tokenOk = !AiAssistant::ProviderUsesToken(config.provider) || config.tokenSet;
    config.usable = config.provider != "jev" && tokenOk && !config.model.empty() && scheme;
    return config;
}
} // namespace native_settings::voice_fields
