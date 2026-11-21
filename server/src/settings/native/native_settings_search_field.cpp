#include "settings/native/native_settings_search_field.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Window.h"

#include <algorithm>

namespace native_settings
{
namespace
{
constexpr float kHeight = 32.0f;
constexpr float kRadius = 4.0f;
constexpr float kTextInset = 11.0f;
constexpr float kGlyphWidth = 32.0f;
constexpr float kClearSize = 24.0f;

// Segoe Fluent Icons code points.
constexpr wchar_t kGlyphSearch[] = L"\xE721";
constexpr wchar_t kGlyphClear[] = L"\xE894";

void DrawGlyph(msimeui::DeviceResources &dr, const wchar_t *glyph, float size, const RectF &box,
               const D2D1_COLOR_F &color)
{
    TextStyle style;
    style.family = IconFontFamily();
    style.size = size;
    style.lineHeight = 1.0f;
    DrawTextInRect(dr, glyph, style, box, color, true);
}
} // namespace

const FluentColors &Fluent(const Palette &p)
{
    static const FluentColors light = {
        Rgba(255, 255, 255, 0.70f), Rgba(249, 249, 249, 0.50f), Hex(0xFFFFFF),
        Rgba(0, 0, 0, 0.06f),       Rgba(0, 0, 0, 0.45f),       Rgba(0, 0, 0, 0.037f),
        Rgba(0, 0, 0, 0.024f),      Rgba(0, 0, 0, 0.62f),       Hex(0xF9F9F9),
        Rgba(0, 0, 0, 0.06f),
    };
    static const FluentColors dark = {
        Rgba(255, 255, 255, 0.06f), Rgba(255, 255, 255, 0.08f), Rgba(30, 30, 30, 0.70f),
        Rgba(255, 255, 255, 0.07f), Rgba(255, 255, 255, 0.54f), Rgba(255, 255, 255, 0.06f),
        Rgba(255, 255, 255, 0.04f), Rgba(255, 255, 255, 0.79f), Hex(0x2C2C2C),
        Rgba(255, 255, 255, 0.08f),
    };
    return p.light ? light : dark;
}

void HoverTextBox::SetOnHoverChanged(std::function<void(bool hovered)> handler)
{
    onHoverChanged_ = std::move(handler);
}

void HoverTextBox::OnMouseEnter()
{
    msimeui::TextBox::OnMouseEnter();
    if (onHoverChanged_)
        onHoverChanged_(true);
}

void HoverTextBox::OnMouseLeave()
{
    msimeui::TextBox::OnMouseLeave();
    if (onHoverChanged_)
        onHoverChanged_(false);
}

bool HoverTextBox::KeepsPopupsOpenOnClick() const
{
    return true;
}

SearchField::SearchField()
{
    box_ = std::make_shared<HoverTextBox>(kHeight, L"搜索设置");
    box_->SetChromeVisible(false);
    box_->SetFontSize(kFontBody);
    box_->SetPlaceholderFontSize(kFontBody);
    box_->SetOnHoverChanged([this](bool hovered) {
        boxHovered_ = hovered;
        InvalidateVisual();
    });
    box_->SetOnTextChanged([this](const std::wstring &text) {
        InvalidateArrange();
        if (onChanged_)
            onChanged_(text);
    });
    box_->SetOnFocusChanged([this](bool focused) {
        InvalidateArrange();
        InvalidateVisual();
        if (onFocusChanged_)
            onFocusChanged_(focused);
    });
    box_->SetOnSubmit([this]() {
        if (onSubmit_)
            onSubmit_();
    });
    AdoptChild(box_);
}

void SearchField::SetOnChanged(std::function<void(const std::wstring &)> handler)
{
    onChanged_ = std::move(handler);
}

void SearchField::SetOnSubmit(std::function<void()> handler)
{
    onSubmit_ = std::move(handler);
}

void SearchField::SetOnFocusChanged(std::function<void(bool focused)> handler)
{
    onFocusChanged_ = std::move(handler);
}

std::wstring SearchField::GetText() const
{
    return box_->GetText();
}

void SearchField::Clear()
{
    box_->SetText(L"");
    InvalidateArrange();
    if (onChanged_)
        onChanged_(std::wstring());
}

bool SearchField::IsFocused() const
{
    return box_->IsFocused();
}

void SearchField::Focus()
{
    if (window_)
        window_->FocusVisual(box_.get());
}

SizeF SearchField::Measure(const SizeF &availableSize)
{
    box_->MeasureInLayout({availableSize.width, kHeight});
    return {availableSize.width, kHeight};
}

void SearchField::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    const float right = ClearVisible() ? ClearRect().x : GlyphRect().x;
    box_->ArrangeInLayout({bounds_.x + kTextInset, bounds_.y, std::max(0.0f, right - bounds_.x - kTextInset), kHeight});
}

void SearchField::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    const FluentColors &c = Fluent(p);
    if (themedLight_ != (p.light ? 1 : 0))
    {
        themedLight_ = p.light ? 1 : 0;
        msimeui::TextBox::Appearance appearance;
        appearance.borderWidth = 0.0f;
        appearance.paddingX = 0.0f;
        appearance.paddingY = (kHeight - LineHeight(BodyStyle())) * 0.5f;
        appearance.fontFamily = UiFontFamily();
        appearance.text = p.text;
        appearance.placeholder = p.textMuted;
        appearance.selection = WithAlpha(p.accent, 0.45f);
        box_->SetAppearance(appearance);
    }

    const bool focused = box_->IsFocused();
    const bool hovered = boxHovered_ || selfHovered_;
    const D2D1_COLOR_F fill = focused ? c.fillFocused : hovered ? c.fillHover : c.fillRest;
    FillRounded(dr, bounds_, kRadius, fill);
    StrokeRounded(dr, bounds_, kRadius, c.stroke, CssBorder());
    // The bottom edge, inside the rounded outline so the corners curve up into it: one hairline
    // at rest, a two-pixel accent bar while focused.
    RenderBottomEdge(dr, focused ? p.accent : c.strokeBottom, focused ? CssBorder(2.0f) : CssBorder());

    box_->Render(dr);

    if (ClearVisible())
    {
        const RectF clear = ClearRect();
        if (clearPressed_ && clearHovered_)
            FillRounded(dr, clear, kRadius, c.subtlePressed);
        else if (clearHovered_)
            FillRounded(dr, clear, kRadius, c.subtleHover);
        DrawGlyph(dr, kGlyphClear, 10.0f, clear, c.glyph);
    }
    DrawGlyph(dr, kGlyphSearch, 12.0f, GlyphRect(), c.glyph);
}

void SearchField::Attach(msimeui::Window *window)
{
    msimeui::Visual::Attach(window);
    box_->Attach(window);
}

msimeui::Visual *SearchField::FindVisualAt(const PointF &point)
{
    if (!Contains(bounds_, point))
        return nullptr;
    if (box_->HitTest(point) && !(ClearVisible() && Contains(ClearRect(), point)))
        return box_.get();
    return this;
}

msimeui::Visual *SearchField::FindFocusableAt(const PointF &point)
{
    if (!Contains(bounds_, point))
        return nullptr;
    // The clear button keeps whatever focus there is; anywhere else in the field focuses the
    // text, like clicking the padding of an <input>. Returning this (not focusable) rather than
    // nullptr matters: the window blurs the focused visual on a click with no focus target, and
    // the button is only live while the text box is focused.
    if (ClearVisible() && Contains(ClearRect(), point))
        return this;
    return box_->HitTest(point) ? box_.get() : nullptr;
}

msimeui::Visual *SearchField::FindFirstFocusableDescendant()
{
    return box_.get();
}

bool SearchField::OnMouseDown(const POINT &point, WPARAM)
{
    if (!window_)
        return false;
    const PointF dip = window_->ClientPixelsToDips(point);
    if (ClearVisible() && Contains(ClearRect(), dip))
    {
        clearPressed_ = true;
        InvalidateVisual();
        return true;
    }
    Focus();
    return true;
}

bool SearchField::OnMouseUp(const POINT &point, WPARAM)
{
    if (!clearPressed_)
        return false;
    clearPressed_ = false;
    InvalidateVisual();
    if (window_ && ClearVisible() && Contains(ClearRect(), window_->ClientPixelsToDips(point)))
    {
        Clear();
        Focus();
    }
    return true;
}

bool SearchField::OnMouseMove(const POINT &point, WPARAM)
{
    const bool over = window_ && ClearVisible() && Contains(ClearRect(), window_->ClientPixelsToDips(point));
    if (over != clearHovered_)
    {
        clearHovered_ = over;
        InvalidateVisual();
    }
    return true;
}

void SearchField::OnMouseEnter()
{
    selfHovered_ = true;
    InvalidateVisual();
}

void SearchField::OnMouseLeave()
{
    selfHovered_ = false;
    clearHovered_ = false;
    clearPressed_ = false;
    InvalidateVisual();
}

HCURSOR SearchField::GetCursor() const
{
    return LoadCursor(nullptr, clearHovered_ ? IDC_ARROW : IDC_IBEAM);
}

bool SearchField::KeepsPopupsOpenOnClick() const
{
    return true;
}

bool SearchField::ClearVisible() const
{
    return box_->IsFocused() && !box_->GetText().empty();
}

RectF SearchField::GlyphRect() const
{
    return {bounds_.x + bounds_.width - kGlyphWidth, bounds_.y, kGlyphWidth, bounds_.height};
}

RectF SearchField::ClearRect() const
{
    const RectF glyph = GlyphRect();
    return {glyph.x - kClearSize + 4.0f, bounds_.y + (bounds_.height - kClearSize) * 0.5f, kClearSize, kClearSize};
}

void SearchField::RenderBottomEdge(msimeui::DeviceResources &dr, const D2D1_COLOR_F &color, float thickness)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    ID2D1SolidColorBrush *brush = target ? dr.GetSolidColorBrush(color) : nullptr;
    if (!brush)
        return;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> shape;
    if (!factory || FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(ToD2D(bounds_), kRadius, kRadius),
                                                                   shape.GetAddressOf())))
        return;
    target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.Get()), nullptr);
    const float bottom = bounds_.y + bounds_.height;
    target->FillRectangle(D2D1::RectF(bounds_.x, bottom - thickness, bounds_.x + bounds_.width, bottom), brush);
    target->PopLayer();
}
} // namespace native_settings
