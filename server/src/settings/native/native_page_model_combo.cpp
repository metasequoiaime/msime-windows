#include "settings/native/native_page_model_combo.h"

#include "settings/native/native_settings_controls.h"

#include <algorithm>

// 移植自旧网页设置页 `.dropdown-toggle.font-combobox.ai-model-combobox` 与
// `#aiModelMenu.font-dropdown-menu.compact-scroll-menu`。控件本身不知道当前是哪一页，
// 也不读配置：列表由调用方通过 SetModels 灌入，选中时通过 OnPicked 回调出去。
namespace native_settings::voice_fields
{
ModelList::ModelList(ModelCombo *owner) : owner_(owner)
{
}

void ModelList::SetItems(std::vector<std::wstring> items)
{
    items_ = std::move(items);
    hovered_ = kNone;
    pressed_ = kNone;
    scrollbar_.Reset();
    LayoutScrollbar();
    InvalidateMeasure();
}

bool ModelList::Empty() const
{
    return items_.empty();
}

float ModelList::ContentHeight() const
{
    const float rows = static_cast<float>(items_.size());
    return kInset * 2.0f + rows * kItem + std::max(rows - 1.0f, 0.0f) * kGap;
}

SizeF ModelList::Measure(const SizeF &)
{
    // 6 rows visible (border box 182px).
    const float visible = 6.0f * 28.0f + 5.0f * 2.0f + 2.0f * 2.0f - 2.0f;
    // No intrinsic width: the popup matches the combobox (max of this and the anchor).
    return {0.0f, std::min(ContentHeight(), visible)};
}

void ModelList::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    LayoutScrollbar();
}

void ModelList::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    target->PushAxisAlignedClip(ToD2D(bounds_), D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < items_.size(); ++i)
    {
        const RectF item = ItemRect(i);
        if (item.y + item.height < bounds_.y || item.y > bounds_.y + bounds_.height)
            continue;
        if (i == hovered_)
            FillRounded(dr, item, 8.0f, p.dropdownItemHover);
        // white-space: nowrap; text-overflow: ellipsis
        TextLayout text;
        TextStyle style = BodyStyle();
        text.Set(items_[i], style);
        const float width = item.width - 16.0f;
        IDWriteTextLayout *layout = text.Get(width);
        if (layout)
        {
            layout->SetMaxWidth(width);
            DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
            DWriteFactory()->CreateEllipsisTrimmingSign(layout, ellipsis.GetAddressOf());
            layout->SetTrimming(&trimming, ellipsis.Get());
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            if (ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(i == hovered_ ? p.text : p.dropdownItem))
                target->DrawTextLayout(D2D1::Point2F(item.x + 8.0f, item.y + (item.height - metrics.height) * 0.5f),
                                       layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    target->PopAxisAlignedClip();
    // This list's thumb does not light up under the pointer.
    scrollbar_.Render(dr, p.scrollbarThumb, p.scrollbarThumb);
}

// The list takes no keyboard focus, but a press on it must not reach a focusable
// field the popup covers: Window sends the press to whatever FindFocusableAt returns.
msimeui::Visual *ModelList::FindFocusableAt(const PointF &point)
{
    return HitTest(point) ? this : nullptr;
}

bool ModelList::OnMouseMove(const POINT &point, WPARAM keyState)
{
    if (!window_)
        return true;
    const PointF dip = window_->ClientPixelsToDips(point);
    bool changed = scrollbar_.Move(dip, keyState);
    if (!scrollbar_.IsDragging())
    {
        const size_t hit = scrollbar_.IsOnScrollbar(dip) ? kNone : HitRow(dip);
        changed = changed || hit != hovered_;
        hovered_ = hit;
    }
    if (changed)
        InvalidateVisual();
    return true;
}

void ModelList::OnMouseLeave()
{
    scrollbar_.Leave();
    hovered_ = kNone;
    InvalidateVisual();
}

bool ModelList::OnMouseDown(const POINT &point, WPARAM)
{
    pressed_ = kNone;
    if (!window_)
        return true;
    const PointF dip = window_->ClientPixelsToDips(point);
    if (scrollbar_.IsOnScrollbar(dip))
    {
        scrollbar_.Press(dip);
        hovered_ = kNone;
        InvalidateVisual();
        return true;
    }
    pressed_ = HitRow(dip);
    return true;
}

bool ModelList::OnMouseUp(const POINT &point, WPARAM)
{
    if (scrollbar_.IsDragging())
    {
        // The press belonged to the scrollbar, so releasing over a row picks nothing.
        scrollbar_.Release(window_ ? window_->ClientPixelsToDips(point) : PointF{});
        pressed_ = kNone;
        return true;
    }
    const size_t hit = window_ ? HitRow(window_->ClientPixelsToDips(point)) : kNone;
    if (hit != kNone && hit == pressed_)
        owner_->Pick(items_[hit]);
    pressed_ = kNone;
    return true;
}

bool ModelList::OnMouseWheel(const POINT &, short delta, WPARAM)
{
    if (scrollbar_.Wheel(delta))
        InvalidateVisual();
    return true;
}

RectF ModelList::ItemRect(size_t i) const
{
    const float scroll = scrollbar_.GetPlacedOffset(window_ ? window_->GetDpi() : 96.0f);
    return {bounds_.x + kInset, bounds_.y + kInset + static_cast<float>(i) * (kItem + kGap) - scroll,
            bounds_.width - kInset * 2.0f, kItem};
}

size_t ModelList::HitRow(const PointF &point) const
{
    if (!Contains(bounds_, point))
        return kNone;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (Contains(ItemRect(i), point))
            return i;
    }
    return kNone;
}

void ModelList::LayoutScrollbar()
{
    scrollbar_.Layout(ContentHeight(), bounds_.height, bounds_);
}

ModelPopup::ModelPopup(const std::shared_ptr<ModelList> &list) : msimeui::Popup(list), list_(list.get())
{
    SetPadding(1.0f);
    SetPlacement(msimeui::PopupPlacement::BelowLeading);
    SetCornerRadius(10.0f);
    SetShadowEnabled(true);
    SetConstrainToViewport(true);
    SetMatchAnchorWidth(true);
}

void ModelPopup::Render(msimeui::DeviceResources &dr)
{
    ScopedTextAntialias grayscale(dr, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    msimeui::Popup::Render(dr);
}

bool ModelPopup::OnMouseWheel(const POINT &point, short delta, WPARAM keyState)
{
    if (!window_ || !HitTest(window_->ClientPixelsToDips(point)))
        return false;
    list_->OnMouseWheel(point, delta, keyState);
    return true;
}

ModelCombo::ModelCombo()
{
    InputBox::Options options;
    options.bare = true;
    options.font = FieldTextStyle(kFontBody);
    input_ = std::make_shared<InputBox>(options);
    AdoptChild(input_);
    list_ = std::make_shared<ModelList>(this);
    popup_ = std::make_shared<ModelPopup>(list_);
}

void ModelCombo::SetModels(std::vector<std::wstring> models)
{
    Close();
    list_->SetItems(std::move(models));
}

bool ModelCombo::HasModels() const
{
    return !list_->Empty();
}

void ModelCombo::Open()
{
    msimeui::Scene *scene = window_ ? window_->GetScene() : nullptr;
    if (open_ || !scene || list_->Empty())
        return;
    scene->ClearPopups(true);
    const Palette &p = CurrentPalette();
    popup_->SetBackgroundFill(p.dropdownMenuBg);
    popup_->SetBorderColor(p.dropdownMenuBorder);
    popup_->SetTopHighlight(p.dropdownMenuHighlight);
    popup_->SetShadowPasses({{p.menuShadow[0].sigma, p.menuShadow[0].alpha, 0.0f, p.menuShadow[0].offsetY},
                             {p.menuShadow[1].sigma, p.menuShadow[1].alpha, 0.0f, p.menuShadow[1].offsetY}});
    popup_->SetAnchorRect(bounds_);
    popup_->SetOffset(0.0f, 2.0f);
    scene->AddPopup(popup_, [this]() {
        open_ = false;
        InvalidateVisual();
    });
    popup_->BeginAppearAnimation(140.0, -3.0f, 0.985f);
    open_ = true;
    window_->Invalidate();
}

void ModelCombo::Close()
{
    if (!open_)
        return;
    if (msimeui::Scene *scene = window_ ? window_->GetScene() : nullptr)
        scene->RemovePopup(popup_.get(), false);
    open_ = false;
    InvalidateVisual();
}

void ModelCombo::Pick(const std::wstring &model)
{
    Close();
    input_->SetText(model);
    if (onPicked_)
    {
        const auto handler = onPicked_;
        handler(model);
    }
}

SizeF ModelCombo::Measure(const SizeF &availableSize)
{
    input_->MeasureInLayout({std::max(availableSize.width - 12.0f - 6.0f - 18.0f - 2.0f, 0.0f), 40.0f});
    return {availableSize.width, 40.0f};
}

void ModelCombo::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    popup_->SetAnchorRect(bounds_);
    const float border = CssBorder();
    const float lineHeight = LineHeight(FieldTextStyle(kFontBody)) + 2.0f;
    input_->ArrangeInLayout({finalRect.x + border + 12.0f, finalRect.y + (finalRect.height - lineHeight) * 0.5f,
                             std::max(finalRect.width - border * 2.0f - 12.0f - 6.0f - 18.0f, 0.0f), lineHeight});
}

void ModelCombo::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    const bool focused = input_->IsFocused();
    // :focus-within ring and border, the dropdown look otherwise.
    if (focused || open_)
        FillRounded(dr, Inflate(bounds_, 2.0f), 10.0f, p.accentSoft);
    FillRounded(dr, bounds_, 8.0f, hovered_ || open_ ? p.surfaceRaised : p.dropdownBg);
    StrokeRounded(dr, bounds_, 8.0f, focused || open_ ? p.accentSoftBorder : (hovered_ ? p.borderStrong : p.border),
                  1.0f);
    input_->Render(dr);
    static const std::string arrow = LoadAssetSvg(L"arrow.svg");
    static const std::string lightArrow = InvertSvgColors(arrow, 0.25f);
    const RectF a = ArrowRect();
    DrawSvg(dr, p.light ? lightArrow : arrow, a);
}

void ModelCombo::Attach(msimeui::Window *window)
{
    msimeui::Visual::Attach(window);
    input_->Attach(window);
    popup_->Attach(window);
}

msimeui::Visual *ModelCombo::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
        return nullptr;
    if (Contains(ArrowRect(), point))
        return this;
    return input_->FindVisualAt(point) ? input_->FindVisualAt(point) : this;
}

msimeui::Visual *ModelCombo::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point) || Contains(ArrowRect(), point))
        return nullptr;
    return input_->FindFocusableAt(point);
}

bool ModelCombo::KeepsPopupsOpenOnClick() const
{
    return true;
}

bool ModelCombo::OnMouseDown(const POINT &point, WPARAM)
{
    pressed_ = window_ && HitTest(window_->ClientPixelsToDips(point));
    return pressed_;
}

bool ModelCombo::OnMouseUp(const POINT &point, WPARAM)
{
    if (!pressed_ || !window_)
        return false;
    pressed_ = false;
    const PointF dip = window_->ClientPixelsToDips(point);
    if (!HitTest(dip))
        return true;
    if (Contains(ArrowRect(), dip) && list_->Empty())
    {
        if (onFetch_)
        {
            // A handler may tear down the visual that owns onFetch_ while it runs.
            const auto handler = onFetch_;
            handler();
        }
        return true;
    }
    open_ ? Close() : Open();
    return true;
}

void ModelCombo::OnMouseEnter()
{
    hovered_ = true;
    InvalidateVisual();
}

void ModelCombo::OnMouseLeave()
{
    hovered_ = false;
    InvalidateVisual();
}

RectF ModelCombo::ArrowRect() const
{
    return {bounds_.x + bounds_.width - CssBorder() - 6.0f - 18.0f, bounds_.y + (bounds_.height - 18.0f) * 0.5f, 18.0f,
            18.0f};
}
} // namespace native_settings::voice_fields
