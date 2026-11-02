#include "window/candidate_presenter.h"

#include "config/ime_config.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include "global/globals.h"
#include "ipc/candidate_translation_policy.h"
#include "ipc/candidate_expansion_panel_policy.h"
#include "ipc/event_listener.h"
#include "ipc/ipc.h"
#include "log/candidate_diag_log.h"
#include "skin/candidate_skin_catalog.h"
#include "window/candidate_skin_layers.h"
#include "window/candidate_skin_overlay.h"
#include "utils/common_utils.h"
#include "utils/ime_utils.h"
#include "utils/window_utils.h"
#include "window/small_windows.h"
#include "window/candidate_skin_palette.h"
#include "window/candidate_skin_scene.h"
#include "window/candidate_skin_tokens.h"
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
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <new>
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
// monitor-derived scale, so clamping and window sizing stay on one scale.
// 横排强制单行时宽度上限放开到整个屏幕宽度，横排列表只在超出屏幕时才换行。
HalfScreenDipLimits ApplyScaleToHalfScreenLimits(POINT pt, FLOAT scale)
{
    HalfScreenDipLimits limits = QueryHalfScreenDipLimitsForPoint(pt);
    const double widthFraction =
        GetConfiguredCandidateWindowSingleLine() && GetConfiguredCandidateWindowLayout() == "horizontal" ? 1.0 : 0.5;
    const double monitorWidthPx = static_cast<double>((std::max)(1, limits.monitor.right - limits.monitor.left));
    if (scale > 0.0f)
    {
        const double monitorHeightPx = static_cast<double>((std::max)(1, limits.monitor.bottom - limits.monitor.top));
        limits.scale = scale;
        limits.maxHeightDip = (monitorHeightPx * 0.5) / static_cast<double>(scale);
    }
    limits.maxWidthDip = (monitorWidthPx * widthFraction) / static_cast<double>(limits.scale);
    return limits;
}

std::wstring AssetRoot()
{
    return CommonUtils::get_ime_data_path_w();
}

constexpr float kShadowPadLeft = 32.0f;
constexpr float kShadowPadTop = 20.0f;
constexpr float kShadowPadRight = 32.0f;
constexpr float kShadowPadBottom = 40.0f;
// Below this fraction of the kept extent, the page really got smaller (fewer or
// shorter candidates) and the card follows it; above it, the difference is a
// late addition coming or going and the card keeps its size.
constexpr float kStickyCardShrinkRatio = 0.7f;

float StickyCardExtent(float kept, float natural)
{
    return natural >= kept || natural < kept * kStickyCardShrinkRatio ? natural : kept;
}

// 候选窗 HWND 上的动图帧定时器。宿主自己的定时器编号见 ime_windows_internal.h（2–14），这里避开它们。
constexpr UINT_PTR kCandidateAnimationTimerId = 0x0A11;

// 系统「透明效果」（设置 → 个性化 → 颜色）。关掉时系统材质一律退回纯色，皮肤的亚克力也跟着退回不透明底色。
// 读不到时按开启处理。
bool SystemTransparencyEnabled()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
    {
        return true;
    }
    return value != 0;
}

} // namespace

struct CandidatePresenter::Impl
{
    msimeui::DeviceResources resources;
    std::unique_ptr<msimeui::Window> window;
    // 整棵候选框场景与套皮肤的逻辑（与设置页的皮肤预览共用）。下面几个是它里面的控件，候选窗要直接摆弄；
    // 场景结构（排列方式、翻页箭头、卷轴展开）变了它们就换成新建的那一批（见 RebuildScene）。
    CandidateSkinScene scene;
    std::shared_ptr<msimeui::StackPanel> root;
    std::shared_ptr<msimeui::Card> card;
    std::shared_ptr<msimeui::TextBlock> preedit;
    std::shared_ptr<msimeui::CandidateList> list;
    // 皮肤关掉翻页箭头时为空。
    std::shared_ptr<msimeui::PagerArrows> pager;
    // 卷轴展开皮肤（横排）才有这几样，其余皮肤为空。开了展开就不放翻页箭头。
    std::shared_ptr<CandidateExpandToggle> expandToggle;
    std::shared_ptr<msimeui::CandidateList> expandGrid;
    std::shared_ptr<CandidateExpandContent> expandContent;
    std::shared_ptr<msimeui::ScrollViewer> expandScroller;
    std::shared_ptr<CandidateExpandBody> expandBody;
    std::shared_ptr<msimeui::Popup> contextMenu;
    std::shared_ptr<msimeui::Popup> contextSubmenu;
    std::shared_ptr<msimeui::MenuFlyoutItem> fixPositionItem;
    // 候选翻译竖排面板（展开键）：挂在高亮候选下方的 Popup，内容是一个只放译义文本的竖排
    // CandidateList。它不进场景树（root），走 Scene 的 popup 通道；每帧按发布快照重建。
    std::shared_ptr<msimeui::Popup> translationPanel;
    std::shared_ptr<msimeui::CandidateList> translationPanelList;
    // 本次显示实际画出来的那一页快照；面板与列表必须来自同一页，不能各自再读一次。
    Global::CandidatePageSnapshotPtr renderedSnapshot;
    D2D1_COLOR_F menuFill = D2D1::ColorF(0x2D2D2D);
    D2D1_COLOR_F menuBorder = ParseCssColor("#9b9b9b2e", D2D1::ColorF(0x3A3A3A, 0.18f));
    D2D1_COLOR_F menuText = D2D1::ColorF(0xE9E8E8);
    D2D1_COLOR_F menuHover = D2D1::ColorF(0x414141);
    // 候选窗卡片自身的边框色与圆角：翻译面板要与中文候选窗统一，就复用这套，而不是右键菜单配色。
    D2D1_COLOR_F cardFill = D2D1::ColorF(0x202020);
    D2D1_COLOR_F cardBorder = D2D1::ColorF(0x9B9B9B, 0x2E / 255.0f);
    float cardRadiusDip = 6.0f;
    // 候选卡片外缘到内容的内边距（DIP）。翻译面板复用它作为 popup 内边距，让面板里高亮与边框的
    // 间距和中文候选窗里高亮与卡片边框的间距一致。
    msimeui::Thickness cardContentPad{5.0f, 5.0f, 5.0f, 5.0f};
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
    // 场景里的点击都投递回候选窗的窗口过程，由宿主转给工作线程，不在鼠标分发途中改动状态。
    CandidateSkinScene::Callbacks callbacks;
    callbacks.itemActivated = [this](size_t index) { CommitItem(index); };
    callbacks.itemContextMenu = [this](size_t index, const POINT &clientPoint) {
        ShowItemContextMenu(index, clientPoint);
    };
    callbacks.pageArrow = [this](bool next) {
        if (hwnd_)
        {
            PostMessageW(hwnd_, WM_PAGE_CANDIDATE_ARROW, next ? CANDIDATE_PAGE_NEXT : CANDIDATE_PAGE_PREVIOUS, 1);
        }
    };
    callbacks.expandToggle = [this]() {
        if (hwnd_)
        {
            PostMessageW(hwnd_, WM_TOGGLE_CANDIDATE_EXPAND, 0, 0);
        }
    };
    callbacks.gridItemActivated = [this](size_t index) { CommitExpandedItem(index); };
    impl_->scene = CandidateSkinScene(std::move(callbacks));
    impl_->window = std::make_unique<msimeui::Window>(L"metasequoiaime_windows", L"cand", 1, 1);
    impl_->window->AdoptExistingHwnd(hwnd);
    impl_->window->SetStealFocusOnClick(false);
    bound_ = impl_->resources.EnsureForComposition(hwnd);
    // 先按当前皮肤搭好场景，第一次显示之前窗口就有内容可画。
    ApplySkin();
    CAND_DIAG_LOGF(L"candidate-d2d bind hwnd={:#x} composition={}", reinterpret_cast<uintptr_t>(hwnd), bound_ ? 1 : 0);
    return bound_;
}

void CandidatePresenter::RebuildScene()
{
    if (!impl_ || !impl_->window || !impl_->scene.Root())
    {
        return;
    }
    // 场景由 CandidateSkinScene 搭（设置页的皮肤预览用的是同一份），这里只留下候选窗要直接摆弄的控件。
    impl_->root = impl_->scene.Root();
    impl_->card = impl_->scene.CardVisual();
    impl_->preedit = impl_->scene.Preedit();
    impl_->list = impl_->scene.List();
    impl_->pager = impl_->scene.Pager();
    impl_->expandToggle = impl_->scene.ExpandToggle();
    impl_->expandGrid = impl_->scene.ExpandGrid();
    impl_->expandContent = impl_->scene.ExpandContent();
    impl_->expandScroller = impl_->scene.ExpandScroller();
    impl_->expandBody = impl_->scene.ExpandBody();
    auto scene = std::make_unique<msimeui::Scene>();
    scene->SetRoot(impl_->root);
    impl_->window->SetScene(std::move(scene));
}

void CandidatePresenter::ApplySkin()
{
    if (!impl_ || !impl_->window)
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
    // 卷轴展开网格的列数上限跟每页候选数走。
    fingerprint << '|' << GetConfiguredCandidatePageSize();
    // 同一个外部皮肤的 skin.toml 或图片改了，只有强制重载能看出来；带上重载代数让 D2D 也重读 manifest。
    fingerprint << '|' << GetCandidateSkinReloadRevision();
    // 亚克力皮肤在透明效果开关切换后要换底色和合成方式。
    fingerprint << '|' << (SystemTransparencyEnabled() ? 'T' : 't') << (backdropUnavailable_ ? 'U' : 'u');
    for (const auto &font : GetConfiguredCandidateFallbackFonts())
        fingerprint << '|' << font.size() << ':' << font;
    const std::string skinKey = fingerprint.str();
    if (skinKey == lastSkinFingerprint_ && impl_->card)
    {
        return;
    }
    lastSkinFingerprint_ = skinKey;
    // 换了皮肤：旧皮肤的动图不再续帧，新皮肤的动画从这一次显示重新起算。强制重载（重载代数变了）
    // 也走到这里，丢掉已解码的图片，皮肤目录里换过的图才会重新读取。
    StopAnimationFrames();
    animationClockRunning_ = false;
    impl_->resources.ClearImageCaches();

    CandidateSkinScene::Settings settings;
    settings.skinsRoot = std::filesystem::path(AssetRoot()) / L"skins";
    settings.skinId = skinId;
    settings.light = candLight;
    settings.horizontal = GetConfiguredCandidateWindowLayout() == "horizontal";
    settings.fontSize = static_cast<float>((std::max)(12, GetConfiguredCandidateFontSize()));
    settings.preeditFontSize = static_cast<float>((std::max)(12, GetConfiguredCandidateWindowPreeditFontSize()));
    settings.fontFamily = string_to_wstring(ResolveSystemFontFamilyForCss(GetConfiguredCandidateEnglishFont()));
    settings.resolveFontFamily = [](const std::string &family) {
        return string_to_wstring(ResolveSystemFontFamilyForCss(family));
    };
    for (const auto &font : GetConfiguredCandidateFallbackFontFamilies())
        settings.fallbackFontFamilies.push_back(string_to_wstring(font));
    settings.textColor = GetConfiguredCandidateTextColor();
    settings.preeditHidden = GetConfiguredCandidateWindowPreeditStyle() == "empty";
    settings.fitContent = GetConfiguredCandidateWindowFitContent();
    settings.pageSize = GetConfiguredCandidatePageSize();
    settings.framePadding = {kShadowPadLeft, kShadowPadTop, kShadowPadRight, kShadowPadBottom};
    settings.backdropAvailable = !backdropUnavailable_ && SystemTransparencyEnabled();
    const CandidateSkinScene::Result result = impl_->scene.Apply(settings);
    if (result.rebuilt)
    {
        // 旧场景的展开面板随场景一起丢掉，界面与工作线程都回到收起状态。
        SetExpanded(false);
        RebuildScene();
        lastSkinFingerprint_ = skinKey;
    }
    const CandSkinTokens &tokens = result.tokens;
    expandRows_ = result.expandRows;
    expandRowStepDip_ = result.expandRowStepDip;
    fitContent_ = result.fitContent;
    cardMinWidthDip_ = result.cardMinWidthDip;
    skinAnimates_ = result.skinAnimates;
    backdropActive_ = result.backdropActive;
    backdropRadiusDip_ = tokens.radius;
    if (backdropActive_)
    {
        impl_->resources.SetBackdropMaterial(result.backdropMaterial);
    }
    impl_->resources.SetBackdropComposition(backdropActive_);
    impl_->menuFill = tokens.menuFill;
    impl_->menuBorder = tokens.menuBorder;
    impl_->menuText = tokens.menuText;
    impl_->menuHover = tokens.menuHover;
    impl_->cardFill = tokens.surface;
    impl_->cardBorder = tokens.border;
    impl_->cardRadiusDip = tokens.radius;
    impl_->cardContentPad = result.cardContentPadding;

    msimeui::Theme theme = msimeui::ThemeManager::GetCurrent();
    theme.textInputFontFamily = string_to_wstring(GetConfiguredCandidateFont());
    if (theme.textInputFontFamily.empty())
    {
        theme.textInputFontFamily = L"Microsoft YaHei UI";
    }
    theme.uiFontFamily = theme.textInputFontFamily;
    theme.surface = tokens.surface;
    theme.border = tokens.border;
    theme.textPrimary = result.palette.text;
    theme.textSecondary = tokens.number;
    theme.primary = tokens.accent;
    theme.windowBackground = D2D1::ColorF(0, 0.0f);
    msimeui::ThemeManager::SetCurrent(theme);
}

std::uint64_t CandidatePresenter::FillItemsFromUi()
{
    const Global::CandidatePageSnapshotPtr page = Global::LoadCandidatePageSnapshot();
    impl_->renderedSnapshot = page;
    const bool translationsEnabled = GetConfiguredCandidateTranslationsEnabled();
    std::vector<msimeui::CandidateList::Item> items;
    items.reserve(page->page_views.size());
    std::vector<CandidateSkinScene::PageWord> words;
    words.reserve(page->page_views.size());
    for (size_t i = 0; i < page->page_views.size(); ++i)
    {
        const auto &view = page->page_views[i];
        words.push_back({string_to_wstring(view.text), string_to_wstring(view.badge)});
        msimeui::CandidateList::Item item;
        item.label = std::to_wstring(i + 1);
        item.text = words.back().text + words.back().badge;
        item.annotation = string_to_wstring(view.annotation);
        if (translationsEnabled)
        {
            // 逐段转成 UTF-16 再拼起来，区间按拼出来的串计。
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
    impl_->scene.SetDecorationCandidates(words, static_cast<size_t>((std::max)(0, page->selected_index_in_page)));
    if (impl_->pager)
    {
        impl_->pager->SetEnabled(page->has_previous_page, page->has_next_page);
    }
    if (impl_->expandBody)
    {
        std::vector<msimeui::CandidateList::Item> gridItems;
        if (expanded_)
        {
            // 换了一批后续候选（新的一页或别的输入）就回到顶部；只是往后追加了候选则保持滚动位置。
            const bool sameContent = page->expanded_start == expandedStart_ && !expandedWords_.empty() &&
                                     !page->expanded_words.empty() &&
                                     page->expanded_words.front() == expandedWords_.front();
            if (!sameContent)
            {
                impl_->expandScroller->SetScrollOffset(0.0f);
                // 新的一批还没请求过「再展开」。
                expandRequestedCount_ = static_cast<size_t>(-1);
            }
            expandedStart_ = page->expanded_start;
            expandedWords_ = page->expanded_words;
            gridItems.reserve(expandedWords_.size());
            for (const auto &word : expandedWords_)
            {
                msimeui::CandidateList::Item item;
                item.text = word;
                gridItems.push_back(std::move(item));
            }
        }
        const bool showGrid = !gridItems.empty();
        impl_->expandGrid->SetItems(std::move(gridItems));
        impl_->expandContent->SetShowGrid(showGrid);
        impl_->expandToggle->SetExpanded(expanded_);
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

void CandidatePresenter::CommitExpandedItem(size_t gridIndex)
{
    if (!hwnd_ || gridIndex >= expandedWords_.size())
    {
        return;
    }
    // 文字随消息交给宿主窗口过程，由它释放。
    auto *text = new (std::nothrow) std::wstring(expandedWords_[gridIndex]);
    if (text && !PostMessageW(hwnd_, WM_COMMIT_CANDIDATE_AT,
                              static_cast<WPARAM>(static_cast<size_t>(expandedStart_) + gridIndex),
                              reinterpret_cast<LPARAM>(text)))
    {
        delete text;
    }
}

void CandidatePresenter::SetExpanded(bool expanded)
{
    expanded_ = expanded;
    Global::candidate_expand_panel_open.store(expanded, std::memory_order_release);
    if (!expanded)
    {
        expandedWords_.clear();
        expandedStart_ = 0;
        expandRequestedCount_ = 0;
    }
    if (!impl_ || !impl_->expandBody)
    {
        return;
    }
    impl_->expandToggle->SetExpanded(expanded);
    if (!expanded)
    {
        impl_->expandGrid->SetItems({});
        impl_->expandContent->SetShowGrid(false);
        impl_->expandScroller->SetScrollOffset(0.0f);
    }
}

void CandidatePresenter::ToggleExpanded()
{
    if (!impl_ || !impl_->expandBody || !::is_global_wnd_cand_shown)
    {
        return;
    }
    if (expanded_)
    {
        SetExpanded(false);
    }
    else
    {
        SetExpanded(true);
        // 后续候选由工作线程展开一批后随候选页一起发布过来，再刷新一次界面。
        expandRequestedCount_ = static_cast<size_t>(-1);
        PostMessageW(hwnd_, WM_EXPAND_CANDIDATES, 0, 0);
    }
    ShowFromGlobalState();
}

void CandidatePresenter::RequestMoreExpandedIfNearEnd()
{
    if (!expanded_ || !impl_ || !impl_->expandScroller || !hwnd_)
    {
        return;
    }
    // 滚到最后一行附近、且这一批候选还没请求过时，再要一批；工作线程展开不出新的就不会变，也就不再请求。
    const float remaining = impl_->expandScroller->GetMaxScrollOffset() - impl_->expandScroller->GetScrollOffset();
    if (remaining > expandRowStepDip_ * 1.5f || expandedWords_.size() == expandRequestedCount_)
    {
        return;
    }
    expandRequestedCount_ = expandedWords_.size();
    PostMessageW(hwnd_, WM_EXPAND_CANDIDATES, 0, 0);
}

void CandidatePresenter::ScrollExpanded(short delta)
{
    if (!expanded_ || !impl_ || !impl_->expandScroller)
    {
        return;
    }
    // 首行与网格一起滚，一格滚一行；高精度滚轮按比例滚。滚到顶或底就停住，展开与收起只认按钮。
    const float offset = impl_->expandScroller->GetScrollOffset();
    impl_->expandScroller->SetScrollOffset(offset - static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA) *
                                                        expandRowStepDip_);
    RequestMoreExpandedIfNearEnd();
    Present();
    ValidateRect(hwnd_, nullptr);
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
    // 右键菜单与竖排译文面板不共存：菜单要挂在被点的候选上，面板也挂在高亮候选上，两个叠着看不清。
    CloseTranslationPanel();
    impl_->contextMenuPageIndex = pageIndex;

    // A mouse callback has no posted-message handoff at all, so it must not walk the worker's live page either.
    const Global::CandidatePageSnapshotPtr page = Global::LoadCandidatePageSnapshot();
    // 冻结的是用户右键时看见的那一页：已发布但还没画出来的一页不算。
    FanyNamedPipe::NoteCandidateUiInteraction(
        Global::rendered_candidate_page_generation.load(std::memory_order_acquire));
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

// 把当前挂着的候选翻译竖排面板从场景里摘掉。面板的关闭回调只表示「用户点了面板外面」，
// 要通知工作线程关状态；这里逐帧重建或整窗收起，用 notify=false 的摘除，不发那个通知。
void CandidatePresenter::CloseTranslationPanel()
{
    if (!impl_ || !impl_->window)
    {
        return;
    }
    if (impl_->translationPanel)
    {
        // 窗口缓存的 hover/focus 是指向面板里控件的裸指针，面板一销毁它们就悬空，先清掉。
        impl_->window->DispatchImportedMessage(WM_MOUSELEAVE, 0, 0);
        impl_->window->FocusVisual(nullptr);
        if (msimeui::Scene *scene = impl_->window->GetScene())
        {
            scene->RemovePopup(impl_->translationPanel.get(), false);
        }
    }
    impl_->translationPanel.reset();
    impl_->translationPanelList.reset();
}

// 按刚画出来的那一页（renderedSnapshot）建候选展开面板：内容是高亮候选（anchor）按启用类别平铺
// 后的全部条目（当前只有翻译），挂在它正下方。返回 true 时面板已进场景，need* 是宿主窗口要撑到的
// 布局尺寸（DIP），PlaceAndShow 之前把 widthDip/heightDip 抬到它，面板和候选就在同一帧里画出来。
bool CandidatePresenter::PrepareExpansionPanel(float &needWidthDip, float &needHeightDip, POINT caret, float scale)
{
    needWidthDip = 0.0f;
    needHeightDip = 0.0f;
    if (!impl_ || !impl_->window || !impl_->list || !impl_->renderedSnapshot)
    {
        return false;
    }
    const Global::CandidatePageSnapshot &page = *impl_->renderedSnapshot;
    if (!page.expansion_panel_active || page.expansion_panel_items.empty())
    {
        return false;
    }
    const int anchorIndex = page.expansion_panel_anchor;
    if (anchorIndex < 0 || impl_->list->GetItem(static_cast<size_t>(anchorIndex)) == nullptr)
    {
        return false;
    }
    if (!impl_->card)
    {
        return false;
    }
    // 面板与中文候选窗统一：宽度对齐候选卡片、左对齐、复用卡片边框色与圆角，两框之间留一段间距。
    // 锚点取整张候选卡片，而不是高亮那一项。
    const msimeui::RectF anchor = impl_->card->GetBounds();
    if (anchor.width < 1.0f || anchor.height < 1.0f)
    {
        return false;
    }
    constexpr float kPanelGapDip = 6.0f;
    constexpr int kMaxPanelColumns = 6;
    constexpr float kPanelColumnGapDip = 8.0f;

    // 位置选项：right = 面板放在候选窗右侧（左缘贴候选窗右缘留间距，顶边跟随高亮候选上下移动）；
    // 其余取值一律按当前方案摆在选定候选词正下方。
    const std::string placementValue = GetConfiguredCandidateExpansionPanelPlacement();
    const bool placeRight = placementValue == "right";

    // 摆在下方时锚在卡片下沿，可用空间是「卡片下沿到显示器下沿」与「卡片左边到显示器右边」。卡片顶边
    // 在屏幕上约落在插入符处（候选窗贴着插入符），据此估算；估算偏保守只会更早转多列，不会让面板
    // 越出屏幕（宿主本身也会被夹到显示器边界）。
    const MonitorCoordinates monitor = GetMonitorCoordinatesFromPoint(caret);
    const float cardBottomScreenY = static_cast<float>(caret.y) + anchor.height * scale;
    const float belowAvailableHeightDip =
        (static_cast<float>(monitor.bottom) - cardBottomScreenY) / scale - kPanelGapDip - 8.0f;
    const float belowAvailableWidthDip =
        (static_cast<float>(monitor.right) - static_cast<float>(caret.x)) / scale - 8.0f;
    // 摆在右方时可用宽度是「卡片右沿到显示器右沿」减去间距，可用高度是整个候选窗以下。
    // 右侧空间放不下一个像样的面板时不硬挤，直接按下方摆。
    const float rightAvailableWidthDip =
        (static_cast<float>(monitor.right) - (static_cast<float>(caret.x) + anchor.width * scale)) / scale -
        kPanelGapDip - 8.0f;
    const float rightAvailableHeightDip =
        (static_cast<float>(monitor.bottom) - static_cast<float>(caret.y)) / scale - 8.0f;
    bool useRight = placeRight && rightAvailableWidthDip >= 120.0f;
    float availableHeightDip = useRight ? rightAvailableHeightDip : belowAvailableHeightDip;
    float availableWidthDip = useRight ? rightAvailableWidthDip : belowAvailableWidthDip;

    // 字体、字号、配色、选中样式、数字标全部沿用主候选列表的外观（皮肤配色），这样两框看起来是一套。
    // 面板没有辅助码、没有行内译文；每行前带与中文候选一致的 1..N 数字标（按数字直接上屏对应译义）；
    // 强制竖排（关掉横排的网格/铺满/最小列宽）。面板按内容自适应宽度（fitContent 让列表按最长内容测宽），
    // 并与当前高亮候选所在列对齐。
    auto list = std::make_shared<msimeui::CandidateList>();
    msimeui::CandidateList::Appearance appearance = impl_->list->GetAppearance();
    appearance.fitContent = true;
    appearance.gridColumns = 0;
    appearance.justifyHorizontalRows = false;
    appearance.minItemWidth = 0.0f;
    // 面板里的译义走与主列表行内译文完全相同的着色通道（主题色，随皮肤切换），只把字号从行内的
    // 0.78 比例稍微放大；行高仍由 itemHeight 决定，不随字号变。
    appearance.translationFontSizeScale = 0.9f;
    list->SetAppearance(std::move(appearance));
    std::vector<msimeui::CandidateList::Item> items;
    items.reserve(page.expansion_panel_items.size());
    // 数字标与中文候选窗一致：每行前标 1..N，复用主列表的 label 外观（labelColor/字号/间距已随 appearance 复制）。
    // 数字后面再带一段类别淡色前缀（item.tag，由提供方从类别元数据填），每个类别自带自己的标签。
    size_t row_index = 0;
    for (const auto &entry : page.expansion_panel_items)
    {
        msimeui::CandidateList::Item item;
        item.label = std::to_wstring(row_index + 1);
        item.tag = entry.tag;
        if (entry.category_id == string_to_wstring(std::string(FanyImeIpc::kCategoryTranslation)))
        {
            // 翻译条目：与主列表 FillItemsFromUi 同一条路径——正文进 translation 通道（译文色），
            // 词性与分隔符按 Secondary/Delimiter run 走各自的皮肤配色；没配时继承译文色。
            for (const auto &segment : FanyImeIpc::SegmentTranslationGloss(wstring_to_string(entry.display)))
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
        else
        {
            item.text = entry.display;
        }
        items.push_back(std::move(item));
        ++row_index;
    }
    list->SetItems(std::move(items));
    list->SetSelectedIndex(static_cast<size_t>((std::max)(0, page.expansion_panel_selected)));
    // 面板里点选一条译义：与数字键 1..N 同一条上屏路径，下标按面板里的顺序交给工作线程。
    list->SetOnItemActivated([this](size_t index) {
        if (!hwnd_)
        {
            return;
        }
        PostMessageW(hwnd_, WM_COMMIT_EXPANSION_PANEL_ITEM, static_cast<WPARAM>(index), 0);
    });

    const msimeui::Thickness pad = impl_->cardContentPad;
    const float innerWidth = (std::max)(1.0f, anchor.width - pad.left - pad.right);
    // 屏幕右缘换算到窗口坐标：窗口把卡片左边 card.x 落在插入符 caret.x 上，所以窗口 x 与屏幕 x
    // 的换算是 screenX = caret.x + (windowX - anchor.x) * scale，反解出屏幕右缘对应的窗口 x。
    const float screenRightWindowX =
        anchor.x + (static_cast<float>(monitor.right) - static_cast<float>(caret.x)) / scale;

    // 先按单列测一次：把高度上界设成可用高度，单列放不下时返回值会被夹到可用高度，从而可靠地判定
    // 「单列会越过下边缘」。按可用宽度测（fitContent 让列表只占最长内容那么宽）。
    float singleBudgetWidth = useRight ? availableWidthDip : (std::max)(availableWidthDip, innerWidth);
    msimeui::SizeF single = list->MeasureInLayout({singleBudgetWidth, (std::max)(availableHeightDip, 1.0f)});
    // 最长一条本身就比右侧可用宽度还宽时，面板会顶出屏幕边缘，改按下方摆。
    if (useRight && single.width > availableWidthDip + 1.0f)
    {
        useRight = false;
        availableHeightDip = belowAvailableHeightDip;
        availableWidthDip = belowAvailableWidthDip;
        singleBudgetWidth = (std::max)(availableWidthDip, innerWidth);
        single = list->MeasureInLayout({singleBudgetWidth, (std::max)(availableHeightDip, 1.0f)});
    }
    const float singlePanelHeight = single.height + pad.top + pad.bottom;
    const bool multiColumn = singlePanelHeight > availableHeightDip;

    float panelWidth = anchor.width;
    float panelHeight = singlePanelHeight;
    if (multiColumn)
    {
        // 多列：把列高/列宽预算写进外观，弹窗之后以宿主视口重新测量也会得到同一布局。列高预算取
        // 卡片下沿到显示器下沿减去间距，列宽预算取卡片左边到显示器右边；列数受 kMaxPanelColumns 与
        // 列宽预算双重限制，放不下时末列在底部裁切。列宽预算用可用宽度（不是卡片内宽），这样测出来的是
        // 按高度排布的自然列总宽，面板宽度就是多列自然宽度，不跟卡片对齐。
        const float colHeight = (std::max)(availableHeightDip - kPanelGapDip, 1.0f);
        const float colBudgetWidth = (std::max)(availableWidthDip, 1.0f);
        msimeui::CandidateList::Appearance colAppearance = list->GetAppearance();
        colAppearance.verticalColumns = kMaxPanelColumns;
        colAppearance.columnGap = kPanelColumnGapDip;
        colAppearance.columnBudgetHeight = colHeight;
        colAppearance.columnBudgetWidth = colBudgetWidth;
        list->SetAppearance(std::move(colAppearance));
        const msimeui::SizeF multi = list->MeasureInLayout({colBudgetWidth, colHeight});
        panelWidth = multi.width + pad.left + pad.right;
        panelHeight = multi.height + pad.top + pad.bottom;
    }
    else
    {
        // 自适应单列：按最长内容测宽。
        panelWidth = single.width + pad.left + pad.right;
    }

    // 面板左缘（窗口坐标）：摆在下方时与当前高亮候选同一列——让面板内容左缘对齐高亮项左缘
    // （item.x = 卡片内列表里该项的左缘），即 panelLeft = item.x - pad.left；竖排单列时它等于卡片左边。
    // 右侧将要溢出屏幕时丢失对齐，把面板右缘贴到屏幕右缘。
    // 摆在右方时左缘固定在候选窗右缘加间距，顶边与高亮项顶边对齐、随高亮切换上下移动；
    // 底边越过显示器下沿时向上回退，但不越过候选窗顶边。
    float panelLeft = anchor.x;
    float panelTop = anchor.y;
    {
        const msimeui::RectF item = impl_->list->GetItemBounds(static_cast<size_t>(anchorIndex));
        if (useRight)
        {
            panelLeft = anchor.x + anchor.width + kPanelGapDip;
            if (item.height > 0.0f)
                panelTop = item.y;
            const float screenBottomWindowY =
                anchor.y + (static_cast<float>(monitor.bottom) - static_cast<float>(caret.y)) / scale;
            if (panelTop + panelHeight > screenBottomWindowY)
                panelTop = (std::max)(anchor.y, screenBottomWindowY - panelHeight);
        }
        else
        {
            if (item.width > 0.0f)
                panelLeft = item.x - pad.left;
            if (panelLeft + panelWidth > screenRightWindowX)
                panelLeft = (std::max)(0.0f, screenRightWindowX - panelWidth);
        }
    }

    auto popup = std::make_shared<msimeui::Popup>(list);
    // 面板宽度按测得的内容宽度排布，不跟随候选卡片。
    // 边框与圆角复用卡片那套。内边距复用候选卡片的外缘内边距，让面板里高亮与边框的间距和中文候选窗一致。
    popup->SetMatchAnchorWidth(false);
    popup->SetPadding(pad);
    popup->SetBackgroundFill(impl_->cardFill);
    popup->SetBorderColor(impl_->cardBorder);
    popup->SetCornerRadius(impl_->cardRadiusDip);
    popup->SetShadowEnabled(true);
    popup->SetConstrainToViewport(true);
    if (useRight)
    {
        // 锚矩形取整张卡片（x/width 用卡片），顶边换成 panelTop：面板贴在候选窗右缘、与高亮候选对齐。
        popup->SetPlacement(msimeui::PopupPlacement::RightLeading);
        popup->SetOffset(kPanelGapDip, 0.0f);
        popup->SetAnchorRect({anchor.x, panelTop, anchor.width, anchor.height});
        needWidthDip = (std::max)(anchor.x + anchor.width, panelLeft + panelWidth) + 8.0f;
        needHeightDip = (std::max)(anchor.y + anchor.height, panelTop + panelHeight) + 8.0f;
    }
    else
    {
        popup->SetPlacement(msimeui::PopupPlacement::BelowLeading);
        popup->SetOffset(0.0f, kPanelGapDip);
        // 锚点仍取卡片下沿（y/height 用卡片），只把左缘换成 panelLeft，保证面板贴在候选窗下方。
        popup->SetAnchorRect({panelLeft, anchor.y, anchor.width, anchor.height});
        needWidthDip = (std::max)(anchor.x + anchor.width, panelLeft + panelWidth) + 8.0f;
        needHeightDip = anchor.y + anchor.height + kPanelGapDip + panelHeight + 8.0f;
    }

    impl_->translationPanelList = std::move(list);
    impl_->translationPanel = std::move(popup);
    if (msimeui::Scene *scene = impl_->window->GetScene())
    {
        // 唯一的关闭通知：用户点了面板外面。请工作线程把面板状态关掉，下一帧就不带面板了。
        scene->AddPopup(impl_->translationPanel, [this]() {
            if (hwnd_)
            {
                PostMessageW(hwnd_, WM_CLOSE_TRANSLATION_PANEL, 0, 0);
            }
        });
    }
    return true;
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
    if (cardOriginPinned_)
    {
        x = pinnedCardOrigin_.x - cardLeftPx;
        y = pinnedCardOrigin_.y - cardTopPx;
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
        CloseTranslationPanel();
        StopAnimationFrames();
        SetCandidateHostCloaked(true);
        ::is_global_wnd_cand_shown = true;
        Global::SetCandidateWindowRenderedVisible(true);
        CAND_DIAG_LOGF(L"candidate-d2d show deferred: no usable caret anchor ({},{})", caret.x, caret.y);
        return;
    }
    CloseContextMenu(false);
    CloseTranslationPanel();
    ApplySkin();
    // 减少动画的系统开关可能随时改，每次显示都重读；v1 皮肤从不播放。动画从候选窗出现时起算，
    // 同一次显示里的按键不会让它重头开始。
    impl_->resources.SetAnimationsEnabled(skinAnimates_ && SystemAnimationsEnabled());
    if (!animationClockRunning_)
    {
        impl_->resources.SetAnimationEpoch(msimeui::DeviceResources::AnimationClock::now());
        animationClockRunning_ = true;
    }
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
    if (impl_->expandBody)
    {
        impl_->expandGrid->SetHoverEnabled(hoverArmed_);
        impl_->expandToggle->SetHoverEnabled(hoverArmed_);
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
        // 展开与收起是两种尺寸，不能互相撑住。
        if (expanded_)
            stickyKey += L"#expanded";
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
    // 候选翻译竖排面板：列表刚排完、锚点已定，先把面板建好挂进场景，再把宿主撑到装得下它，
    // PlaceAndShow 里的那一次 Present 就连面板一起画出来。卡片本身的位置和大小不受影响。
    float panelNeedWidthDip = 0.0f;
    float panelNeedHeightDip = 0.0f;
    if (PrepareExpansionPanel(panelNeedWidthDip, panelNeedHeightDip, caret, scale.scale))
    {
        widthDip = (std::max)(widthDip, panelNeedWidthDip);
        heightDip = (std::max)(heightDip, panelNeedHeightDip);
    }
    PlaceAndShow(caret, widthDip, heightDip, cardLeftDip, cardTopDip, scale);
    ::is_global_wnd_cand_shown = true;
    Global::SetCandidateWindowRenderedVisible(true);
    // 后续候选不够填满一屏时，不用等滚动就再要一批。
    RequestMoreExpandedIfNearEnd();
    // PlaceAndShow presented while the host was still being placed; it is placed and shown only now.
    ScheduleAnimationFrame();
    // PlaceAndShow paints synchronously, so the page is on screen now. Echo the generation of the
    // snapshot that was actually rendered (not the latest published one) so a pending digit/space
    // selection can settle against the same page the user is looking at.
    Global::PublishRenderedCandidatePageGeneration(renderedGeneration);
}

void CandidatePresenter::SetPinnedCardOrigin(const POINT *origin)
{
    cardOriginPinned_ = origin != nullptr;
    pinnedCardOrigin_ = origin ? *origin : POINT{};
}

void CandidatePresenter::Hide()
{
    cardOriginPinned_ = false;
    if (!hwnd_)
    {
        return;
    }
    CloseContextMenu(false);
    CloseTranslationPanel();
    StopAnimationFrames();
    animationClockRunning_ = false;
    ::is_global_wnd_cand_shown = false;
    Global::SetCandidateWindowRenderedVisible(false);
    hoverArmed_ = false;
    wheelDeltaAccumulator_ = 0;
    SetExpanded(false);
    if (impl_ && impl_->list)
    {
        impl_->list->SetHoverEnabled(false);
    }
    if (impl_ && impl_->pager)
    {
        impl_->pager->SetHoverEnabled(false);
    }
    if (impl_ && impl_->expandBody)
    {
        impl_->expandGrid->SetHoverEnabled(false);
        impl_->expandToggle->SetHoverEnabled(false);
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
        if (!impl_->resources.UsesBackdropComposition())
        {
            return;
        }
        // 建不起 Windows.UI.Composition：本次运行退回交换链，并让下一次 ApplySkin 换回不透明底色。
        CAND_DIAG_LOGF(L"candidate-d2d backdrop composition unavailable, falling back to swap chain");
        backdropUnavailable_ = true;
        backdropActive_ = false;
        lastSkinFingerprint_.clear();
        impl_->resources.SetBackdropComposition(false);
        if (!impl_->resources.EnsureForComposition(hwnd_))
        {
            return;
        }
    }
    impl_->resources.Resize(width, height);
    // Samples the animation clock for this frame and starts the next-frame deadline over.
    impl_->resources.BeginFrame();
    if (!impl_->resources.BeginDraw())
    {
        return;
    }
    ID2D1RenderTarget *target = impl_->resources.GetRenderTarget();
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
    if (backdropActive_ && impl_->card)
    {
        // 和这一帧的内容同一次提交，卡片怎么伸缩毛玻璃都贴着它。
        impl_->resources.SetBackdrop(impl_->card->GetBounds(), backdropRadiusDip_);
    }
    const HRESULT hr = impl_->resources.EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        impl_->resources.DiscardTarget();
        return;
    }
    impl_->resources.Present();
    ScheduleAnimationFrame();
}

void CandidatePresenter::ScheduleAnimationFrame()
{
    if (!hwnd_ || !impl_ || !impl_->window)
    {
        return;
    }
    const msimeui::Scene *scene = impl_->window->GetScene();
    const auto deadline = scene ? scene->GetNextFrameDeadline() : std::nullopt;
    // While placing, PlaceAndShow presents and schedules once the host is visible; a hidden or
    // parked host has nothing on screen to animate.
    if (!deadline || placingHost_ || !::is_global_wnd_cand_shown || !IsWindowVisible(hwnd_))
    {
        StopAnimationFrames();
        return;
    }
    const auto wait =
        std::chrono::ceil<std::chrono::milliseconds>(*deadline - msimeui::DeviceResources::AnimationClock::now());
    const UINT delay = static_cast<UINT>((std::clamp)(wait.count(), static_cast<long long>(USER_TIMER_MINIMUM),
                                                      static_cast<long long>(USER_TIMER_MAXIMUM)));
    // One shot per frame, replacing any pending one. A little slack lets Windows batch the wake-up
    // with others without visibly changing the frame timing.
    SetCoalescableTimer(hwnd_, kCandidateAnimationTimerId, delay, nullptr, (std::min)(delay / 10U, 8U));
}

void CandidatePresenter::StopAnimationFrames()
{
    if (hwnd_)
    {
        KillTimer(hwnd_, kCandidateAnimationTimerId);
    }
}

void CandidatePresenter::PresentEmptyFrame()
{
    if (!bound_ || !impl_)
    {
        return;
    }
    if (!impl_->resources.BeginDraw())
    {
        return;
    }
    impl_->resources.GetRenderTarget()->Clear(D2D1::ColorF(0, 0.0f));
    impl_->resources.SetBackdrop({}, 0.0f);
    if (impl_->resources.EndDraw() == D2DERR_RECREATE_TARGET)
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
    if (impl_ && impl_->expandBody)
    {
        impl_->expandGrid->SetHoverEnabled(true);
        impl_->expandToggle->SetHoverEnabled(true);
    }
}

void CandidatePresenter::NoteHoverOverItem(POINT clientPoint)
{
    // 指针只是从候选窗上划过不算选词，要真的停到某个候选上才冻结这一页的顺序。
    const std::uint64_t generation = Global::rendered_candidate_page_generation.load(std::memory_order_acquire);
    if (generation == hoverNotedGeneration_ || !impl_ || !impl_->list || !impl_->window)
    {
        return;
    }
    const msimeui::PointF point = impl_->window->ClientPixelsToDips(clientPoint);
    for (size_t index = 0; impl_->list->GetItem(index) != nullptr; ++index)
    {
        const msimeui::RectF bounds = impl_->list->GetItemBounds(index);
        if (point.x >= bounds.x && point.x < bounds.x + bounds.width && point.y >= bounds.y &&
            point.y < bounds.y + bounds.height)
        {
            FanyNamedPipe::NoteCandidateUiInteraction(generation);
            hoverNotedGeneration_ = generation;
            return;
        }
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
    case WM_TIMER:
        if (wParam != kCandidateAnimationTimerId)
        {
            return false;
        }
        // One-shot: the Present below schedules the next frame if anything still animates.
        StopAnimationFrames();
        if (::is_global_wnd_cand_shown && !placingHost_)
        {
            Present();
        }
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
        if (hoverArmed_)
        {
            NoteHoverOverItem(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
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
    case WM_TOGGLE_CANDIDATE_EXPAND:
        ToggleExpanded();
        return true;
    case WM_MOUSEWHEEL: {
        if (impl_->expandBody && expanded_)
        {
            // 卷轴展开皮肤展开时：滚轮上下滚动面板，不翻页，也不受「滚轮翻页」开关约束。
            // 收起时和其他皮肤一样按下面的规则翻页。
            wheelDeltaAccumulator_ = 0;
            if (!::is_global_wnd_cand_shown)
            {
                return true;
            }
            if (impl_->contextMenuOpen)
            {
                CloseContextMenu(true);
                return true;
            }
            ScrollExpanded(GET_WHEEL_DELTA_WPARAM(wParam));
            return true;
        }
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
