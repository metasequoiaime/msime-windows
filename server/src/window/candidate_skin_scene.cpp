#include "window/candidate_skin_scene.h"

#include "window/candidate_skin_layers.h"
#include "window/candidate_skin_overlay.h"

#include "msimeui/Window.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha)
{
    color.a *= alpha;
    return color;
}

std::wstring Widen(const std::string &text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>((std::max)(length, 0)), L'\0');
    if (length > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

// 皮肤阴影档位只缩放两层阴影的不透明度，几何不变；none 由调用方直接关掉阴影。
float ShadowAlphaScale(const std::string &shadow)
{
    if (shadow == "soft")
        return 0.5f;
    if (shadow == "strong")
        return 1.6f;
    return 1.0f;
}

msimeui::ImageStretch BackgroundStretch(const std::string &fit)
{
    if (fit == "contain")
        return msimeui::ImageStretch::Uniform;
    if (fit == "stretch")
        return msimeui::ImageStretch::Fill;
    return msimeui::ImageStretch::UniformToFill;
}

msimeui::HorizontalAlignment DecorationAlignment(const std::string &align)
{
    if (align == "left")
        return msimeui::HorizontalAlignment::Leading;
    if (align == "center")
        return msimeui::HorizontalAlignment::Center;
    return msimeui::HorizontalAlignment::Trailing;
}

std::vector<CandidateSkinOverlay::Item> OverlayItems(const std::vector<CandidateSkinCatalog::FreeDecoration> &layer,
                                                     const std::wstring &packageRoot)
{
    std::vector<CandidateSkinOverlay::Item> items;
    for (const CandidateSkinCatalog::FreeDecoration &dec : layer)
    {
        CandidateSkinOverlay::Item item;
        item.path = packageRoot + Widen(dec.image);
        item.anchor = dec.anchor;
        item.offsetXDip = static_cast<float>(dec.offsetXDip);
        item.offsetYDip = static_cast<float>(dec.offsetYDip);
        item.widthDip = static_cast<float>(dec.widthDip);
        item.heightDip = static_cast<float>(dec.heightDip);
        items.push_back(std::move(item));
    }
    return items;
}

msimeui::Thickness ToThickness(const CandidateSkinLayers::Edges &edges)
{
    return {static_cast<float>(edges.left), static_cast<float>(edges.top), static_cast<float>(edges.right),
            static_cast<float>(edges.bottom)};
}

// 横排且皮肤开了翻页箭头时的候选行：列表占去箭头以外的全部宽度，箭头贴在最右侧，
// 相当于在候选右边多出一列窄列；箭头的竖直位置由它自己的外边距对齐到最后一行。
class CandidateRowWithPager : public msimeui::Panel
{
  public:
    CandidateRowWithPager(std::shared_ptr<msimeui::Visual> list, std::shared_ptr<msimeui::Visual> pager)
        : list_(list.get()), pager_(pager.get())
    {
        AddChild(std::move(list));
        AddChild(std::move(pager));
    }

    msimeui::SizeF Measure(const msimeui::SizeF &availableSize) override
    {
        const msimeui::SizeF pager = pager_->MeasureInLayout(availableSize);
        const msimeui::SizeF list =
            list_->MeasureInLayout({(std::max)(availableSize.width - pager.width, 1.0f), availableSize.height});
        pagerWidth_ = pager.width;
        return {list.width + pager.width, (std::max)(list.height, pager.height)};
    }

    void Arrange(const msimeui::RectF &finalRect) override
    {
        bounds_ = finalRect;
        const float listWidth = (std::max)(finalRect.width - pagerWidth_, 0.0f);
        list_->ArrangeInLayout({finalRect.x, finalRect.y, listWidth, finalRect.height});
        pager_->ArrangeInLayout({finalRect.x + listWidth, finalRect.y, pagerWidth_, finalRect.height});
    }

    void Render(msimeui::DeviceResources &deviceResources) override
    {
        for (const auto &child : children_)
        {
            child->Render(deviceResources);
        }
    }

  private:
    msimeui::Visual *list_ = nullptr;
    msimeui::Visual *pager_ = nullptr;
    float pagerWidth_ = 0.0f;
};
} // namespace

// ---------------------------------------------------------------------------------
// CandidateExpandToggle
// ---------------------------------------------------------------------------------
void CandidateExpandToggle::SetAppearance(const Appearance &appearance)
{
    appearance_ = appearance;
    InvalidateMeasure();
    InvalidateVisual();
}

void CandidateExpandToggle::SetExpanded(bool expanded)
{
    if (expanded_ != expanded)
    {
        expanded_ = expanded;
        InvalidateVisual();
    }
}

void CandidateExpandToggle::SetOnClick(std::function<void()> handler)
{
    onClick_ = std::move(handler);
}

void CandidateExpandToggle::SetHoverEnabled(bool enabled)
{
    hoverEnabled_ = enabled;
    if (!enabled && hovered_)
    {
        hovered_ = false;
        InvalidateVisual();
    }
}

msimeui::SizeF CandidateExpandToggle::Measure(const msimeui::SizeF &)
{
    return {appearance_.buttonWidth, appearance_.buttonHeight};
}

void CandidateExpandToggle::Arrange(const msimeui::RectF &finalRect)
{
    bounds_ = finalRect;
}

void CandidateExpandToggle::Render(msimeui::DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    const msimeui::RectF box = Box();
    if (!target || box.width <= 0.0f || box.height <= 0.0f)
    {
        return;
    }
    if (pressed_ || hovered_)
    {
        if (ID2D1SolidColorBrush *fill =
                deviceResources.GetSolidColorBrush(pressed_ ? appearance_.pressedFill : appearance_.hoverFill))
        {
            target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(box.x, box.y, box.x + box.width, box.y + box.height),
                                  appearance_.cornerRadius, appearance_.cornerRadius),
                fill);
        }
    }
    ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(appearance_.glyphColor);
    if (!brush)
    {
        return;
    }
    const float halfWidth = (std::min)(appearance_.glyphWidth, box.width) * 0.5f;
    const float halfHeight = halfWidth * 0.5f;
    const float cx = box.x + box.width * 0.5f;
    const float cy = box.y + box.height * 0.5f;
    // 展开时尖朝上（∧），收起时尖朝下（∨）。
    const float tipY = expanded_ ? cy - halfHeight : cy + halfHeight;
    const float tailY = expanded_ ? cy + halfHeight : cy - halfHeight;
    target->DrawLine(D2D1::Point2F(cx - halfWidth, tailY), D2D1::Point2F(cx, tipY), brush, appearance_.strokeWidth);
    target->DrawLine(D2D1::Point2F(cx, tipY), D2D1::Point2F(cx + halfWidth, tailY), brush, appearance_.strokeWidth);
}

bool CandidateExpandToggle::HitTest(const msimeui::PointF &point) const
{
    const msimeui::RectF box = Box();
    return point.x >= box.x && point.x < box.x + box.width && point.y >= box.y && point.y < box.y + box.height;
}

bool CandidateExpandToggle::OnMouseDown(const POINT &point, WPARAM)
{
    if (!window_ || !HitTest(window_->ClientPixelsToDips(point)))
    {
        return false;
    }
    pressed_ = true;
    InvalidateVisual();
    return true;
}

bool CandidateExpandToggle::OnMouseUp(const POINT &point, WPARAM)
{
    if (!window_ || !pressed_)
    {
        return false;
    }
    pressed_ = false;
    InvalidateVisual();
    if (HitTest(window_->ClientPixelsToDips(point)) && onClick_)
    {
        // A handler may tear down the visual that owns onClick_ while it runs.
        const auto handler = onClick_;
        handler();
    }
    return true;
}

bool CandidateExpandToggle::OnMouseMove(const POINT &point, WPARAM)
{
    if (!window_ || !hoverEnabled_)
    {
        return false;
    }
    const bool hovered = HitTest(window_->ClientPixelsToDips(point));
    if (hovered_ != hovered)
    {
        hovered_ = hovered;
        InvalidateVisual();
    }
    return hovered;
}

void CandidateExpandToggle::OnMouseLeave()
{
    if (hovered_)
    {
        hovered_ = false;
        InvalidateVisual();
    }
}

HCURSOR CandidateExpandToggle::GetCursor() const
{
    return LoadCursor(nullptr, IDC_ARROW);
}

msimeui::RectF CandidateExpandToggle::Box() const
{
    const float width = (std::min)(appearance_.buttonWidth, bounds_.width);
    const float height = (std::min)(appearance_.buttonHeight, bounds_.height);
    return {bounds_.x + bounds_.width - width, bounds_.y, width, height};
}

// ---------------------------------------------------------------------------------
// CandidateExpandContent
// ---------------------------------------------------------------------------------
CandidateExpandContent::CandidateExpandContent(std::shared_ptr<msimeui::CandidateList> list,
                                               std::shared_ptr<msimeui::CandidateList> grid)
    : list_(list.get()), grid_(grid.get())
{
    AddChild(std::move(list));
    AddChild(std::move(grid));
}

void CandidateExpandContent::SetShowGrid(bool show)
{
    if (showGrid_ != show)
    {
        showGrid_ = show;
        InvalidateMeasure();
    }
}

bool CandidateExpandContent::ShowsGrid() const
{
    return showGrid_;
}

void CandidateExpandContent::SetGap(float gap)
{
    gap_ = gap;
    InvalidateMeasure();
}

float CandidateExpandContent::GetGap() const
{
    return gap_;
}

float CandidateExpandContent::GetListExtent() const
{
    return listExtent_;
}

msimeui::SizeF CandidateExpandContent::Measure(const msimeui::SizeF &availableSize)
{
    const msimeui::SizeF list = list_->MeasureInLayout({availableSize.width, 1000000.0f});
    listExtent_ = list.height;
    gridExtent_ = showGrid_ ? grid_->MeasureInLayout({availableSize.width, 1000000.0f}).height : 0.0f;
    return {list.width, showGrid_ ? listExtent_ + gap_ + gridExtent_ : listExtent_};
}

void CandidateExpandContent::Arrange(const msimeui::RectF &finalRect)
{
    bounds_ = finalRect;
    list_->ArrangeInLayout({finalRect.x, finalRect.y, finalRect.width, listExtent_});
    grid_->ArrangeInLayout({finalRect.x, finalRect.y + listExtent_ + (showGrid_ ? gap_ : 0.0f), finalRect.width,
                            showGrid_ ? gridExtent_ : 0.0f});
}

void CandidateExpandContent::Render(msimeui::DeviceResources &deviceResources)
{
    list_->Render(deviceResources);
    if (showGrid_)
    {
        grid_->Render(deviceResources);
    }
}

// ---------------------------------------------------------------------------------
// CandidateExpandBody
// ---------------------------------------------------------------------------------
CandidateExpandBody::CandidateExpandBody(std::shared_ptr<msimeui::ScrollViewer> scroller,
                                         std::shared_ptr<CandidateExpandContent> content,
                                         std::shared_ptr<msimeui::Visual> toggle)
    : scroller_(scroller.get()), content_(content.get()), toggle_(toggle.get())
{
    AddChild(std::move(scroller));
    AddChild(std::move(toggle));
}

void CandidateExpandBody::SetGeometry(int visibleRows, float rowHeight, float rowGap, float scrollbarReserve)
{
    visibleRows_ = (std::max)(visibleRows, 1);
    rowHeight_ = rowHeight;
    rowGap_ = rowGap;
    scrollbarReserve_ = scrollbarReserve;
    InvalidateMeasure();
}

msimeui::SizeF CandidateExpandBody::Measure(const msimeui::SizeF &availableSize)
{
    const msimeui::SizeF toggle = toggle_->MeasureInLayout(availableSize);
    toggleWidth_ = toggle.width;
    const float available = (std::max)(availableSize.width - toggle.width, 1.0f);
    const msimeui::SizeF natural = content_->MeasureInLayout({available, 1000000.0f});
    const float listExtent = content_->GetListExtent();
    float width = natural.width;
    viewport_ = listExtent;
    if (content_->ShowsGrid())
    {
        const float rows = static_cast<float>(visibleRows_);
        const float cap = listExtent + content_->GetGap() + rows * rowHeight_ + (rows - 1.0f) * rowGap_;
        viewport_ = (std::min)({natural.height, cap, availableSize.height});
        if (natural.height > viewport_ + 0.5f)
        {
            width = (std::min)(width + scrollbarReserve_, available);
        }
    }
    scroller_->MeasureInLayout({width, viewport_});
    return {width + toggle.width, (std::max)(viewport_, toggle.height)};
}

void CandidateExpandBody::Arrange(const msimeui::RectF &finalRect)
{
    bounds_ = finalRect;
    const float scrollerWidth = (std::max)(finalRect.width - toggleWidth_, 0.0f);
    // 卡片保留了更宽的尺寸时内容按最终宽度重排，滚动区按同一宽度重新量内容高度。
    scroller_->MeasureInLayout({scrollerWidth, viewport_});
    scroller_->ArrangeInLayout({finalRect.x, finalRect.y, scrollerWidth, viewport_});
    toggle_->ArrangeInLayout({finalRect.x + scrollerWidth, finalRect.y, toggleWidth_, finalRect.height});
}

void CandidateExpandBody::Render(msimeui::DeviceResources &deviceResources)
{
    scroller_->Render(deviceResources);
    toggle_->Render(deviceResources);
}

// ---------------------------------------------------------------------------------
// CandidateSkinScene
// ---------------------------------------------------------------------------------
CandidateSkinScene::CandidateSkinScene(Callbacks callbacks) : callbacks_(std::move(callbacks))
{
}

void CandidateSkinScene::Build(bool horizontal, bool pageArrows, bool expandPanel)
{
    built_ = true;
    horizontal_ = horizontal;
    pageArrows_ = pageArrows;
    expandPanel_ = expandPanel;
    root_ = std::make_shared<msimeui::StackPanel>(0.0f);
    decoration_ = std::make_shared<msimeui::Image>(L"");
    decoration_->SetStretch(msimeui::ImageStretch::Uniform);
    decorationWord_ = std::make_shared<msimeui::TextBlock>(L"", 16.0f, D2D1::ColorF(D2D1::ColorF::White));
    decorationWord_->SetTextLayoutPadding({0.0f, 0.0f, 0.0f, 0.0f});
    decorationBox_ = std::make_shared<msimeui::OverlayPanel>();
    decorationBox_->SetHorizontalAlignment(msimeui::HorizontalAlignment::Trailing);
    decorationBox_->AddChildAt(decoration_, 0.0f, 0.0f);
    decorationBox_->AddChildAt(decorationWord_, 0.0f, 0.0f);
    decorationWordShown_.clear();
    preedit_ = std::make_shared<msimeui::TextBlock>(L"", 14.0f, D2D1::ColorF(0xF5F5F5));
    preedit_->SetTextLayoutPadding({0.0f, 1.0f, 0.0f, 1.0f});
    preedit_->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
    list_ = std::make_shared<msimeui::CandidateList>(28.0f);
    list_->SetOnItemActivated([this](size_t index) {
        if (callbacks_.itemActivated)
            callbacks_.itemActivated(index);
    });
    list_->SetOnContextMenu([this](size_t index, const POINT &clientPoint) {
        if (callbacks_.itemContextMenu)
            callbacks_.itemContextMenu(index, clientPoint);
    });
    body_ = std::make_shared<msimeui::StackPanel>(kCandidateBodyRowSpacingDip);
    body_->SetPadding({0.0f, 0.0f, 0.0f, 0.0f});
    body_->AddChild(preedit_);
    pager_.reset();
    expandToggle_.reset();
    expandGrid_.reset();
    expandContent_.reset();
    expandScroller_.reset();
    expandBody_.reset();
    if (expandPanel)
    {
        expandToggle_ = std::make_shared<CandidateExpandToggle>();
        expandToggle_->SetOnClick([this]() {
            if (callbacks_.expandToggle)
                callbacks_.expandToggle();
        });
        expandGrid_ = std::make_shared<msimeui::CandidateList>(28.0f);
        expandGrid_->SetOrientation(msimeui::CandidateList::Orientation::Horizontal);
        expandGrid_->SetOnItemActivated([this](size_t index) {
            if (callbacks_.gridItemActivated)
                callbacks_.gridItemActivated(index);
        });
        expandContent_ = std::make_shared<CandidateExpandContent>(list_, expandGrid_);
        expandScroller_ = std::make_shared<msimeui::ScrollViewer>(expandContent_);
        expandBody_ = std::make_shared<CandidateExpandBody>(expandScroller_, expandContent_, expandToggle_);
        body_->AddChild(expandBody_);
    }
    else if (pageArrows)
    {
        pager_ = std::make_shared<msimeui::PagerArrows>();
        // 箭头在自己的槽位里贴右下角，槽位铺满即可（竖排是整行宽，横排是列表的整个高度）。
        pager_->SetVerticalAlignment(msimeui::VerticalAlignment::Stretch);
        pager_->SetOnClick([this](msimeui::PagerArrows::Part part) {
            if (callbacks_.pageArrow)
                callbacks_.pageArrow(part == msimeui::PagerArrows::Part::Next);
        });
    }
    if (expandBody_)
    {
        // 候选行已经放在展开区里了。
    }
    else if (pager_ && horizontal)
    {
        body_->AddChild(std::make_shared<CandidateRowWithPager>(list_, pager_));
    }
    else
    {
        // 竖排时箭头是列表下方单独的一矮行，靠右。
        body_->AddChild(list_);
        if (pager_)
            body_->AddChild(pager_);
    }
    msimeui::Brush brush;
    brush.fill = D2D1::ColorF(0x202020);
    brush.stroke = D2D1::ColorF(0x9B9B9B, 0x2E / 255.0f);
    brush.strokeWidth = 1.5f;
    brush.radiusX = 6.0f;
    brush.radiusY = 6.0f;
    card_ = std::make_shared<msimeui::Card>(brush, 5.0f);
    card_->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
    card_->AddChild(body_);
    root_->SetHorizontalContentAlignment(msimeui::HorizontalAlignment::Leading);
    overlayGrid_ = std::make_shared<msimeui::Grid>();
    overlayGrid_->AddRow(msimeui::GridLength{msimeui::GridUnitType::Auto, 1.0f});
    overlayGrid_->AddColumn(msimeui::GridLength{msimeui::GridUnitType::Auto, 1.0f});
    for (auto *overlay : {&skinOverlayBack_, &skinOverlayFront_})
    {
        *overlay = std::make_shared<CandidateSkinOverlay>();
        (*overlay)->SetHorizontalAlignment(msimeui::HorizontalAlignment::Stretch);
        (*overlay)->SetVerticalAlignment(msimeui::VerticalAlignment::Stretch);
    }
    overlayGrid_->AddChild(skinOverlayBack_, 0, 0);
    overlayGrid_->AddChild(card_, 0, 0);
    overlayGrid_->AddChild(skinOverlayFront_, 0, 0);
    cardColumn_ = std::make_shared<msimeui::StackPanel>(0.0f);
    cardColumn_->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
    cardColumn_->SetHorizontalContentAlignment(msimeui::HorizontalAlignment::Stretch);
    cardColumn_->AddChild(decorationBox_);
    cardColumn_->AddChild(overlayGrid_);
    frame_ = std::make_shared<msimeui::Container>();
    frame_->SetChild(cardColumn_);
    root_->AddChild(frame_);
}

CandidateSkinScene::Result CandidateSkinScene::Apply(const Settings &settings)
{
    Result result;
    const std::filesystem::path &skinsRoot = settings.skinsRoot;
    const std::string &skinId = settings.skinId;
    const bool candLight = settings.light;
    const bool horizontalLayout = settings.horizontal;
    // 外部皮肤用声明支持当前布局与主题的那一份；不支持或读不出来就回退 fluent。
    std::optional<CandidateSkinCatalog::Package> package;
    if (!CandidateSkinCatalog::IsBuiltIn(skinId))
    {
        package = CandidateSkinCatalog::LoadSupported(skinsRoot, skinId, horizontalLayout ? "horizontal" : "vertical",
                                                      candLight ? "light" : "dark");
    }
    const CandidateSkinCatalog::Package *packagePtr = package ? &*package : nullptr;
    // 自定义包的外观取自它的 base 皮肤；随后由包覆盖自己写了的键。
    const std::string baseSkin =
        package ? package->base : (CandidateSkinCatalog::IsBuiltIn(skinId) ? skinId : std::string("fluent"));
    // 翻页箭头与卷轴展开改变的是场景结构（竖排多一行、横排多一列或一块展开区），开关或排列方式变了就整棵重建，
    // 再往下套用配色与尺寸。卷轴展开只做横排；开了它就不再放翻页箭头。
    const bool expandPanel =
        horizontalLayout && CandidateSkinCatalog::ResolveExpandPanel(skinsRoot, skinId, packagePtr);
    const bool pageArrows = !expandPanel && CandidateSkinCatalog::ResolvePageArrows(skinsRoot, skinId, packagePtr);
    if (!built_ || pageArrows != pageArrows_ || horizontalLayout != horizontal_ || expandPanel != expandPanel_)
    {
        Build(horizontalLayout, pageArrows, expandPanel);
        result.rebuilt = true;
    }
    const bool fitContent =
        settings.fitContent || CandidateSkinCatalog::ResolveFitContent(skinsRoot, skinId, packagePtr);
    result.fitContent = fitContent;
    const float fontSize = settings.fontSize;
    // 基底的外观来自它的皮肤清单（出厂清单叠上数据目录里那份），外部皮肤再覆盖自己写了的键。
    const CandidateSkinCatalog::Package baseLook = CandidateSkinCatalog::ResolveBuiltInLook(skinsRoot, baseSkin);
    result.expandRows = package && package->expandRows
                            ? *package->expandRows
                            : baseLook.expandRows.value_or(CandidateSkinCatalog::kDefaultExpandRows);
    CandSkinTokens tokens = ResolveCandidateSkinTokens(baseLook, packagePtr, candLight, horizontalLayout, fontSize);
    const CandidateSkinCatalog::CandidateColors *packageColors =
        package ? &(candLight ? package->light : package->dark) : nullptr;
    const CandidateSkinPalette palette =
        ResolveCandidateSkinPalette(baseLook, candLight, settings.textColor, packageColors);
    tokens.surface = palette.surface;
    tokens.border = palette.border;
    tokens.text = palette.text;
    // v2 面板贴图：有它时卡片自己的底色、描边、圆角和阴影都让给贴图（同 CSS border-image）。
    const bool panelImage =
        package && package->schemaVersion >= 2 && package->borderImage && !package->borderImage->source.empty();
    result.skinAnimates = package && package->schemaVersion >= 2;
    // 面板贴图盖住整张卡片，垫系统材质没有意义。
    result.wantsBackdrop = package && !package->backdrop.empty() && package->backdrop != "none" && !panelImage;
    result.backdropActive = result.wantsBackdrop && settings.backdropAvailable;
    if (result.backdropActive)
    {
        // 色调由合成层按 Fluent 材质配方画（模糊 → 亮度层 → 色调），D2D 卡片底色此时留空。
        const bool mica = package->backdrop == "mica";
        const CandidateSkinCatalog::CandidateColors &themeColors = candLight ? package->light : package->dark;
        msimeui::BackdropMaterial &material = result.backdropMaterial;
        material.source =
            mica ? msimeui::BackdropMaterial::Source::Wallpaper : msimeui::BackdropMaterial::Source::WindowsBehind;
        material.tint = tokens.surface;
        material.tintOpacity = static_cast<float>(themeColors.backdropOpacity.value_or(tokens.surface.a));
        material.luminosityOpacity = static_cast<float>(themeColors.backdropLuminosity.value_or(mica ? 1.0 : 0.85));
    }
    {
        // 阴影跟皮肤走：light `8px 10px 24px rgba(0,0,0,.18), 2px 3px 8px rgba(0,0,0,.10)`，dark α .34/.22；
        // σ = blur ÷ 2。外部皮肤没写阴影就用基底清单的（canvas 的是 none，皮肤写了 soft / strong 才有）；
        // 面板贴图在时一律不画。
        const std::string shadow = ResolveCandidateSkinShadow(baseLook, packagePtr);
        if (shadow != "none" && !panelImage)
        {
            const float scale = ShadowAlphaScale(shadow);
            const auto alpha = [scale](float value) { return (std::min)(value * scale, 1.0f); };
            card_->SetShadowEnabled(true);
            card_->SetShadowPasses({
                {12.0f, alpha(candLight ? 0.18f : 0.34f), 8.0f, 10.0f},
                {4.0f, alpha(candLight ? 0.10f : 0.22f), 2.0f, 3.0f},
            });
            // 卡片底色半透明时阴影会透上来把毛玻璃压暗，只画在卡片外面。
            card_->SetShadowOutsideOnly(result.backdropActive);
        }
        else
        {
            card_->SetShadowEnabled(false);
        }
    }

    msimeui::CandidateList::Appearance appearance;
    appearance.fontFamily = settings.fontFamily;
    // 皮肤的 font_family 排在最前，用户配置的字体整体退为回退。
    if (package && !package->fontFamily.empty())
    {
        appearance.fallbackFontFamilies.push_back(appearance.fontFamily);
        appearance.fontFamily =
            settings.resolveFontFamily ? settings.resolveFontFamily(package->fontFamily) : Widen(package->fontFamily);
    }
    for (const auto &font : settings.fallbackFontFamilies)
        appearance.fallbackFontFamilies.push_back(font);
    appearance.itemHeight = fontSize * 1.35f + tokens.itemExtraHeight;
    appearance.itemGap = tokens.itemGap;
    appearance.fontSize = fontSize;
    appearance.labelFontSize = fontSize * tokens.labelScale;
    appearance.annotationFontSize = fontSize;
    appearance.contentPadLeft = tokens.itemPadLeft;
    appearance.contentPadRight = tokens.itemPadRight;
    // itemExtraHeight 上下各半：横排翻译换到第二行时从下半段内边距上方开始，内边距留在翻译下面。
    appearance.contentPadBottom = tokens.itemExtraHeight * 0.5f;
    appearance.textPadLeft = kCandidateNumberTextPad;
    appearance.labelGap = tokens.labelGap;
    appearance.minItemWidth = tokens.minItemWidth;
    // 收紧宽度时右侧留白与序号左侧的留白相同，卡片两边对称。
    appearance.fitContent = fitContent;
    if (fitContent)
        appearance.contentPadRight = appearance.contentPadLeft + appearance.textPadLeft;
    appearance.selectedBarWidth = tokens.selectedBarWidth;
    appearance.selectedBarHeight = fontSize * tokens.selectedBarHeightEm;
    appearance.selectedBarInside = tokens.selectedBarInside;
    appearance.showSelectedBar = tokens.showSelectedBar;
    appearance.selectedBarColor = tokens.selectedBar.a > 0.001f ? tokens.selectedBar : tokens.accent;
    appearance.cornerRadius = tokens.itemRadius;
    // 预编辑行隐藏时列表顶边才贴着卡片顶边。
    const bool preeditHidden = settings.preeditHidden;
    appearance.outerCornerRadius = tokens.outerItemRadius;
    appearance.outerTopCornersEnabled = preeditHidden;
    appearance.justifyHorizontalRows = tokens.justifyRows;
    // 隐藏的预编辑行高度为 0，但 body 仍在它和列表之间留 2px 行距；贴角皮肤把它抵掉，
    // 让首项贴着卡片的内容顶边。
    if (preeditHidden)
    {
        preedit_->SetMargin(
            {0.0f, 0.0f, 0.0f,
             tokens.outerItemRadius > 0.0f || tokens.trimHiddenPreeditGap ? -kCandidateBodyRowSpacingDip : 0.0f});
    }
    else
    {
        msimeui::Thickness preeditMargin{tokens.preeditMargin.left, tokens.preeditMargin.top,
                                         tokens.preeditMargin.right, tokens.preeditMargin.bottom};
        // v2 的 line_gap_dip 只是预编辑行与候选之间的距离，列表与翻页箭头之间仍是 body 的 2 DIP 行距，
        // 所以落在预编辑行的下外边距上，再抵掉那 2 DIP。
        if (package && package->schemaVersion >= 2 && package->lineGapDip)
            preeditMargin.bottom = static_cast<float>(*package->lineGapDip) - kCandidateBodyRowSpacingDip;
        preedit_->SetMargin(preeditMargin);
    }
    appearance.textColor = palette.candidateText;
    appearance.labelColor = tokens.number;
    appearance.annotationColor = palette.candidateText;
    appearance.translationColor = tokens.translation;
    appearance.rowTranslationSelected = tokens.rowTranslationSelected;
    appearance.translationSecondaryColor = tokens.translationPos;
    appearance.translationDelimiterColor = tokens.translationSeparator;
    appearance.rowTranslationSecondarySelected = tokens.rowTranslationPosSelected;
    appearance.rowTranslationDelimiterSelected = tokens.rowTranslationSeparatorSelected;
    appearance.rowFillSelected = tokens.selected;
    appearance.rowFillHover = tokens.hover;
    appearance.rowFillPressed = tokens.selected;
    appearance.rowTextSelected = tokens.rowTextSelected;
    appearance.rowLabelSelected = tokens.rowLabelSelected;
    list_->SetAppearance(appearance);
    list_->SetOrientation(horizontalLayout ? msimeui::CandidateList::Orientation::Horizontal
                                           : msimeui::CandidateList::Orientation::Vertical);
    if (pager_)
    {
        // 箭头跟着候选字号缩放，与序号差不多大；颜色用序号色，悬停底色沿用候选项的悬停色。
        msimeui::PagerArrows::Appearance pagerAppearance;
        pagerAppearance.buttonWidth = std::round(fontSize * 0.72f);
        pagerAppearance.buttonHeight = std::round(fontSize * 0.9f);
        pagerAppearance.gap = 1.0f;
        pagerAppearance.glyphSize = fontSize * 0.5f;
        pagerAppearance.strokeWidth = (std::max)(1.2f, fontSize / 13.0f);
        pagerAppearance.cornerRadius = (std::min)(3.0f, tokens.itemRadius);
        pagerAppearance.glyphColor = tokens.number;
        pagerAppearance.disabledGlyphColor = WithAlpha(tokens.number, 0.3f);
        pagerAppearance.hoverFill = tokens.hover;
        pagerAppearance.pressedFill = tokens.selected.a > 0.001f ? tokens.selected : tokens.hover;
        if (tokens.trianglePager)
        {
            // 微软拼音式：候选文字色的实心三角，按钮与候选行等高。横排时按钮更宽，前面隔一条分隔线。
            pagerAppearance.glyph = msimeui::PagerArrows::Glyph::Triangle;
            // 高 0.52em、宽 0.44em，三个角各圆掉高度的八分之三，尖端是钝的。
            pagerAppearance.glyphSize = fontSize * 0.52f;
            pagerAppearance.strokeWidth = pagerAppearance.glyphSize * 0.375f;
            pagerAppearance.cornerRadius = tokens.itemRadius;
            pagerAppearance.gap = 0.0f;
            pagerAppearance.glyphColor = palette.candidateText;
            pagerAppearance.disabledGlyphColor = WithAlpha(palette.candidateText, 0.25f);
            pagerAppearance.buttonHeight = std::round(appearance.itemHeight);
            if (horizontalLayout)
            {
                pagerAppearance.buttonWidth = std::round(fontSize * 0.96f);
                pagerAppearance.dividerWidth = 1.0f;
                pagerAppearance.dividerGap = std::round(fontSize * 0.12f);
                pagerAppearance.dividerColor = tokens.pagerDivider;
            }
            else
            {
                pagerAppearance.buttonWidth = std::round(fontSize * 0.81f);
                pagerAppearance.buttonHeight = std::round(fontSize * 1.2f);
            }
        }
        pager_->SetAppearance(pagerAppearance);
        if (horizontalLayout && tokens.trianglePager)
        {
            // 与最后一项隔 0.3em，按钮与行等高，不用再抬。
            pager_->SetMargin({std::round(fontSize * 0.3f), 0.0f, 0.0f, 0.0f});
        }
        else if (horizontalLayout)
        {
            // 横排：窄列与候选之间留一点空，箭头竖直居中在最后一行上。
            const float lift = (std::max)(0.0f, (appearance.itemHeight - pagerAppearance.buttonHeight) * 0.5f);
            pager_->SetMargin({2.0f, 0.0f, 1.0f, lift});
        }
        else
        {
            // 竖排：矮行抵掉 body 的 2px 行距，贴着最后一个候选、靠左，“‹”的左端与序号左端对齐。
            // 序号从候选行的 contentPadLeft + textPadLeft 处开始；箭头在按钮里居中，要减去它左侧的留白。
            pager_->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
            const float labelLeft = appearance.contentPadLeft + appearance.textPadLeft;
            const float glyphInset = (pagerAppearance.buttonWidth - pagerAppearance.GlyphWidth()) * 0.5f;
            pager_->SetMargin({labelLeft - glyphInset, -1.0f, 0.0f, 1.0f});
        }
    }
    if (expandBody_)
    {
        // 展开按钮与翻页箭头同样跟着候选字号缩放、用序号色，竖直居中在首行上。
        CandidateExpandToggle::Appearance toggleAppearance;
        toggleAppearance.buttonWidth = std::round(fontSize * 1.1f);
        toggleAppearance.buttonHeight = std::round(fontSize * 1.1f);
        toggleAppearance.glyphWidth = std::round(fontSize * 0.6f);
        toggleAppearance.strokeWidth = (std::max)(1.2f, fontSize / 13.0f);
        toggleAppearance.cornerRadius = (std::min)(4.0f, tokens.itemRadius);
        toggleAppearance.glyphColor = tokens.number;
        toggleAppearance.hoverFill = tokens.hover;
        toggleAppearance.pressedFill = tokens.selected.a > 0.001f ? tokens.selected : tokens.hover;
        expandToggle_->SetAppearance(toggleAppearance);
        expandToggle_->SetMargin(
            {2.0f, (std::max)(0.0f, (appearance.itemHeight - toggleAppearance.buttonHeight) * 0.5f), 1.0f, 0.0f});

        // 网格沿用首行的字体与行高，但没有序号、没有选中态，按等宽列排开；点中即上屏。
        msimeui::CandidateList::Appearance gridAppearance = appearance;
        // 网格没有选中项：列表内部的选中下标总落在某一格上，不关掉的话那一格（通常是第一格）
        // 永远按选中态画，悬停高亮就出不来。
        gridAppearance.selectable = false;
        gridAppearance.rowFillPressed = tokens.hover;
        gridAppearance.showSelectedBar = false;
        gridAppearance.outerCornerRadius = 0.0f;
        gridAppearance.justifyHorizontalRows = false;
        gridAppearance.fitContent = false;
        gridAppearance.minItemWidth = std::round(fontSize * 2.6f);
        gridAppearance.gridColumns = (std::max)(1, settings.pageSize);
        expandGrid_->SetAppearance(gridAppearance);

        // 细滚动条：只画滑块，不画轨道；命中区比滑块宽，好抓。
        msimeui::ScrollViewer::Appearance scrollAppearance;
        scrollAppearance.reservedWidth = 10.0f;
        scrollAppearance.trackWidth = 10.0f;
        scrollAppearance.thumbWidth = 4.0f;
        scrollAppearance.thumbInset = 2.0f;
        scrollAppearance.thumbRightInset = 2.0f;
        scrollAppearance.thumbMinLength = 20.0f;
        scrollAppearance.cornerRadius = 2.0f;
        scrollAppearance.drawTrack = false;
        scrollAppearance.thumbColor = WithAlpha(tokens.number, 0.45f);
        scrollAppearance.thumbHoverColor = WithAlpha(tokens.number, 0.85f);
        expandScroller_->SetAppearance(scrollAppearance);

        expandContent_->SetGap(appearance.itemGap);
        expandBody_->SetGeometry(result.expandRows, appearance.itemHeight, appearance.itemGap,
                                 scrollAppearance.reservedWidth);
        result.expandRowStepDip = appearance.itemHeight + appearance.itemGap;
    }
    preedit_->SetFontFamily(appearance.fontFamily);
    preedit_->SetFallbackFontFamilies(appearance.fallbackFontFamilies);
    preedit_->SetFontSize(settings.preeditFontSize);
    preedit_->SetColor(palette.preeditText);
    preedit_->SetCaretColor(tokens.preeditCaret.a > 0.001f ? tokens.preeditCaret : tokens.accent);
    preedit_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    preedit_->SetTextLayoutPadding({5.0f, 0.0f, 5.0f, 0.0f});
    // 预编辑背景与分隔线：底色带与高亮同圆角，分隔线固定 1px；有其一时预编辑行横向铺满，否则底色只包住文字。
    preedit_->SetBackground(tokens.preeditBackground, tokens.itemRadius);
    preedit_->SetBottomRule(tokens.preeditDivider, 1.0f);
    preedit_->SetHorizontalAlignment(tokens.preeditBackground.a > 0.001f || tokens.preeditDivider.a > 0.001f
                                         ? msimeui::HorizontalAlignment::Stretch
                                         : msimeui::HorizontalAlignment::Leading);

    // 装饰图是宽 width_dip、高 top_inset_dip 的盒子，底边贴着卡片顶边、不与卡片重叠，
    // 按 align 贴卡片左/中/右，图片在盒内等比居中（contain）。
    float cardMinWidthDip = fitContent ? 0.0f : kCandidateMinWidthDip;
    float decorationTopDip = 0.0f;
    std::wstring decorationPath;
    std::wstring backgroundPath;
    const std::wstring packageRoot =
        package ? (skinsRoot / std::filesystem::u8path(package->id)).wstring() + L"\\" : std::wstring();
    if (package)
    {
        decorationTopDip = static_cast<float>(package->decorationTopDip);
        if (!package->decorationImage.empty())
            decorationPath = packageRoot + Widen(package->decorationImage);
        if (!package->backgroundImage.empty())
            backgroundPath = packageRoot + Widen(package->backgroundImage);
        cardMinWidthDip = (std::max)({cardMinWidthDip, static_cast<float>(package->minWidthDip),
                                      static_cast<float>(package->decorationWidthDip)});
    }
    result.cardMinWidthDip = cardMinWidthDip;
    // 外框上方留给阴影的空白与装饰盒重叠，装饰盒高过它时不再额外留白。
    msimeui::Thickness framePad = settings.framePadding;
    decorationCandidate_.reset();
    decorationWordShown_.clear();
    decorationWord_->SetText(L"");
    if (decorationPath.empty() || decorationTopDip <= 0.0f)
    {
        decorationBoxWidthDip_ = 0.0f;
        decorationBoxHeightDip_ = 0.0f;
        decorationBox_->SetHeight(0.0f);
        decorationBox_->SetWidth(0.0f);
        decoration_->SetHeight(0.0f);
        decoration_->ClearWidth();
        decoration_->SetSource(L"");
    }
    else
    {
        decorationBoxWidthDip_ = static_cast<float>(package->decorationWidthDip);
        decorationBoxHeightDip_ = decorationTopDip;
        decoration_->SetSource(decorationPath);
        decoration_->SetHeight(decorationBoxHeightDip_);
        decoration_->SetWidth(decorationBoxWidthDip_);
        decorationBox_->SetHeight(decorationBoxHeightDip_);
        decorationBox_->SetWidth(decorationBoxWidthDip_);
        decorationBox_->SetHorizontalAlignment(DecorationAlignment(package->decorationAlign));
        framePad.top = (std::max)(0.0f, framePad.top - decorationTopDip);
        decorationCandidate_ = package->decorationCandidate;
        if (decorationCandidate_)
        {
            decorationWord_->SetFontFamily(appearance.fontFamily);
            decorationWord_->SetFallbackFontFamilies(appearance.fallbackFontFamilies);
            decorationWord_->SetColor(
                ParseCandidateCssColor(decorationCandidate_->color, D2D1::ColorF(D2D1::ColorF::White)));
        }
    }
    // v2 挂件挂在卡片外沿，要按 outer_insets_dip 往四边多留出空白，否则立绘被裁掉。
    if (package && package->outerInsetsDip)
    {
        const CandidateSkinCatalog::InsetsDip &outer = *package->outerInsetsDip;
        framePad.left += static_cast<float>(outer.left);
        framePad.right += static_cast<float>(outer.right);
        framePad.top += static_cast<float>(outer.top);
        framePad.bottom += static_cast<float>(outer.bottom);
    }
    frame_->SetPadding(framePad);

    // v2 自由挂件交给两层 overlay 绘制，都测量为 0、不参与卡片布局：z_index 低于 kDecorationFrontZIndex 的
    // 在卡片之前绘制（面板之下），其余在卡片之后（面板与文字之上）。同层按 z_index、再按声明顺序叠放。
    {
        CandidateSkinLayers::Layers layers;
        if (package && package->schemaVersion >= 2)
            layers = CandidateSkinLayers::SplitDecorations(package->freeDecorations);
        skinOverlayBack_->SetItems(OverlayItems(layers.below, packageRoot));
        skinOverlayFront_->SetItems(OverlayItems(layers.above, packageRoot));
    }

    msimeui::Brush brush;
    brush.fill = tokens.surface;
    // 有系统材质时色调已经画在合成层里，卡片不再叠底色；皮肤要材质却用不上（透明效果关闭或系统不支持）时
    // 把半透明底色补成不透明，免得直接透出后面的窗口。
    if (result.backdropActive)
        brush.fill.a = 0.0f;
    else if (result.wantsBackdrop)
        brush.fill.a = 1.0f;
    brush.stroke = tokens.border;
    brush.strokeWidth = tokens.borderWidth;
    brush.radiusX = tokens.radius;
    brush.radiusY = tokens.radius;
    if (panelImage)
    {
        brush.fill = D2D1::ColorF(0, 0.0f);
        brush.stroke = D2D1::ColorF(0, 0.0f);
        brush.strokeWidth = 0.0f;
        brush.radiusX = 0.0f;
        brush.radiusY = 0.0f;
    }
    card_->SetBrush(brush);
    msimeui::Thickness cardContentPad{tokens.containerPad, tokens.containerPad, tokens.containerPad,
                                      tokens.containerPad};
    if (package && package->schemaVersion >= 2)
    {
        // 内容从面板外缘量起：content_insets_dip，或九宫格边宽加基底内边距，不会压到边框贴图上。
        cardContentPad =
            ToThickness(CandidateSkinLayers::ResolveContentPadding(*package, tokens.containerPad, !horizontalLayout));
    }
    card_->SetPadding(cardContentPad);
    result.cardContentPadding = cardContentPad;
    card_->SetMinWidth(cardMinWidthDip);
    if (panelImage)
    {
        // CSS border-image 语义：切片以源图像素计，边宽以 DIP 计，fill 决定是否画中心块，
        // repeat 决定中段是拉伸还是按源图原尺寸平铺。竖排写了 vertical 子表时换那张图和它自己的几何；
        // 浅色主题有 light_source 时再换主题图。
        const CandidateSkinCatalog::BorderImage &borderImage =
            !horizontalLayout && package->borderImageVertical ? *package->borderImageVertical : *package->borderImage;
        const CandidateSkinLayers::NineSlice nine = CandidateSkinLayers::ResolveNineSlice(borderImage);
        card_->SetBackgroundNineSlice(
            packageRoot + Widen(CandidateSkinCatalog::BorderImageSource(borderImage, candLight)),
            msimeui::NineSlice{ToThickness(nine.slice), ToThickness(nine.width), nine.fill, nine.repeatX, nine.repeatY},
            1.0f);
    }
    else
    {
        card_->SetBackgroundImage(backgroundPath, BackgroundStretch(package ? package->backgroundFit : ""),
                                  package ? static_cast<float>(package->backgroundOpacity) : 1.0f);
    }
    result.tokens = tokens;
    result.palette = palette;
    return result;
}

void CandidateSkinScene::SetDecorationCandidates(const std::vector<PageWord> &page, size_t selectedIndex)
{
    if (!decorationWord_)
        return;
    std::wstring word;
    if (decorationCandidate_)
    {
        const size_t index =
            decorationCandidate_->index == 0 ? selectedIndex : static_cast<size_t>(decorationCandidate_->index - 1);
        if (index < page.size())
            word = decorationCandidate_->stripBadge ? page[index].text : page[index].text + page[index].badge;
    }
    if (word == decorationWordShown_)
        return;
    decorationWordShown_ = word;
    decorationWord_->SetText(word);
    if (word.empty())
        return;
    const CandidateSkinCatalog::DecorationCandidate &style = *decorationCandidate_;
    // 先按皮肤字号量出一行的宽度，宽过上限就按比例缩小字号，最小到 min_font_size_dip。
    const msimeui::SizeF unbounded = {10000.0f, 10000.0f};
    float fontSize = static_cast<float>(style.fontSizeDip);
    decorationWord_->ClearWidth();
    decorationWord_->ClearHeight();
    decorationWord_->SetFontSize(fontSize);
    msimeui::SizeF size = decorationWord_->MeasureInLayout(unbounded);
    const float limit = style.maxWidthDip > 0.0 ? static_cast<float>(style.maxWidthDip) : decorationBoxWidthDip_;
    const float minFontSize = (std::min)(fontSize, static_cast<float>(style.minFontSizeDip));
    if (size.width > limit && size.width > 0.0f && fontSize > minFontSize)
    {
        fontSize = (std::max)(minFontSize, fontSize * limit / size.width);
        decorationWord_->SetFontSize(fontSize);
        size = decorationWord_->MeasureInLayout(unbounded);
    }
    // 定死量出来的尺寸：装饰框比词窄时词照样画成一行（超出框的部分照画），不在框宽处折行。
    decorationWord_->SetWidth(std::ceil(size.width));
    decorationWord_->SetHeight(std::ceil(size.height));
    float x = static_cast<float>(style.offsetXDip);
    if (style.alignH == "center")
        x += (decorationBoxWidthDip_ - size.width) * 0.5f;
    else if (style.alignH == "right")
        x += decorationBoxWidthDip_ - size.width;
    float y = static_cast<float>(style.offsetYDip);
    if (style.alignV == "center")
        y += (decorationBoxHeightDip_ - size.height) * 0.5f;
    else if (style.alignV == "bottom")
        y += decorationBoxHeightDip_ - size.height;
    decorationBox_->SetChildPosition(decorationWord_.get(), x, y);
}
