#include "window/candidate_presenter.h"

#include "config/ime_config.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include "global/globals.h"
#include "ipc/candidate_translation_policy.h"
#include "ipc/ipc.h"
#include "log/candidate_diag_log.h"
#include "skin/candidate_skin_catalog.h"
#include "window/candidate_skin_overlay.h"
#include "utils/common_utils.h"
#include "utils/ime_utils.h"
#include "utils/window_utils.h"
#include "webview2/windows_webview2.h"
#include "window/candidate_skin_palette.h"
#include "window/candidate_wheel_paging.h"
#include "window/ime_windows.h"

#include "msimeui/Controls.h"
#include "msimeui/DeviceResources.h"
#include "msimeui/Layout.h"
#include "msimeui/Scene.h"
#include "msimeui/Theme.h"
#include "msimeui/Window.h"

#include "engine/common/helpcode_utils.h"
#include "engine/core/scheme_type.h"
#include "engine/core/word_item.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
D2D1_COLOR_F ColorFromRgb(UINT rgb, float alpha = 1.0f)
{
    return CandidateColorFromRgb(rgb, alpha);
}

D2D1_COLOR_F ParseCssColor(const std::string &text, D2D1_COLOR_F fallback)
{
    return ParseCandidateCssColor(text, fallback);
}

int AlignHostPixels(int value)
{
    constexpr int kBucket = 64;
    if (value < 1)
    {
        value = 1;
    }
    return ((value + kBucket - 1) / kBucket) * kBucket;
}

// Recompute half-screen DIP limits against `scale` when it differs from the
// monitor-derived scale, so clamping and window sizing stay on one scale
// (mirrors the webview path's rasterization-scale composition).
HalfScreenDipLimits ApplyScaleToHalfScreenLimits(POINT pt, FLOAT scale)
{
    HalfScreenDipLimits limits = QueryHalfScreenDipLimitsForPoint(pt);
    if (scale > 0.0f)
    {
        const double monitorWidthPx = static_cast<double>((std::max)(1, limits.monitor.right - limits.monitor.left));
        const double monitorHeightPx = static_cast<double>((std::max)(1, limits.monitor.bottom - limits.monitor.top));
        limits.scale = scale;
        limits.maxWidthDip = (monitorWidthPx * 0.5) / static_cast<double>(scale);
        limits.maxHeightDip = (monitorHeightPx * 0.5) / static_cast<double>(scale);
    }
    return limits;
}

struct CandSkinTokens
{
    D2D1_COLOR_F surface{};
    D2D1_COLOR_F border{};
    D2D1_COLOR_F text{};
    D2D1_COLOR_F number = ParseCssColor("#e9e8e89d", ColorFromRgb(0xE9E8E8, 0.616f));
    D2D1_COLOR_F selected = ParseCssColor("#3e3e3eb9", ColorFromRgb(0x3E3E3E, 0.725f));
    D2D1_COLOR_F hover = ColorFromRgb(0x414141);
    D2D1_COLOR_F accent = ColorFromRgb(0x6B69D6);
    // Alpha 0: follow the candidate text at the CSS opacity .62 (see CandidateList::Appearance).
    D2D1_COLOR_F translation = D2D1::ColorF(0, 0.0f);
    float radius = 6.0f;
    float borderWidth = 1.5f;
    float containerPad = 5.0f;
    bool showSelectedBar = true;
    // Row corner radius, selected-row text colors and context-menu palette.
    // Defaults are fluent dark; the fluent light branch and per-skin branches
    // override. rowTextSelected/rowLabelSelected alpha 0 keeps the normal
    // colors (bar-style selection, like the fluent CSS).
    float itemRadius = 4.0f;
    // > 0: rows touching the card's corners take this radius there instead.
    float outerItemRadius = 0.0f;
    // Row box geometry. Defaults suit the inset-highlight skins; full-bleed
    // skins (willow green) pad the rows themselves because the card does not.
    float itemGap = 2.0f;
    float itemExtraHeight = 2.0f;
    float itemPadLeft = 0.0f;
    float itemPadRight = 0.0f;
    // 横排时把卡片多出的宽度均分给同一行各项，铺满高亮的皮肤（杨柳青）末项才能贴到卡片右边。
    bool justifyRows = false;
    // 序号字号相对候选字号的比例、序号与候选之间的空、横排每项的最小宽度（0 为不限）。
    float labelScale = 0.8f;
    float labelGap = 1.5f;
    float minItemWidth = 0.0f;
    // 选中条的尺寸；selectedBarInside 让它落在高亮左缘之内，而不是骑在左缘上。
    float selectedBarWidth = 3.0f;
    float selectedBarHeightEm = 0.85f;
    bool selectedBarInside = false;
    // 翻页箭头画成候选文字色的实心三角（微软拼音那样），横排时前面再加一条分隔线。
    bool trianglePager = false;
    // 预编辑行隐藏时抵掉它与列表之间的 2px 行距，让卡片上下内边距一致。贴角皮肤总是这样做。
    bool trimHiddenPreeditGap = false;
    D2D1_COLOR_F pagerDivider = D2D1::ColorF(0, 0.0f);
    msimeui::Thickness preeditMargin{};
    D2D1_COLOR_F rowTextSelected = D2D1::ColorF(0, 0.0f);
    D2D1_COLOR_F rowLabelSelected = D2D1::ColorF(0, 0.0f);
    D2D1_COLOR_F menuFill = D2D1::ColorF(0x2D2D2D);
    D2D1_COLOR_F menuBorder = ParseCssColor("#9b9b9b2e", D2D1::ColorF(0x3A3A3A, 0.18f));
    D2D1_COLOR_F menuText = D2D1::ColorF(0xE9E8E8);
    D2D1_COLOR_F menuHover = ColorFromRgb(0x414141);
    // 外部皮肤的细分配色。alpha 0 表示没写：光标与选中条跟 accent，选中行翻译跟 translation，
    // 预编辑背景与分隔线不画。
    D2D1_COLOR_F preeditCaret = D2D1::ColorF(0, 0.0f);
    D2D1_COLOR_F selectedBar = D2D1::ColorF(0, 0.0f);
    D2D1_COLOR_F rowTranslationSelected = D2D1::ColorF(0, 0.0f);
    // 翻译里的词性与分隔符。alpha 0 表示与本行翻译同色。
    D2D1_COLOR_F translationPos = D2D1::ColorF(0, 0.0f);
    D2D1_COLOR_F translationSeparator = D2D1::ColorF(0, 0.0f);
    D2D1_COLOR_F rowTranslationPosSelected = D2D1::ColorF(0, 0.0f);
    D2D1_COLOR_F rowTranslationSeparatorSelected = D2D1::ColorF(0, 0.0f);
    D2D1_COLOR_F preeditBackground = D2D1::ColorF(0, 0.0f);
    D2D1_COLOR_F preeditDivider = D2D1::ColorF(0, 0.0f);
};

// 内置皮肤翻译里词性与分隔符的不透明度，对应 CSS 里 8 位十六进制颜色的 99 与 73。
constexpr float kTranslationPosAlpha = 0x80 / 255.0f;
constexpr float kTranslationSeparatorAlpha = 0x73 / 255.0f;

// 内置皮肤的翻译配色取自 Notion 调色板（https://docs.super.so/notion-colors），与
// ui-html/webview2/candwnd/skins/<skin>/*.css 里的 .cand-translation 同值。词性用相邻的另一个色相再调淡，
// 和英文单词区分开；分隔符是单词色降不透明度。
void SetTranslationColors(CandSkinTokens &tokens, UINT rgb, UINT posRgb)
{
    tokens.translation = ColorFromRgb(rgb);
    tokens.translationPos = ColorFromRgb(posRgb, kTranslationPosAlpha);
    tokens.translationSeparator = ColorFromRgb(rgb, kTranslationSeparatorAlpha);
}

// 选中行铺了实色高亮的皮肤，翻译在选中行另换一组颜色，避免彩色字压在高亮色上。
// 词性的不透明度单独给：实色高亮上按普通行的比例调淡会糊进底色。
void SetSelectedTranslationColors(CandSkinTokens &tokens, UINT rgb, float alpha, UINT posRgb, float posAlpha)
{
    tokens.rowTranslationSelected = ColorFromRgb(rgb, alpha);
    tokens.rowTranslationPosSelected = ColorFromRgb(posRgb, posAlpha);
    tokens.rowTranslationSeparatorSelected = ColorFromRgb(rgb, alpha * kTranslationSeparatorAlpha);
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

void ApplyPackageColors(const CandidateSkinCatalog::CandidateColors &colors, CandSkinTokens &tokens)
{
    const auto apply = [](const std::string &value, D2D1_COLOR_F &out) {
        if (!value.empty())
            out = ParseCssColor(value, out);
    };
    apply(colors.preeditCaret, tokens.preeditCaret);
    apply(colors.selectedBar, tokens.selectedBar);
    apply(colors.selectedText, tokens.rowTextSelected);
    apply(colors.selectedNumber, tokens.rowLabelSelected);
    // 包换了翻译色就不再用基础皮肤的词性/分隔符配色：普通行回到与翻译同色，选中行跟随 selected_translation。
    // 与 WebView2 端的 `color: inherit` 重置一致。
    if (!colors.translation.empty())
    {
        tokens.translationPos = D2D1::ColorF(0, 0.0f);
        tokens.translationSeparator = D2D1::ColorF(0, 0.0f);
    }
    if (!colors.selectedTranslation.empty())
    {
        apply(colors.selectedTranslation, tokens.rowTranslationSelected);
        tokens.rowTranslationPosSelected = tokens.rowTranslationSelected;
        tokens.rowTranslationSeparatorSelected = tokens.rowTranslationSelected;
    }
    apply(colors.translationPos, tokens.translationPos);
    apply(colors.translationSeparator, tokens.translationSeparator);
    apply(colors.selectedTranslationPos, tokens.rowTranslationPosSelected);
    apply(colors.selectedTranslationSeparator, tokens.rowTranslationSeparatorSelected);
    apply(colors.preeditBackground, tokens.preeditBackground);
    apply(colors.preeditDivider, tokens.preeditDivider);
    apply(colors.menuBackground, tokens.menuFill);
    apply(colors.menuBorder, tokens.menuBorder);
    apply(colors.menuText, tokens.menuText);
    apply(colors.menuHover, tokens.menuHover);
    if (!colors.accent.empty())
    {
        tokens.accent = ParseCssColor(colors.accent, tokens.accent);
    }
    if (!colors.selected.empty())
    {
        tokens.selected = ParseCssColor(colors.selected, tokens.selected);
    }
    if (!colors.hover.empty())
    {
        tokens.hover = ParseCssColor(colors.hover, tokens.hover);
    }
    if (!colors.number.empty())
    {
        tokens.number = ParseCssColor(colors.number, tokens.number);
    }
    if (!colors.translation.empty())
    {
        tokens.translation = ParseCssColor(colors.translation, tokens.translation);
    }
    if (colors.showSelectedBar.has_value())
    {
        tokens.showSelectedBar = *colors.showSelectedBar;
    }
}

std::wstring AssetRoot()
{
    return CommonUtils::get_ime_data_path_w();
}

constexpr float kShadowPadLeft = 32.0f;
constexpr float kShadowPadTop = 20.0f;
constexpr float kShadowPadRight = 32.0f;
constexpr float kShadowPadBottom = 40.0f;
constexpr float kCandidateMinWidthDip = 160.0f;

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
// Below this fraction of the kept extent, the page really got smaller (fewer or
// shorter candidates) and the card follows it; above it, the difference is a
// late addition coming or going and the card keeps its size. Mirrors
// kStickyShrinkRatio in the WebView2 candidate script.
constexpr float kStickyCardShrinkRatio = 0.7f;

float StickyCardExtent(float kept, float natural)
{
    return natural >= kept || natural < kept * kStickyCardShrinkRatio ? natural : kept;
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

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha)
{
    color.a *= alpha;
    return color;
}

} // namespace

struct CandidatePresenter::Impl
{
    msimeui::DeviceResources resources;
    std::unique_ptr<msimeui::Window> window;
    std::shared_ptr<msimeui::StackPanel> root;
    std::shared_ptr<msimeui::Image> decoration;
    std::shared_ptr<msimeui::Card> card;
    // 卡片与 v2 挂件层放在同一格里：挂件层测量为 0，格子仍由卡片决定，挂件按 anchor 画到卡片外沿。
    std::shared_ptr<msimeui::Grid> overlayGrid;
    std::shared_ptr<CandidateSkinOverlay> skinOverlay;
    // 装饰图与卡片同列：列宽即卡片宽，装饰图在列内按 align 对齐，对应 CSS 里
    // .containerParent 为 fit-content、::before 相对它定位。
    std::shared_ptr<msimeui::StackPanel> cardColumn;
    std::shared_ptr<msimeui::Container> frame;
    std::shared_ptr<msimeui::StackPanel> body;
    std::shared_ptr<msimeui::TextBlock> preedit;
    std::shared_ptr<msimeui::CandidateList> list;
    // 皮肤关掉翻页箭头时为空。场景按 sceneHorizontal / scenePageArrows 搭建，两者变了就重建。
    std::shared_ptr<msimeui::PagerArrows> pager;
    bool sceneHorizontal = false;
    bool scenePageArrows = false;
    std::shared_ptr<msimeui::Popup> contextMenu;
    std::shared_ptr<msimeui::Popup> contextSubmenu;
    std::shared_ptr<msimeui::MenuFlyoutItem> fixPositionItem;
    D2D1_COLOR_F menuFill = D2D1::ColorF(0x2D2D2D);
    D2D1_COLOR_F menuBorder = ParseCssColor("#9b9b9b2e", D2D1::ColorF(0x3A3A3A, 0.18f));
    D2D1_COLOR_F menuText = D2D1::ColorF(0xE9E8E8);
    D2D1_COLOR_F menuHover = D2D1::ColorF(0x414141);
    RECT hostRectBeforeMenu{};
    bool contextMenuOpen = false;
    bool contextSubmenuOpen = false;
    bool hostExpandedForMenu = false;
    size_t contextMenuPageIndex = 0;
};

CandidatePresenter::CandidatePresenter() = default;

CandidatePresenter &CandidatePresenter::Instance()
{
    static CandidatePresenter instance;
    return instance;
}

bool CandidatePresenter::IsBound() const
{
    return bound_;
}

bool CandidatePresenter::Bind(HWND hwnd)
{
    hwnd_ = hwnd;
    if (!hwnd)
    {
        bound_ = false;
        impl_.reset();
        return false;
    }
    impl_ = std::make_unique<Impl>();
    impl_->window = std::make_unique<msimeui::Window>(L"metasequoiaime_windows", L"cand", 1, 1);
    impl_->window->AdoptExistingHwnd(hwnd);
    impl_->window->SetStealFocusOnClick(false);
    bound_ = impl_->resources.EnsureForComposition(hwnd);
    RebuildScene();
    CAND_DIAG_LOGF(L"candidate-d2d bind hwnd={:#x} composition={}", reinterpret_cast<uintptr_t>(hwnd), bound_ ? 1 : 0);
    return bound_;
}

void CandidatePresenter::RebuildScene()
{
    if (!impl_ || !impl_->window)
    {
        return;
    }
    lastSkinFingerprint_.clear();
    impl_->root = std::make_shared<msimeui::StackPanel>(0.0f);
    impl_->decoration = std::make_shared<msimeui::Image>(L"");
    impl_->decoration->SetHorizontalAlignment(msimeui::HorizontalAlignment::Trailing);
    impl_->decoration->SetStretch(msimeui::ImageStretch::Uniform);
    impl_->preedit = std::make_shared<msimeui::TextBlock>(L"", 14.0f, D2D1::ColorF(0xF5F5F5));
    impl_->preedit->SetTextLayoutPadding({0.0f, 1.0f, 0.0f, 1.0f});
    impl_->preedit->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
    impl_->list = std::make_shared<msimeui::CandidateList>(28.0f);
    impl_->list->SetOnItemActivated([this](size_t index) { CommitItem(index); });
    impl_->list->SetOnContextMenu(
        [this](size_t index, const POINT &clientPoint) { ShowItemContextMenu(index, clientPoint); });
    impl_->body = std::make_shared<msimeui::StackPanel>(2.0f);
    impl_->body->SetPadding({0.0f, 0.0f, 0.0f, 0.0f});
    impl_->body->AddChild(impl_->preedit);
    impl_->pager.reset();
    if (impl_->scenePageArrows)
    {
        impl_->pager = std::make_shared<msimeui::PagerArrows>();
        // 箭头在自己的槽位里贴右下角，槽位铺满即可（竖排是整行宽，横排是列表的整个高度）。
        impl_->pager->SetVerticalAlignment(msimeui::VerticalAlignment::Stretch);
        impl_->pager->SetOnClick([this](msimeui::PagerArrows::Part part) {
            if (hwnd_)
            {
                PostMessageW(hwnd_, WM_PAGE_CANDIDATE_ARROW,
                             part == msimeui::PagerArrows::Part::Next ? CANDIDATE_PAGE_NEXT : CANDIDATE_PAGE_PREVIOUS,
                             1);
            }
        });
    }
    if (impl_->pager && impl_->sceneHorizontal)
    {
        impl_->body->AddChild(std::make_shared<CandidateRowWithPager>(impl_->list, impl_->pager));
    }
    else
    {
        // 竖排时箭头是列表下方单独的一矮行，靠右。
        impl_->body->AddChild(impl_->list);
        if (impl_->pager)
            impl_->body->AddChild(impl_->pager);
    }
    msimeui::Brush brush;
    brush.fill = D2D1::ColorF(0x202020);
    brush.stroke = ParseCssColor("#9b9b9b2e", D2D1::ColorF(0x3A3A3A, 0.18f));
    brush.strokeWidth = 1.5f;
    brush.radiusX = 6.0f;
    brush.radiusY = 6.0f;
    impl_->card = std::make_shared<msimeui::Card>(brush, 5.0f);
    impl_->card->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
    impl_->card->AddChild(impl_->body);
    impl_->root->SetHorizontalContentAlignment(msimeui::HorizontalAlignment::Leading);
    impl_->overlayGrid = std::make_shared<msimeui::Grid>();
    impl_->overlayGrid->AddRow(msimeui::GridLength{msimeui::GridUnitType::Auto, 1.0f});
    impl_->overlayGrid->AddColumn(msimeui::GridLength{msimeui::GridUnitType::Auto, 1.0f});
    impl_->overlayGrid->AddChild(impl_->card, 0, 0);
    impl_->skinOverlay = std::make_shared<CandidateSkinOverlay>();
    impl_->skinOverlay->SetHorizontalAlignment(msimeui::HorizontalAlignment::Stretch);
    impl_->skinOverlay->SetVerticalAlignment(msimeui::VerticalAlignment::Stretch);
    impl_->overlayGrid->AddChild(impl_->skinOverlay, 0, 0);
    impl_->cardColumn = std::make_shared<msimeui::StackPanel>(0.0f);
    impl_->cardColumn->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
    impl_->cardColumn->SetHorizontalContentAlignment(msimeui::HorizontalAlignment::Stretch);
    impl_->cardColumn->AddChild(impl_->decoration);
    impl_->cardColumn->AddChild(impl_->overlayGrid);
    impl_->frame = std::make_shared<msimeui::Container>();
    impl_->frame->SetPadding({kShadowPadLeft, kShadowPadTop, kShadowPadRight, kShadowPadBottom});
    impl_->frame->SetChild(impl_->cardColumn);
    impl_->root->AddChild(impl_->frame);
    auto scene = std::make_unique<msimeui::Scene>();
    scene->SetRoot(impl_->root);
    impl_->window->SetScene(std::move(scene));
}

void CandidatePresenter::ApplySkin()
{
    if (!impl_ || !impl_->list || !impl_->preedit || !impl_->window)
    {
        return;
    }
    const std::string skinId = GetConfiguredCandidateSkin();
    // Resolve the effective light/dark mode before the cache check: with
    // theme_cand = "follow" the raw setting is unchanged when theme_mode or
    // the Windows theme flips, but the resolved colors are what we render
    // with, so the resolved mode must participate in the fingerprint.
    const bool candLight = ResolveConfiguredTheme(GetConfiguredThemeCand()) == "light";
    std::ostringstream fingerprint;
    fingerprint << skinId << '|' << GetConfiguredThemeCand() << '|' << (candLight ? 'L' : 'D') << '|'
                << GetConfiguredCandidateFont() << '|' << GetConfiguredCandidateFontSize() << '|'
                << GetConfiguredCandidateWindowPreeditFontSize() << '|' << GetConfiguredCandidateWindowLayout() << '|'
                << GetConfiguredCandidateTextColor() << '|' << GetConfiguredCandidateWindowPreeditStyle();
    fingerprint << '|' << GetConfiguredCandidateEnglishFont() << '|' << GetConfiguredCandidateWindowFitContent();
    // 同一个外部皮肤的 skin.toml 或图片改了，只有强制重载能看出来；带上重载代数让 D2D 也重读 manifest。
    fingerprint << '|' << GetCandidateSkinReloadRevision();
    for (const auto &font : GetConfiguredCandidateFallbackFonts())
        fingerprint << '|' << font.size() << ':' << font;
    const std::string skinKey = fingerprint.str();
    if (skinKey == lastSkinFingerprint_ && impl_->card)
    {
        return;
    }
    lastSkinFingerprint_ = skinKey;
    CandSkinTokens tokens;
    if (candLight)
    {
        tokens.number = D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f, 0.55f);
        tokens.selected = ColorFromRgb(0xE8E8E8);
        tokens.hover = ColorFromRgb(0xECECEC);
        tokens.menuFill = ColorFromRgb(0xFFFFFF);
        tokens.menuBorder = D2D1::ColorF(0, 0.12f);
        tokens.menuText = ColorFromRgb(0x1A1A1A);
        tokens.menuHover = ColorFromRgb(0xECECEC);
    }
    const std::wstring skinsRoot = AssetRoot() + L"\\skins";
    // 外部皮肤用声明支持当前布局与主题的那一份；不支持或读不出来就回退 fluent，和 WebView2 端同一条规则。
    const std::string candidateLayout = GetConfiguredCandidateWindowLayout();
    const std::string candidateTheme = candLight ? "light" : "dark";
    std::optional<CandidateSkinCatalog::Package> package;
    if (!CandidateSkinCatalog::IsBuiltIn(skinId))
    {
        package = CandidateSkinCatalog::LoadSupported(std::filesystem::path(skinsRoot), skinId, candidateLayout,
                                                      candidateTheme);
    }
    // 自定义包的几何取自它的 base 皮肤，与 WebView2 端注入 base 皮肤 CSS 一致；颜色随后由包覆盖。
    const std::string baseSkin =
        package ? package->base : (CandidateSkinCatalog::IsBuiltIn(skinId) ? skinId : std::string("fluent"));
    // Built-in skin palettes mirror ui-html/webview2/candwnd/skins/<skin>/;
    // the WebView2 CSS is the reference for both the light and dark values.
    // 横排时高亮内缩于外框，首项、尾项落在卡片四角上的那几个角与卡片同 R，其余角保持
    // itemRadius，对应 CSS 横排的 .pinyin + .row-wrapper / .last-visible 规则。
    const bool horizontalLayout = candidateLayout == "horizontal";
    // 翻页箭头改变的是场景结构（竖排多一行、横排多一列），开关或排列方式变了就整棵重建，
    // 再往下套用配色与尺寸。重建会清掉指纹，这里补回去。
    const bool pageArrows = CandidateSkinCatalog::ResolvePageArrows(std::filesystem::path(skinsRoot), skinId,
                                                                    package ? &*package : nullptr);
    if (pageArrows != impl_->scenePageArrows || horizontalLayout != impl_->sceneHorizontal)
    {
        impl_->scenePageArrows = pageArrows;
        impl_->sceneHorizontal = horizontalLayout;
        RebuildScene();
        lastSkinFingerprint_ = skinKey;
    }
    fitContent_ = GetConfiguredCandidateWindowFitContent() ||
                  CandidateSkinCatalog::ResolveFitContent(std::filesystem::path(skinsRoot), skinId,
                                                          package ? &*package : nullptr);
    const float fontSize = static_cast<float>((std::max)(12, GetConfiguredCandidateFontSize()));
    if (baseSkin == "fluent")
    {
        if (horizontalLayout)
            tokens.outerItemRadius = tokens.radius;
        // 单词 Notion Purple，与强调色同一色相；词性 Notion Pink。
        SetTranslationColors(tokens, candLight ? 0x6940A5 : 0x9A6DD7, candLight ? 0xAD1A72 : 0xE255A1);
    }
    else if (baseSkin == "wechat")
    {
        tokens.borderWidth = 1.0f;
        tokens.radius = 5.0f;
        tokens.containerPad = 2.0f;
        if (horizontalLayout)
            tokens.outerItemRadius = tokens.radius;
        tokens.accent = ColorFromRgb(0x07C160);
        tokens.selected = ColorFromRgb(0x07C160);
        tokens.showSelectedBar = false;
        tokens.rowTextSelected = ColorFromRgb(0xFFFFFF);
        tokens.rowLabelSelected = ColorFromRgb(0xFFFFFF);
        // 单词 Notion Blue，词性 Notion Green；选中行是实色绿底，单词换成半透明白，词性换 Notion Yellow。
        SetTranslationColors(tokens, candLight ? 0x0B6E99 : 0x529CCA, candLight ? 0x0F7B6C : 0x4DAB9A);
        SetSelectedTranslationColors(tokens, 0xFFFFFF, 0.8f, 0xFFDC49, 0.85f);
        if (candLight)
        {
            tokens.hover = D2D1::ColorF(7.0f / 255.0f, 193.0f / 255.0f, 96.0f / 255.0f, 0.14f);
            tokens.number = ColorFromRgb(0x757575);
            tokens.menuFill = ColorFromRgb(0xFFFFFF);
            tokens.menuBorder = ColorFromRgb(0xD9D9D9);
            tokens.menuText = ColorFromRgb(0x333333);
            tokens.menuHover = ColorFromRgb(0xEEEEEE);
        }
        else
        {
            tokens.hover = D2D1::ColorF(7.0f / 255.0f, 193.0f / 255.0f, 96.0f / 255.0f, 0.32f);
            tokens.number = ColorFromRgb(0x858585);
            tokens.menuFill = ColorFromRgb(0x1F1F1F);
            tokens.menuBorder = ColorFromRgb(0x343434);
            tokens.menuText = ColorFromRgb(0xD0D0D0);
            tokens.menuHover = ColorFromRgb(0x2A2A2A);
        }
    }
    else if (baseSkin == "willow_green")
    {
        tokens.borderWidth = 0.0f;
        tokens.radius = 9.0f;
        tokens.containerPad = 0.0f;
        // 高亮整行（竖排）/整列（横排）铺满、左右贴着卡片边，行本身是直角；CSS 靠
        // 容器 clip-path 裁出窗口圆角。D2D 的 Card 不裁剪子控件，所以改由列表把
        // 落在卡片四角上的那几个角画成卡片的 9px，其余角保持直角。
        tokens.itemRadius = 0.0f;
        tokens.outerItemRadius = 9.0f;
        tokens.itemGap = 0.0f;
        tokens.justifyRows = true;
        // 行内边距对齐 CSS 的 .row.cand padding（加上 .text 的左内边距），预编辑行对齐 .row.pinyin。
        if (GetConfiguredCandidateWindowLayout() == "horizontal")
        {
            tokens.itemExtraHeight = 10.0f;
            tokens.itemPadLeft = 6.0f;
            tokens.itemPadRight = 10.0f;
            tokens.preeditMargin = {3.0f, 6.0f, 5.0f, 1.0f};
        }
        else
        {
            tokens.itemExtraHeight = 8.0f;
            tokens.itemPadLeft = 9.0f;
            tokens.itemPadRight = 14.0f;
            tokens.preeditMargin = {3.0f, 6.0f, 7.0f, 2.0f};
        }
        tokens.showSelectedBar = false;
        tokens.rowTextSelected = ColorFromRgb(0xFFFFFF);
        tokens.rowLabelSelected = ColorFromRgb(0xFFFFFF);
        // 单词 Notion Green，词性 Notion Blue；选中行是实色绿底，单词换成半透明白，词性换 Notion Yellow。
        SetTranslationColors(tokens, candLight ? 0x0F7B6C : 0x4DAB9A, candLight ? 0x0B6E99 : 0x529CCA);
        SetSelectedTranslationColors(tokens, 0xFFFFFF, 0.8f, 0xFFDC49, 0.85f);
        if (candLight)
        {
            tokens.accent = ColorFromRgb(0x58B980);
            tokens.selected = ColorFromRgb(0x58B980);
            tokens.hover = D2D1::ColorF(88.0f / 255.0f, 185.0f / 255.0f, 128.0f / 255.0f, 0.16f);
            tokens.number = ColorFromRgb(0x686F6A);
            tokens.menuFill = ColorFromRgb(0xFBFCFA);
            tokens.menuBorder = ColorFromRgb(0xD8DED9);
            tokens.menuText = ColorFromRgb(0x343936);
            tokens.menuHover = ColorFromRgb(0xE9EEEA);
        }
        else
        {
            tokens.accent = ColorFromRgb(0x65C98D);
            tokens.selected = ColorFromRgb(0x65C98D);
            tokens.hover = D2D1::ColorF(101.0f / 255.0f, 201.0f / 255.0f, 141.0f / 255.0f, 0.22f);
            tokens.number = ColorFromRgb(0xA6ABA7);
            tokens.menuFill = ColorFromRgb(0x343635);
            tokens.menuBorder = ColorFromRgb(0x454845);
            tokens.menuText = ColorFromRgb(0xE0E2DF);
            tokens.menuHover = ColorFromRgb(0x414441);
        }
    }
    else if (baseSkin == "graphite")
    {
        tokens.borderWidth = 1.0f;
        tokens.radius = 3.0f;
        tokens.containerPad = 5.0f;
        tokens.itemRadius = 2.0f;
        tokens.selected = D2D1::ColorF(0, 0.0f);
        tokens.showSelectedBar = false;
        // 单词 Notion Brown，石墨铅笔的木杆色，压在冷灰底上不抢候选；词性 Notion Orange。
        SetTranslationColors(tokens, candLight ? 0x64473A : 0x937264, candLight ? 0xD9730D : 0xFFA344);
        if (candLight)
        {
            tokens.accent = ColorFromRgb(0x5F6B7A);
            tokens.hover = D2D1::ColorF(31.0f / 255.0f, 41.0f / 255.0f, 55.0f / 255.0f, 0.055f);
            tokens.number = ColorFromRgb(0x8993A1);
            tokens.rowTextSelected = ColorFromRgb(0x111827);
            tokens.rowLabelSelected = ColorFromRgb(0x111827);
            tokens.menuFill = ColorFromRgb(0xFFFFFF);
            tokens.menuBorder = ColorFromRgb(0xDFE3E8);
            tokens.menuText = ColorFromRgb(0x374151);
            tokens.menuHover = ColorFromRgb(0xF1F3F5);
        }
        else
        {
            tokens.accent = ColorFromRgb(0x8993A0);
            tokens.hover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f);
            tokens.number = ColorFromRgb(0x707987);
            tokens.rowTextSelected = ColorFromRgb(0xF1F3F5);
            tokens.rowLabelSelected = ColorFromRgb(0xF1F3F5);
            tokens.menuFill = ColorFromRgb(0x23272C);
            tokens.menuBorder = ColorFromRgb(0x3A4047);
            tokens.menuText = ColorFromRgb(0xC7CDD5);
            tokens.menuHover = ColorFromRgb(0x30353B);
        }
    }
    else if (baseSkin == "autumn_osmanthus")
    {
        tokens.borderWidth = 0.0f;
        tokens.radius = 10.0f;
        tokens.containerPad = 5.0f;
        // 高亮块内缩在卡片里：普通角 6px，贴着卡片四角的那几个角与卡片同为 10px
        // （首项顶部两角、尾项底部两角），对应 CSS 的 --ao-radius / --ao-item-radius。
        tokens.itemRadius = 6.0f;
        tokens.outerItemRadius = 10.0f;
        tokens.showSelectedBar = false;
        // 翻译：浅色单词 Notion Orange、词性 Notion Red，选中行的淡黄高亮上单词换 Notion Brown；
        // 深色单词 Notion Yellow、词性 Notion Orange，选中行橙底上单词换半透明白、词性换 Notion Brown。
        if (candLight)
        {
            SetTranslationColors(tokens, 0xD9730D, 0xE03E3E);
            SetSelectedTranslationColors(tokens, 0x64473A, 1.0f, 0xE03E3E, kTranslationPosAlpha);
            tokens.accent = ColorFromRgb(0xE6A817);
            tokens.selected = ColorFromRgb(0xFFE399);
            tokens.hover = D2D1::ColorF(255.0f / 255.0f, 227.0f / 255.0f, 153.0f / 255.0f, 0.55f);
            tokens.number = ColorFromRgb(0x5B727B);
            tokens.rowTextSelected = ColorFromRgb(0x1A1A1A);
            tokens.rowLabelSelected = ColorFromRgb(0x1A1A1A);
            tokens.menuFill = ColorFromRgb(0xE8F5F7);
            tokens.menuBorder = ColorFromRgb(0xBCD8DE);
            tokens.menuText = ColorFromRgb(0x1F3138);
            tokens.menuHover = ColorFromRgb(0xD2E8EC);
        }
        else
        {
            SetTranslationColors(tokens, 0xFFDC49, 0xFFA344);
            SetSelectedTranslationColors(tokens, 0xFFFFFF, 0.8f, 0x64473A, 0.85f);
            tokens.accent = ColorFromRgb(0xF97D0A);
            tokens.selected = ColorFromRgb(0xF97D0A);
            tokens.hover = D2D1::ColorF(249.0f / 255.0f, 125.0f / 255.0f, 10.0f / 255.0f, 0.30f);
            tokens.number = ColorFromRgb(0xE1E8EC);
            tokens.rowTextSelected = ColorFromRgb(0xFFFFFF);
            tokens.rowLabelSelected = ColorFromRgb(0xFFFFFF);
            tokens.menuFill = ColorFromRgb(0x6F8491);
            tokens.menuBorder = ColorFromRgb(0x8CA0AC);
            tokens.menuText = ColorFromRgb(0xF5F8FA);
            tokens.menuHover = ColorFromRgb(0x637885);
        }
    }
    else if (baseSkin == "microsoft")
    {
        // 几何随字号缩放，对应 CSS 里以 em 写的尺寸（ui-html/webview2/candwnd/skins/microsoft/）。
        tokens.borderWidth = 1.0f;
        tokens.radius = 8.0f;
        tokens.containerPad = std::round(fontSize * 0.3f);
        tokens.itemRadius = 4.0f;
        tokens.itemExtraHeight = std::round(fontSize * 0.24f);
        // 序号左端距高亮左缘 0.4em，其中 5 DIP 是列表固定的 textPadLeft。
        tokens.itemPadLeft = (std::max)(0.0f, std::round(fontSize * 0.4f) - 5.0f);
        tokens.itemPadRight = std::round(fontSize * 0.3f);
        if (horizontalLayout)
        {
            tokens.itemGap = std::round(fontSize * 0.15f);
            tokens.minItemWidth = std::round(fontSize * 3.5f);
        }
        tokens.labelScale = 1.0f;
        tokens.labelGap = std::round(fontSize * 0.2f);
        tokens.selectedBarWidth = 3.0f;
        tokens.selectedBarHeightEm = 0.9f;
        tokens.selectedBarInside = true;
        tokens.trianglePager = true;
        tokens.trimHiddenPreeditGap = true;
        tokens.accent = ColorFromRgb(0xE183D9);
        // 单词 Notion Pink，与粉色选中条同一色相；词性 Notion Purple。
        SetTranslationColors(tokens, candLight ? 0xAD1A72 : 0xE255A1, candLight ? 0x6940A5 : 0x9A6DD7);
        if (candLight)
        {
            tokens.selected = ColorFromRgb(0xEAEAEA);
            tokens.hover = ColorFromRgb(0xF0F0F0);
            tokens.number = ColorFromRgb(0x5F5F5F);
            tokens.pagerDivider = ColorFromRgb(0xE0E0E0);
            tokens.menuFill = ColorFromRgb(0xF9F9F9);
            tokens.menuBorder = D2D1::ColorF(0, 0.1f);
            tokens.menuText = ColorFromRgb(0x1A1A1A);
            tokens.menuHover = ColorFromRgb(0xEAEAEA);
        }
        else
        {
            tokens.selected = ColorFromRgb(0x383838);
            tokens.hover = ColorFromRgb(0x353535);
            tokens.number = ColorFromRgb(0xCFCFCF);
            tokens.pagerDivider = ColorFromRgb(0x3D3D3D);
            tokens.menuFill = ColorFromRgb(0x2C2C2C);
            tokens.menuBorder = ColorFromRgb(0x1C1C1C);
            tokens.menuText = ColorFromRgb(0xFFFFFF);
            tokens.menuHover = ColorFromRgb(0x383838);
        }
    }

    if (package)
    {
        ApplyPackageColors(candLight ? package->light : package->dark, tokens);
        // 自定义圆角：外框和贴着外框四角的高亮角一起换成 R，普通高亮角不变。
        if (package->cornerRadiusDip)
        {
            tokens.radius = static_cast<float>(*package->cornerRadiusDip);
            // 贴角规则以 outerItemRadius > 0 为开关；R 为 0 时仍要保持开启，否则贴角的
            // 高亮会退回普通圆角、从直角外框里探出来。
            if (tokens.outerItemRadius > 0.0f)
                tokens.outerItemRadius = (std::max)(tokens.radius, 0.01f);
        }
        if (package->borderWidthDip)
            tokens.borderWidth = static_cast<float>(*package->borderWidthDip);
        // 只改普通高亮角；贴着卡片四角的那几个角仍跟卡片圆角。
        if (package->itemCornerRadiusDip)
            tokens.itemRadius = static_cast<float>(*package->itemCornerRadiusDip);
    }
    const CandidateSkinCatalog::CandidateColors *packageColors =
        package ? &(candLight ? package->light : package->dark) : nullptr;
    const CandidateSkinPalette palette =
        ResolveCandidateSkinPalette(baseSkin, candLight, GetConfiguredCandidateTextColor(), packageColors);
    tokens.surface = palette.surface;
    tokens.border = palette.border;
    tokens.text = palette.text;
    // 阴影跟皮肤走，与 WebView2 端同一参照：四套内置皮肤与自定义包共用的皮肤 CSS
    // （ui-html/webview2/candwnd/skins/*/horizontal_*.css）带同一对 box-shadow——
    // light `8px 10px 24px rgba(0,0,0,.18), 2px 3px 8px rgba(0,0,0,.10)`，dark α .34/.22；
    // σ = blur ÷ 2。默认皮肤（webview 端只有 body HTML，无 box-shadow）直接关阴影。
    if (impl_->card)
    {
        const std::string shadow = package ? package->shadow : std::string{};
        if (shadow != "none")
        {
            // 外部皮肤的 shadow = soft / strong 按同一系数缩放两层不透明度，与 WebView2 端一致。
            const float scale = ShadowAlphaScale(shadow);
            const auto alpha = [scale](float value) { return (std::min)(value * scale, 1.0f); };
            const std::vector<msimeui::ShadowPass> shadowPasses = {
                {12.0f, alpha(candLight ? 0.18f : 0.34f), 8.0f, 10.0f},
                {4.0f, alpha(candLight ? 0.10f : 0.22f), 2.0f, 3.0f},
            };
            impl_->card->SetShadowEnabled(true);
            impl_->card->SetShadowPasses(shadowPasses);
        }
        else
        {
            impl_->card->SetShadowEnabled(false);
        }
    }

    msimeui::Theme theme = msimeui::ThemeManager::GetCurrent();
    theme.textInputFontFamily = string_to_wstring(GetConfiguredCandidateFont());
    if (theme.textInputFontFamily.empty())
    {
        theme.textInputFontFamily = L"Microsoft YaHei UI";
    }
    theme.uiFontFamily = theme.textInputFontFamily;
    theme.surface = tokens.surface;
    theme.border = tokens.border;
    theme.textPrimary = palette.text;
    theme.textSecondary = tokens.number;
    theme.primary = tokens.accent;
    theme.windowBackground = D2D1::ColorF(0, 0.0f);
    msimeui::ThemeManager::SetCurrent(theme);

    const float preeditSize = static_cast<float>((std::max)(12, GetConfiguredCandidateWindowPreeditFontSize()));
    msimeui::CandidateList::Appearance appearance;
    appearance.fontFamily = string_to_wstring(ResolveSystemFontFamilyForCss(GetConfiguredCandidateEnglishFont()));
    // 皮肤的 font_family 排在最前，用户配置的字体整体退为回退，与 WebView2 端的 font-family 列表同序。
    if (package && !package->fontFamily.empty())
    {
        appearance.fallbackFontFamilies.push_back(appearance.fontFamily);
        appearance.fontFamily = string_to_wstring(ResolveSystemFontFamilyForCss(package->fontFamily));
    }
    for (const auto &font : GetConfiguredCandidateFallbackFontFamilies())
        appearance.fallbackFontFamilies.push_back(string_to_wstring(font));
    appearance.itemHeight = fontSize * 1.35f + tokens.itemExtraHeight;
    appearance.itemGap = tokens.itemGap;
    appearance.fontSize = fontSize;
    appearance.labelFontSize = fontSize * tokens.labelScale;
    appearance.annotationFontSize = fontSize;
    appearance.contentPadLeft = tokens.itemPadLeft;
    appearance.contentPadRight = tokens.itemPadRight;
    // itemExtraHeight 上下各半：横排翻译换到第二行时从下半段内边距上方开始，内边距留在翻译下面，
    // 对应 CSS 里 .row.cand 的 padding 包住整个 .text（杨柳青横排上下各 6px）。
    appearance.contentPadBottom = tokens.itemExtraHeight * 0.5f;
    appearance.textPadLeft = 5.0f;
    appearance.labelGap = tokens.labelGap;
    appearance.minItemWidth = tokens.minItemWidth;
    // 收紧宽度时右侧留白与序号左侧的留白相同，卡片两边对称。
    appearance.fitContent = fitContent_;
    if (fitContent_)
        appearance.contentPadRight = appearance.contentPadLeft + appearance.textPadLeft;
    appearance.selectedBarWidth = tokens.selectedBarWidth;
    appearance.selectedBarHeight = fontSize * tokens.selectedBarHeightEm;
    appearance.selectedBarInside = tokens.selectedBarInside;
    appearance.showSelectedBar = tokens.showSelectedBar;
    appearance.selectedBarColor = tokens.selectedBar.a > 0.001f ? tokens.selectedBar : tokens.accent;
    appearance.cornerRadius = tokens.itemRadius;
    // 预编辑行隐藏时列表顶边才贴着卡片顶边，与 CSS 的 .preedit-hidden 同一判据。
    const bool preeditHidden = GetConfiguredCandidateWindowPreeditStyle() == "empty";
    appearance.outerCornerRadius = tokens.outerItemRadius;
    appearance.outerTopCornersEnabled = preeditHidden;
    appearance.justifyHorizontalRows = tokens.justifyRows;
    // 隐藏的预编辑行高度为 0，但 body 仍在它和列表之间留 2px 行距；贴角皮肤把它抵掉，
    // 让首项贴着卡片的内容顶边（秋桂四边内边距一致，杨柳青高亮直抵卡片顶边）。
    if (preeditHidden)
        impl_->preedit->SetMargin(
            {0.0f, 0.0f, 0.0f, tokens.outerItemRadius > 0.0f || tokens.trimHiddenPreeditGap ? -2.0f : 0.0f});
    else
        impl_->preedit->SetMargin(tokens.preeditMargin);
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
    impl_->menuFill = tokens.menuFill;
    impl_->menuBorder = tokens.menuBorder;
    impl_->menuText = tokens.menuText;
    impl_->menuHover = tokens.menuHover;
    impl_->list->SetAppearance(appearance);
    impl_->list->SetOrientation(GetConfiguredCandidateWindowLayout() == "horizontal"
                                    ? msimeui::CandidateList::Orientation::Horizontal
                                    : msimeui::CandidateList::Orientation::Vertical);
    if (impl_->pager)
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
            // 微软拼音式：候选文字色的实心三角，按钮与候选行等高。横排时按钮更宽，前面隔一条分隔线，
            // 与 CSS 的 .page-arrow 同一组 em 尺寸。
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
        impl_->pager->SetAppearance(pagerAppearance);
        if (horizontalLayout && tokens.trianglePager)
        {
            // 与最后一项隔 0.3em，按钮与行等高，不用再抬。
            impl_->pager->SetMargin({std::round(fontSize * 0.3f), 0.0f, 0.0f, 0.0f});
        }
        else if (horizontalLayout)
        {
            // 横排：窄列与候选之间留一点空，箭头竖直居中在最后一行上。
            const float lift = (std::max)(0.0f, (appearance.itemHeight - pagerAppearance.buttonHeight) * 0.5f);
            impl_->pager->SetMargin({2.0f, 0.0f, 1.0f, lift});
        }
        else
        {
            // 竖排：矮行抵掉 body 的 2px 行距，贴着最后一个候选、靠左，“‹”的左端与序号左端对齐。
            // 序号从候选行的 contentPadLeft + textPadLeft 处开始；箭头在按钮里居中，要减去它左侧的留白。
            impl_->pager->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
            const float labelLeft = appearance.contentPadLeft + appearance.textPadLeft;
            const float glyphInset = (pagerAppearance.buttonWidth - pagerAppearance.GlyphWidth()) * 0.5f;
            impl_->pager->SetMargin({labelLeft - glyphInset, -1.0f, 0.0f, 1.0f});
        }
    }
    impl_->preedit->SetFontFamily(appearance.fontFamily);
    impl_->preedit->SetFallbackFontFamilies(appearance.fallbackFontFamilies);
    impl_->preedit->SetFontSize(preeditSize);
    impl_->preedit->SetColor(palette.preeditText);
    impl_->preedit->SetCaretColor(tokens.preeditCaret.a > 0.001f ? tokens.preeditCaret : tokens.accent);
    impl_->preedit->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    impl_->preedit->SetTextLayoutPadding({5.0f, 0.0f, 5.0f, 0.0f});
    // 预编辑背景与分隔线对应 CSS 的 .pinyin background / border-bottom：底色带与高亮同圆角，分隔线固定 1px；
    // 有其一时预编辑行横向铺满，否则底色只包住文字。
    impl_->preedit->SetBackground(tokens.preeditBackground, tokens.itemRadius);
    impl_->preedit->SetBottomRule(tokens.preeditDivider, 1.0f);
    impl_->preedit->SetHorizontalAlignment(tokens.preeditBackground.a > 0.001f || tokens.preeditDivider.a > 0.001f
                                               ? msimeui::HorizontalAlignment::Stretch
                                               : msimeui::HorizontalAlignment::Leading);

    // 装饰图与 WebView2 端的 .containerParent::before 同一几何：宽 width_dip、高 top_inset_dip 的盒子
    // 底边贴着卡片顶边、不与卡片重叠，按 align 贴卡片左/中/右，图片在盒内等比居中（contain）。
    decorationTopDip_ = 0.0f;
    decorationWidthDip_ = 0.0f;
    cardMinWidthDip_ = fitContent_ ? 0.0f : kCandidateMinWidthDip;
    std::wstring decorationPath;
    std::wstring backgroundPath;
    const std::wstring packageRoot = package ? skinsRoot + L"\\" + string_to_wstring(package->id) + L"\\" : L"";
    if (package)
    {
        decorationTopDip_ = static_cast<float>(package->decorationTopDip);
        decorationWidthDip_ = static_cast<float>(package->decorationWidthDip);
        if (!package->decorationImage.empty())
            decorationPath = packageRoot + string_to_wstring(package->decorationImage);
        if (!package->backgroundImage.empty())
            backgroundPath = packageRoot + string_to_wstring(package->backgroundImage);
        cardMinWidthDip_ =
            (std::max)({cardMinWidthDip_, static_cast<float>(package->minWidthDip), decorationWidthDip_});
    }
    // 卡片上方留给阴影的 kShadowPadTop 与装饰盒重叠，装饰盒高过它时不再额外留白。
    float framePadTop = kShadowPadTop;
    if (decorationPath.empty() || decorationTopDip_ <= 0.0f)
    {
        impl_->decoration->SetHeight(0.0f);
        impl_->decoration->ClearWidth();
        impl_->decoration->SetSource(L"");
    }
    else
    {
        impl_->decoration->SetSource(decorationPath);
        impl_->decoration->SetHeight(decorationTopDip_);
        impl_->decoration->SetWidth(decorationWidthDip_);
        impl_->decoration->SetHorizontalAlignment(DecorationAlignment(package->decorationAlign));
        framePadTop = (std::max)(0.0f, kShadowPadTop - decorationTopDip_);
    }
    // v2 挂件挂在卡片外沿，窗口要按 outer_insets_dip 往四边多留出空白，否则立绘被窗口边界裁掉。
    float framePadLeft = kShadowPadLeft;
    float framePadRight = kShadowPadRight;
    float framePadBottom = kShadowPadBottom;
    if (package && package->outerInsetsDip)
    {
        const CandidateSkinCatalog::InsetsDip &outer = *package->outerInsetsDip;
        framePadLeft += static_cast<float>(outer.left);
        framePadRight += static_cast<float>(outer.right);
        framePadTop += static_cast<float>(outer.top);
        framePadBottom += static_cast<float>(outer.bottom);
    }
    if (impl_->frame)
    {
        impl_->frame->SetPadding({framePadLeft, framePadTop, framePadRight, framePadBottom});
    }

    // v2 自由挂件交给 overlay 层绘制；它测量为 0，不参与卡片布局。
    if (impl_->skinOverlay)
    {
        std::vector<CandidateSkinOverlay::Item> items;
        if (package)
        {
            for (const CandidateSkinCatalog::FreeDecoration &dec : package->freeDecorations)
            {
                CandidateSkinOverlay::Item item;
                item.path = packageRoot + string_to_wstring(dec.image);
                item.anchor = dec.anchor;
                item.offsetXDip = static_cast<float>(dec.offsetXDip);
                item.offsetYDip = static_cast<float>(dec.offsetYDip);
                item.widthDip = static_cast<float>(dec.widthDip);
                item.heightDip = static_cast<float>(dec.heightDip);
                items.push_back(std::move(item));
            }
        }
        impl_->skinOverlay->SetItems(std::move(items));
    }
    // v2 的 line_gap_dip 落到预编辑行与候选列表之间的间距上。
    // TODO(v2 皮肤): 暂定 WebView2 独占。SetSpacing 作用于 body 的全部子元素，竖排时列表与翻页栏之间也会被拉开，
    // WebView2 只加在 .row.pinyin 下方。
    if (impl_->body)
    {
        impl_->body->SetSpacing(package && package->lineGapDip ? static_cast<float>(*package->lineGapDip) : 2.0f);
    }

    msimeui::Brush brush;
    brush.fill = tokens.surface;
    brush.stroke = tokens.border;
    brush.strokeWidth = tokens.borderWidth;
    brush.radiusX = tokens.radius;
    brush.radiusY = tokens.radius;
    if (impl_->card)
    {
        impl_->card->SetBrush(brush);
        if (package && package->contentInsetsDip)
        {
            // v2 的 content_insets_dip 从面板外缘量起，D2D 侧直接落成卡片内边距（这里没有 CSS 的边框内缘概念）。
            const CandidateSkinCatalog::InsetsDip &insets = *package->contentInsetsDip;
            impl_->card->SetPadding(msimeui::Thickness{static_cast<float>(insets.left), static_cast<float>(insets.top),
                                                       static_cast<float>(insets.right),
                                                       static_cast<float>(insets.bottom)});
        }
        else
        {
            impl_->card->SetPadding(tokens.containerPad);
        }
        impl_->card->SetMinWidth(cardMinWidthDip_);
        if (package && package->borderImage && !package->borderImage->source.empty())
        {
            // v2 九宫格：四边原样、其余拉伸。切片按 1/2/4 值展开成「上右下左」。
            // TODO(v2 皮肤): 暂定 WebView2 独占。D2D 尚未处理 width_dip 与 fill = false，九宫格仍叠在卡片底色、
            // 描边与阴影之上并被卡片圆角裁剪；WebView2 会把这些清成透明、圆角归零。
            const CandidateSkinCatalog::BorderImage &bi = *package->borderImage;
            const std::vector<double> &values = bi.stretchDip.empty() ? bi.sliceDip : bi.stretchDip;
            const auto expand4 = [](const std::vector<double> &v, float out[4]) {
                if (v.size() == 1)
                    out[0] = out[1] = out[2] = out[3] = static_cast<float>(v[0]);
                else if (v.size() == 2)
                {
                    out[0] = out[2] = static_cast<float>(v[0]);
                    out[1] = out[3] = static_cast<float>(v[1]);
                }
                else
                {
                    for (size_t i = 0; i < 4; ++i)
                        out[i] = i < v.size() ? static_cast<float>(v[i]) : 0.0f;
                }
            };
            float slice[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            expand4(values, slice);
            impl_->card->SetBackgroundImageNineSlice(packageRoot + string_to_wstring(bi.source), slice[3], slice[0],
                                                     slice[1], slice[2], 1.0f);
        }
        else
        {
            impl_->card->SetBackgroundImage(backgroundPath, BackgroundStretch(package ? package->backgroundFit : ""),
                                            package ? static_cast<float>(package->backgroundOpacity) : 1.0f);
        }
    }
}

std::uint64_t CandidatePresenter::FillItemsFromUi()
{
    const Global::CandidatePageSnapshotPtr page = Global::LoadCandidatePageSnapshot();
    const bool translationsEnabled = GetConfiguredCandidateTranslationsEnabled();
    std::vector<msimeui::CandidateList::Item> items;
    items.reserve(page->page_views.size());
    for (size_t i = 0; i < page->page_views.size(); ++i)
    {
        const auto &view = page->page_views[i];
        msimeui::CandidateList::Item item;
        item.label = std::to_wstring(i + 1);
        item.text = string_to_wstring(view.text + view.badge);
        item.annotation = string_to_wstring(view.annotation);
        if (translationsEnabled)
        {
            // 逐段转成 UTF-16 再拼起来，区间按拼出来的串计，与 WebView2 的 .cand-pos / .cand-sep 同一切法。
            for (const auto &segment : FanyImeIpc::SegmentTranslationGloss(view.translation))
            {
                const std::wstring text = string_to_wstring(segment.text);
                if (segment.kind != FanyImeIpc::TranslationSegmentKind::Text)
                {
                    item.translationRuns.push_back({static_cast<UINT32>(item.translation.size()),
                                                    static_cast<UINT32>(text.size()),
                                                    segment.kind == FanyImeIpc::TranslationSegmentKind::Separator
                                                        ? msimeui::CandidateList::TextRunRole::Delimiter
                                                        : msimeui::CandidateList::TextRunRole::Secondary});
                }
                item.translation += text;
            }
        }
        items.push_back(std::move(item));
    }
    ignoreSelectionCallback_ = true;
    impl_->list->SetItems(std::move(items));
    impl_->list->SetSelectedIndex(static_cast<size_t>((std::max)(0, page->selected_index_in_page)));
    ignoreSelectionCallback_ = false;
    if (impl_->pager)
    {
        impl_->pager->SetEnabled(page->has_previous_page, page->has_next_page);
    }
    return page->generation;
}

void CandidatePresenter::CommitItem(size_t pageIndex)
{
    if (!hwnd_)
    {
        return;
    }
    PostMessageW(hwnd_, WM_COMMIT_CANDIDATE, static_cast<WPARAM>(pageIndex + 1), 0);
}

namespace
{
constexpr float kContextMenuWidthDip = 108.0f;
constexpr float kContextSubmenuWidthDip = 96.0f;

void StyleContextPopup(msimeui::Popup &popup, const D2D1_COLOR_F &fill, const D2D1_COLOR_F &border, float width)
{
    popup.SetMatchAnchorWidth(false);
    popup.SetWidth(width);
    popup.SetPadding({2.0f, 2.0f, 2.0f, 2.0f});
    popup.SetBackgroundFill(fill);
    popup.SetBorderColor(border);
    popup.SetCornerRadius(6.0f);
    popup.SetShadowEnabled(true);
    popup.SetOffset(0.0f, 0.0f);
    popup.SetConstrainToViewport(true);
}
} // namespace

void CandidatePresenter::ShowItemContextMenu(size_t pageIndex, POINT clientPoint)
{
    if (!hwnd_ || !impl_ || !impl_->list || !impl_->window)
    {
        return;
    }

    CloseContextMenu(true);
    impl_->contextMenuPageIndex = pageIndex;

    // A mouse callback has no posted-message handoff at all, so it must not walk the worker's live page either.
    const Global::CandidatePageSnapshotPtr page = Global::LoadCandidatePageSnapshot();
    std::wstring word;
    if (pageIndex < page->page_words.size())
    {
        word = page->page_words[pageIndex];
    }
    size_t codePoints = 0;
    for (size_t i = 0; i < word.size(); ++i)
    {
        ++codePoints;
        if (i + 1 < word.size() && IS_HIGH_SURROGATE(word[i]) && IS_LOW_SURROGATE(word[i + 1]))
        {
            ++i;
        }
    }
    const bool showDelete = codePoints != 1;
    const WPARAM oneBased = static_cast<WPARAM>(pageIndex + 1);

    auto makeItem = [this](const std::wstring &text, bool hasSubmenu, std::function<void()> onClick) {
        auto item = std::make_shared<msimeui::MenuFlyoutItem>(text, hasSubmenu);
        item->SetColors(impl_->menuText, impl_->menuHover);
        if (onClick)
        {
            item->SetOnClick(std::move(onClick));
        }
        return item;
    };

    auto stack = std::make_shared<msimeui::StackPanel>(0.0f);
    auto pin = makeItem(L"置顶", false, [this, oneBased]() {
        CloseContextMenu(true);
        PostMessageW(hwnd_, WM_PIN_TO_TOP_CANDIDATE, oneBased, 0);
    });
    pin->SetOnHover([this](bool hovered) {
        if (hovered)
        {
            CloseFixSubmenu();
        }
    });
    impl_->fixPositionItem = makeItem(L"固定排位", true, {});
    impl_->fixPositionItem->SetOnHover([this](bool hovered) {
        if (hovered)
        {
            OpenFixSubmenu();
        }
    });
    stack->AddChild(pin);
    stack->AddChild(impl_->fixPositionItem);
    if (showDelete)
    {
        auto del = makeItem(L"删除", false, [this, oneBased]() {
            CloseContextMenu(true);
            PostMessageW(hwnd_, WM_DELETE_CANDIDATE, oneBased, 0);
        });
        del->SetOnHover([this](bool hovered) {
            if (hovered)
            {
                CloseFixSubmenu();
            }
        });
        stack->AddChild(del);
    }

    auto subStack = std::make_shared<msimeui::StackPanel>(0.0f);
    for (int position = 1; position <= 5; ++position)
    {
        const std::wstring label = L"第 " + std::to_wstring(position) + L" 位";
        subStack->AddChild(makeItem(label, false, [this, oneBased, position]() {
            CloseContextMenu(true);
            PostMessageW(hwnd_, WM_FIX_CANDIDATE_POSITION, oneBased, position);
        }));
    }
    auto separator = std::make_shared<msimeui::MenuSeparator>();
    separator->SetColor(D2D1::ColorF(impl_->menuText.r, impl_->menuText.g, impl_->menuText.b, 0.125f));
    subStack->AddChild(separator);
    subStack->AddChild(makeItem(L"取消固定", false, [this, oneBased]() {
        CloseContextMenu(true);
        PostMessageW(hwnd_, WM_CLEAR_CANDIDATE_POSITION, oneBased, 0);
    }));

    impl_->contextMenu = std::make_shared<msimeui::Popup>(stack);
    StyleContextPopup(*impl_->contextMenu, impl_->menuFill, impl_->menuBorder, kContextMenuWidthDip);
    impl_->contextSubmenu = std::make_shared<msimeui::Popup>(subStack);
    StyleContextPopup(*impl_->contextSubmenu, impl_->menuFill, impl_->menuBorder, kContextSubmenuWidthDip);

    const msimeui::PointF anchor = impl_->window->ClientPixelsToDips(clientPoint);
    impl_->contextMenu->SetAnchorRect({anchor.x, anchor.y, 1.0f, 1.0f});
    ExpandHostForMenu(clientPoint);

    if (msimeui::Scene *scene = impl_->window->GetScene())
    {
        scene->AddPopup(impl_->contextMenu, [this]() { CloseContextMenu(true); });
        impl_->contextMenuOpen = true;
    }
    Present();
}

void CandidatePresenter::CloseContextMenu(bool restoreHost)
{
    if (!impl_ || !impl_->window)
    {
        return;
    }

    // ShowFromGlobalState and Hide call this on every keystroke, so the hover
    // teardown and the repaint below only run when a menu is actually up.
    const bool hadPopup = impl_->contextMenu || impl_->contextSubmenu;
    if (hadPopup)
    {
        // Window caches the hovered and focused Visual as raw pointers. They
        // point into the popup destroyed a few lines below, so they have to be
        // dropped first or the next mouse message dispatches through a dangling
        // pointer.
        impl_->window->DispatchImportedMessage(WM_MOUSELEAVE, 0, 0);
        impl_->window->FocusVisual(nullptr);
    }

    if (msimeui::Scene *scene = impl_->window->GetScene())
    {
        if (impl_->contextSubmenu)
        {
            scene->RemovePopup(impl_->contextSubmenu.get(), false);
        }
        if (impl_->contextMenu)
        {
            scene->RemovePopup(impl_->contextMenu.get(), false);
        }
    }
    impl_->contextMenuOpen = false;
    impl_->contextSubmenuOpen = false;
    impl_->fixPositionItem.reset();
    impl_->contextMenu.reset();
    impl_->contextSubmenu.reset();
    if (restoreHost)
    {
        RestoreHostAfterMenu();
    }
    else
    {
        impl_->hostExpandedForMenu = false;
    }
    if (hadPopup)
    {
        Present();
    }
}

void CandidatePresenter::OpenFixSubmenu()
{
    if (!impl_ || !impl_->window || !impl_->contextSubmenu || !impl_->fixPositionItem || impl_->contextSubmenuOpen)
    {
        return;
    }
    const msimeui::RectF anchor = impl_->fixPositionItem->GetBounds();
    if (anchor.width < 1.0f || anchor.height < 1.0f)
    {
        return;
    }
    impl_->contextSubmenu->SetAnchorRect(anchor);
    impl_->contextSubmenu->SetOffset(anchor.width - 2.0f, -anchor.height);
    if (msimeui::Scene *scene = impl_->window->GetScene())
    {
        scene->AddPopup(impl_->contextSubmenu, [this]() { impl_->contextSubmenuOpen = false; });
        impl_->contextSubmenuOpen = true;
    }
}

void CandidatePresenter::CloseFixSubmenu()
{
    if (!impl_ || !impl_->window || !impl_->contextSubmenu || !impl_->contextSubmenuOpen)
    {
        return;
    }
    // The submenu visual outlives this call (contextSubmenu keeps it alive for
    // a later reopen), but it leaves the scene here, so the cached hover has to
    // go with it. Focus stays where it is: the parent menu is still open.
    impl_->window->DispatchImportedMessage(WM_MOUSELEAVE, 0, 0);
    if (msimeui::Scene *scene = impl_->window->GetScene())
    {
        scene->RemovePopup(impl_->contextSubmenu.get(), false);
    }
    impl_->contextSubmenuOpen = false;
    Present();
}

void CandidatePresenter::ExpandHostForMenu(POINT clientPoint)
{
    if (!hwnd_ || !impl_ || !impl_->window)
    {
        return;
    }
    RECT windowRect{};
    RECT clientRect{};
    GetWindowRect(hwnd_, &windowRect);
    GetClientRect(hwnd_, &clientRect);
    if (!impl_->hostExpandedForMenu)
    {
        impl_->hostRectBeforeMenu = windowRect;
        impl_->hostExpandedForMenu = true;
    }

    const float dpi = impl_->window->GetDpi();
    const msimeui::PointF anchor = impl_->window->ClientPixelsToDips(clientPoint);
    const msimeui::SizeF clientDip = impl_->window->ClientPixelsToDips(SIZE{clientRect.right, clientRect.bottom});
    const float needWidth =
        (std::max)(clientDip.width, anchor.x + kContextMenuWidthDip + kContextSubmenuWidthDip + 16.0f);
    const float needHeight = (std::max)(clientDip.height, (std::max)(anchor.y + 168.0f, 176.0f));
    const int widthPx = (std::max)(static_cast<int>(windowRect.right - windowRect.left),
                                   static_cast<int>(std::ceil(needWidth * dpi / 96.0f)));
    const int heightPx = (std::max)(static_cast<int>(windowRect.bottom - windowRect.top),
                                    static_cast<int>(std::ceil(needHeight * dpi / 96.0f)));
    if (widthPx == windowRect.right - windowRect.left && heightPx == windowRect.bottom - windowRect.top)
    {
        return;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, windowRect.left, windowRect.top, widthPx, heightPx,
                 SWP_NOACTIVATE | SWP_NOZORDER);
}

void CandidatePresenter::RestoreHostAfterMenu()
{
    if (!hwnd_ || !impl_ || !impl_->hostExpandedForMenu)
    {
        return;
    }
    const RECT &rc = impl_->hostRectBeforeMenu;
    SetWindowPos(hwnd_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    impl_->hostExpandedForMenu = false;
}

void CandidatePresenter::PlaceAndShow(POINT caret, float widthDip, float heightDip, float cardLeftDip, float cardTopDip,
                                      const ResolvedCandidateScale &resolved)
{
    FLOAT scale = resolved.scale;
    CandidateScaleSource scaleSource = resolved.source;
    if (scale <= 0.0f)
    {
        const ResolvedCandidateScale now = ResolveCandidateScaleForCaret(caret);
        scale = now.scale;
        scaleSource = now.source;
    }
    if (scale <= 0.0f)
    {
        scale = 1.0f;
        scaleSource = CandidateScaleSource::Monitor;
    }
    const HalfScreenDipLimits limits = ApplyScaleToHalfScreenLimits(caret, scale);
    widthDip = static_cast<float>(ClampWidthDipToHalfScreen(widthDip, limits));
    heightDip = static_cast<float>(ClampHeightDipToHalfScreen(heightDip, limits));
    lastLayoutWidthDip_ = widthDip;
    lastLayoutHeightDip_ = heightDip;
    cardLeftDip = (std::max)(0.0f, cardLeftDip);
    cardTopDip = (std::max)(0.0f, (std::min)(cardTopDip, heightDip));
    float cardWidthDip = (std::max)(widthDip - cardLeftDip - kShadowPadRight, 1.0f);
    float cardHeightDip = (std::max)(heightDip - cardTopDip - kShadowPadBottom, 1.0f);
    if (impl_->card)
    {
        const msimeui::RectF card = impl_->card->GetBounds();
        if (card.width > 1.0f)
        {
            cardWidthDip = card.width;
        }
        if (card.height > 1.0f)
        {
            cardHeightDip = card.height;
        }
    }
    auto properPos = std::make_shared<std::pair<int, int>>();
    // Position the opaque card, not the decoration or drop shadow around it.
    AdjustCandidateWindowPosition(&caret, {cardWidthDip, cardHeightDip}, properPos, scale, cardWidthDip);
    int widthPx = AlignHostPixels((std::max)(1, static_cast<int>(std::ceil(widthDip * scale))));
    int heightPx = AlignHostPixels((std::max)(1, static_cast<int>(std::ceil(heightDip * scale))));
    widthPx = (std::max)(widthPx, lastHostWidthPx_);
    heightPx = (std::max)(heightPx, lastHostHeightPx_);
    lastHostWidthPx_ = widthPx;
    lastHostHeightPx_ = heightPx;
    const int cardLeftPx = static_cast<int>(std::lround(cardLeftDip * scale));
    const int cardTopPx = static_cast<int>(std::lround(cardTopDip * scale));
    const int cardHeightPx = static_cast<int>(std::lround(cardHeightDip * scale));
    int x = properPos->first - cardLeftPx;
    int y = properPos->second - cardTopPx;
    const MonitorCoordinates monitor = GetMonitorCoordinatesFromPoint(caret);
    // Clamp the opaque card, not the host window — same rule the vertical branch
    // below already follows. The host carries transparent shadow padding, is
    // rounded up to a 64px bucket and only ever grows while typing, so clamping
    // *its* right edge shoved the card left by all of that slack. AdjustCandidate-
    // WindowPosition had already parked the card flush against the screen edge;
    // deleting a character kept the stale (larger) host width, so the card's left
    // edge froze and its right edge drifted further from the edge on every delete.
    const int cardWidthPx = (std::max)(1, static_cast<int>(std::ceil(cardWidthDip * scale)));
    if (x + cardLeftPx + cardWidthPx > monitor.right)
    {
        x = monitor.right - cardLeftPx - cardWidthPx - 2;
    }
    if (x + cardLeftPx < monitor.left)
    {
        x = monitor.left + 2 - cardLeftPx;
    }
    const int cardBottom = y + cardTopPx + cardHeightPx;
    if (cardBottom > monitor.bottom)
    {
        y = monitor.bottom - cardTopPx - cardHeightPx - 2;
    }
    // Allow the mascot to clip above the work area so the card stays on the caret.
    if (y + cardTopPx < monitor.top)
    {
        y = monitor.top + 2 - cardTopPx;
    }
    // Rendering must use the same scale the window is sized with: the ui
    // override replaces GetDpiForWindow (144 inside RDP) so DIPs map exactly
    // onto the pixels computed above. Must land before EnsureForComposition —
    // a freshly created target reads it through DpiForHwnd() — and before the
    // resize below, whose WM_SIZE already presents.
    const FLOAT dpiOverride = scale * 96.0f;
    impl_->window->SetDpiOverride(dpiOverride);
    impl_->resources.SetDpiOverride(dpiOverride);
    // The card is clamped to the work area, but the host around it carries the
    // shadow padding and the 64px bucket slack, so near the right or bottom edge
    // it reached onto the neighbouring monitor. DWM applies a window move and a
    // swap-chain present independently and in no guaranteed order, so a frame
    // can pair either card with either host origin; whatever it put past the
    // card then flashed on the next screen. Cut the host at the work-area edge:
    // nothing can be drawn outside the caret's monitor, whatever DWM pairs up.
    // The card still fits (it was clamped above); only its shadow is trimmed.
    const int hostWidthPx = (std::max)(1, (std::min)(widthPx, monitor.right - x));
    const int hostHeightPx = (std::max)(1, (std::min)(heightPx, monitor.bottom - y));
    // Submission order still decides which stale pairing is likelier. Moving a
    // host left/up before the present, and right/down after it, keeps the
    // stale frame on the side of the card that is already on screen.
    RECT current{};
    DWORD cloaked = 0;
    const bool onScreen =
        IsWindowVisible(hwnd_) && GetWindowRect(hwnd_, &current) && current.top != Global::INVALID_Y &&
        SUCCEEDED(DwmGetWindowAttribute(hwnd_, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked == 0;
    const int firstX = onScreen && x > current.left ? current.left : x;
    const int firstY = onScreen && y > current.top ? current.top : y;
    // WM_SIZE from this resize would present the frame once more right before
    // the explicit Present below; let it only resize the swap chain.
    placingHost_ = true;
    SetWindowPos(hwnd_, HWND_TOPMOST, firstX, firstY, hostWidthPx, hostHeightPx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    placingHost_ = false;
    impl_->resources.EnsureForComposition(hwnd_);
    Present();
    // Every setter above invalidated the window, which queued a WM_PAINT that
    // would render the whole card (shadow blur included) a second time. The
    // frame just presented is already current.
    ValidateRect(hwnd_, nullptr);
    if (firstX != x || firstY != y)
    {
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
    }
    SetCandidateHostCloaked(false);
    // Acceptance trace for the RDP candidate-scale fix: which scale authority
    // won, and the system's own (potentially diverging) DPI values. Coordinates
    // and DPI only — never user input, per the diagnostic logging red lines.
    CAND_DIAG_LOGF(
        L"candidate-d2d place source={} scale={:.3f} hwnd_dpi={} system_dpi={} remote={} caret=({},{}) "
        L"size_px=({},{}) host=({},{}) first=({},{}) on_screen={} card_px=({},{} {}w) monitor=({},{})-({},{})",
        scaleSource == CandidateScaleSource::RdpForeground ? L"rdp-foreground" : L"monitor", scale,
        GetDpiForWindow(hwnd_), GetDpiForSystem(), GetSystemMetrics(SM_REMOTESESSION) ? 1 : 0, caret.x, caret.y,
        hostWidthPx, hostHeightPx, x, y, firstX, firstY, onScreen, x + cardLeftPx, y + cardTopPx, cardWidthPx,
        monitor.left, monitor.top, monitor.right, monitor.bottom);
}

void CandidatePresenter::ShowFromGlobalState()
{
    ShowFromGlobalState(POINT{Global::Point[0], Global::Point[1]});
}

void CandidatePresenter::ShowFromGlobalState(POINT caret)
{
    if (!bound_ || !hwnd_ || !impl_ || !impl_->root)
    {
        return;
    }
    // A composition can start before the host reports a usable text extent
    // (first focus, or resuming after a long idle). The caret then arrives as
    // (0, INVALID_Y), which placement would clamp into the monitor's work area.
    // Hide even an already-visible host until a real anchor arrives. Preserve
    // the show request so MoveCandidate / the settle timer can place it later.
    if (caret.y == Global::INVALID_Y)
    {
        CloseContextMenu(false);
        SetCandidateHostCloaked(true);
        ::is_global_wnd_cand_shown = true;
        Global::SetCandidateWindowRenderedVisible(true);
        CAND_DIAG_LOGF(L"candidate-d2d show deferred: no usable caret anchor ({},{})", caret.x, caret.y);
        return;
    }
    CloseContextMenu(false);
    ApplySkin();
    std::wstring preedit;
    size_t caretIndex = 0;
    if (GetConfiguredCandidateWindowPreeditStyle() != "empty")
    {
        preedit = GetPreeditWithCaretMarker();
        const size_t marker = preedit.find(L'\uE000');
        if (marker != std::wstring::npos)
        {
            caretIndex = marker;
            preedit.erase(marker, 1);
        }
        else
        {
            caretIndex = preedit.size();
        }
    }
    impl_->preedit->SetText(preedit);
    if (preedit.empty() && GetConfiguredCandidateWindowPreeditStyle() == "empty")
    {
        impl_->preedit->ClearCaret();
        impl_->preedit->SetHeight(0.0f);
    }
    else
    {
        impl_->preedit->ClearHeight();
        impl_->preedit->SetCaretIndex(caretIndex);
    }
    const std::uint64_t renderedGeneration = FillItemsFromUi();
    impl_->list->SetHoverEnabled(hoverArmed_);
    if (impl_->pager)
    {
        impl_->pager->SetHoverEnabled(hoverArmed_);
    }
    // Resolve the scale once per show and thread it through measure, clamping,
    // window sizing and rendering DPI — a foreground window changing mid-show
    // must not split measure (clamped at one scale) from placement (another).
    const ResolvedCandidateScale scale = ResolveCandidateScaleForCaret(caret);
    const HalfScreenDipLimits limits = ApplyScaleToHalfScreenLimits(caret, scale.scale);
    const float maxW = limits.maxWidthDip > 1.0 ? static_cast<float>(limits.maxWidthDip) : 480.0f;
    const float maxH = limits.maxHeightDip > 1.0 ? static_cast<float>(limits.maxHeightDip) : 640.0f;
    float widthDip = 0.0f;
    float heightDip = 0.0f;
    auto layoutRoot = [&]() {
        impl_->root->InvalidateMeasure();
        const msimeui::SizeF measured = impl_->root->MeasureInLayout({maxW, maxH});
        widthDip = (std::max)(measured.width, cardMinWidthDip_ + kShadowPadLeft + kShadowPadRight);
        heightDip = (std::max)(measured.height, 36.0f);
        impl_->root->InvalidateArrange();
        impl_->root->ArrangeInLayout({0.0f, 0.0f, widthDip, heightDip});
    };
    // Late additions to the page (translations, cloud/AI/English merges) change
    // the card's size between two frames of the same input, and the eye reads
    // that as flicker even though every frame is complete. For one input
    // (preedit + caret + page) the card therefore only grows; it shrinks back
    // only when the natural size falls well below the kept one (a page that
    // really got smaller). Another keystroke, caret move or page turn is a
    // different page and starts from its natural size, as does Hide();
    // carrying the old size over leaves blank rows at the bottom.
    if (impl_->card)
    {
        impl_->card->SetMinWidth(cardMinWidthDip_);
        impl_->card->ClearMinHeight();
    }
    layoutRoot();
    if (impl_->card)
    {
        std::wstring stickyKey = GetPreeditWithCaretMarker();
        stickyKey += L'#';
        stickyKey += std::to_wstring(Global::candidate_ui.page_index);
        if (stickyKey != stickyCardKey_)
        {
            stickyCardKey_ = std::move(stickyKey);
            stickyCardWidthDip_ = 0.0f;
            stickyCardHeightDip_ = 0.0f;
        }
        const msimeui::RectF natural = impl_->card->GetBounds();
        const float maxCardWidthDip = (std::max)(cardMinWidthDip_, maxW - kShadowPadLeft - kShadowPadRight);
        // fit_content 的卡片严格跟随文字宽度，只保留高度的只增不减。
        stickyCardWidthDip_ = fitContent_
                                  ? natural.width
                                  : (std::min)(StickyCardExtent(stickyCardWidthDip_, natural.width), maxCardWidthDip);
        stickyCardHeightDip_ = StickyCardExtent(stickyCardHeightDip_, natural.height);
        if (stickyCardWidthDip_ > natural.width + 0.5f || stickyCardHeightDip_ > natural.height + 0.5f)
        {
            impl_->card->SetMinWidth((std::max)(cardMinWidthDip_, stickyCardWidthDip_));
            impl_->card->SetMinHeight(stickyCardHeightDip_);
            layoutRoot();
        }
    }
    hoverArmed_ = false;
    if (!GetCursorPos(&hoverBaseline_))
    {
        hoverBaseline_ = {};
    }
    float cardLeftDip = 0.0f;
    float cardTopDip = 0.0f;
    if (impl_->card)
    {
        const msimeui::RectF card = impl_->card->GetBounds();
        cardLeftDip = card.x;
        cardTopDip = card.y;
    }
    PlaceAndShow(caret, widthDip, heightDip, cardLeftDip, cardTopDip, scale);
    ::is_global_wnd_cand_shown = true;
    Global::SetCandidateWindowRenderedVisible(true);
    // PlaceAndShow paints synchronously, so the page is on screen now. Echo the generation of the
    // snapshot that was actually rendered (not the latest published one) so a pending digit/space
    // selection can settle against the same page the user is looking at.
    Global::PublishRenderedCandidatePageGeneration(renderedGeneration);
}

void CandidatePresenter::Hide()
{
    if (!hwnd_)
    {
        return;
    }
    CloseContextMenu(false);
    ::is_global_wnd_cand_shown = false;
    Global::SetCandidateWindowRenderedVisible(false);
    hoverArmed_ = false;
    wheelDeltaAccumulator_ = 0;
    if (impl_ && impl_->list)
    {
        impl_->list->SetHoverEnabled(false);
    }
    if (impl_ && impl_->pager)
    {
        impl_->pager->SetHoverEnabled(false);
    }
    // Cloaking keeps the swap chain's last frame. The next show presents and
    // then uncloaks, but DWM may compose the uncloaked host before it latches
    // that present: one frame of the previous card at the new origin. After a
    // wide card, a narrower one clamped to the right screen edge sits further
    // right, so the stale card spilled onto the next monitor. Leave nothing
    // behind to show.
    PresentEmptyFrame();
    SetCandidateHostCloaked(true);
    lastHostWidthPx_ = 0;
    lastHostHeightPx_ = 0;
    stickyCardKey_.clear();
    stickyCardWidthDip_ = 0.0f;
    stickyCardHeightDip_ = 0.0f;
    SetWindowPos(hwnd_, nullptr, 0, Global::INVALID_Y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CandidatePresenter::Present()
{
    if (!bound_ || !hwnd_ || !impl_ || !impl_->window)
    {
        return;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const UINT width = static_cast<UINT>((std::max)(1L, rc.right));
    const UINT height = static_cast<UINT>((std::max)(1L, rc.bottom));
    if (!impl_->resources.EnsureForComposition(hwnd_))
    {
        return;
    }
    impl_->resources.Resize(width, height);
    ID2D1RenderTarget *target = impl_->resources.GetRenderTarget();
    if (!target)
    {
        return;
    }
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0f));
    if (msimeui::Scene *scene = impl_->window->GetScene())
    {
        const float dpi = impl_->window->GetDpi();
        float layoutW = msimeui::PixelsToDips(static_cast<float>(width), dpi);
        float layoutH = msimeui::PixelsToDips(static_cast<float>(height), dpi);
        // Host HWND is larger than the card (shadow padding, 64px buckets, and a
        // grow-only size while typing). Stretching the scene to that HWND fills
        // the rounded card with empty space. Keep layout at the measured DIP size.
        if (!impl_->hostExpandedForMenu && lastLayoutWidthDip_ > 0.0f && lastLayoutHeightDip_ > 0.0f)
        {
            layoutW = lastLayoutWidthDip_;
            layoutH = lastLayoutHeightDip_;
        }
        scene->EnsureLayout({layoutW, layoutH});
        scene->Render(impl_->resources);
    }
    const HRESULT hr = target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        impl_->resources.DiscardTarget();
        return;
    }
    impl_->resources.Present();
}

void CandidatePresenter::PresentEmptyFrame()
{
    if (!bound_ || !impl_)
    {
        return;
    }
    ID2D1RenderTarget *target = impl_->resources.GetRenderTarget();
    if (!target)
    {
        return;
    }
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0f));
    if (target->EndDraw() == D2DERR_RECREATE_TARGET)
    {
        impl_->resources.DiscardTarget();
        return;
    }
    impl_->resources.Present();
}

void CandidatePresenter::ArmHoverIfPointerMoved()
{
    POINT now{};
    if (!GetCursorPos(&now))
    {
        return;
    }
    const int dx = now.x - hoverBaseline_.x;
    const int dy = now.y - hoverBaseline_.y;
    if (dx * dx + dy * dy < 4)
    {
        return;
    }
    hoverArmed_ = true;
    if (impl_ && impl_->list)
    {
        impl_->list->SetHoverEnabled(true);
    }
    if (impl_ && impl_->pager)
    {
        impl_->pager->SetHoverEnabled(true);
    }
}

bool CandidatePresenter::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!bound_ || !impl_ || !impl_->window)
    {
        return false;
    }
    switch (message)
    {
    case WM_ERASEBKGND:
        return true;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Present();
        EndPaint(hwnd_, &ps);
        return true;
    }
    case WM_SIZE:
        if (::is_global_wnd_cand_shown)
        {
            impl_->resources.Resize(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)));
            if (!placingHost_)
            {
                Present();
            }
        }
        return true;
    case WM_MOUSEMOVE:
        ArmHoverIfPointerMoved();
        if (!hoverArmed_ && !(impl_ && impl_->contextMenuOpen))
        {
            return true;
        }
        impl_->window->DispatchImportedMessage(message, wParam, lParam);
        Present();
        return true;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MOUSELEAVE:
        impl_->window->DispatchImportedMessage(message, wParam, lParam);
        Present();
        return true;
    case WM_MOUSEWHEEL: {
        if (!GetConfiguredPagingMouseWheelEnabled())
        {
            // Leave the message to DefWindowProc, exactly as before the feature
            // existed. WM_PAGE_CANDIDATE checks the same setting for both hosts;
            // this early-out only keeps a disabled wheel from being swallowed.
            wheelDeltaAccumulator_ = 0;
            return false;
        }
        if (!::is_global_wnd_cand_shown)
        {
            // Hide() only parks the host off-screen, so a wheel message posted
            // just before the page went away can still arrive here.
            wheelDeltaAccumulator_ = 0;
            return true;
        }
        if (impl_->contextMenuOpen)
        {
            // Scrolling dismisses the open context menu and consumes the event.
            wheelDeltaAccumulator_ = 0;
            CloseContextMenu(true);
            return true;
        }
        const CandidateWheel::PagingSteps steps =
            CandidateWheel::ConsumeWheelDelta(wheelDeltaAccumulator_, GET_WHEEL_DELTA_WPARAM(wParam), WHEEL_DELTA);
        // Route through the host WndProc like every other candidate UI action
        // instead of reaching into the IPC layer from the window layer.
        if (steps.page_up > 0)
        {
            PostMessageW(hwnd_, WM_PAGE_CANDIDATE, CANDIDATE_PAGE_PREVIOUS, steps.page_up);
        }
        if (steps.page_down > 0)
        {
            PostMessageW(hwnd_, WM_PAGE_CANDIDATE, CANDIDATE_PAGE_NEXT, steps.page_down);
        }
        return true;
    }
    default:
        return false;
    }
}

CandidatePresenter::~CandidatePresenter() = default;
