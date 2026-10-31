#pragma once

#include "skin/candidate_skin_catalog.h"
#include "window/candidate_skin_palette.h"
#include "window/candidate_skin_tokens.h"

#include "msimeui/Controls.h"
#include "msimeui/DeviceResources.h"
#include "msimeui/Layout.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CandidateSkinOverlay;

// 卷轴展开皮肤在候选行右端的展开按钮：收起时画“∨”，展开时画“∧”。按钮贴在槽位的右上角，
// 竖直位置由外边距对齐到首行中间。
class CandidateExpandToggle : public msimeui::Visual
{
  public:
    struct Appearance
    {
        float buttonWidth = 14.0f;
        float buttonHeight = 14.0f;
        // 折线的宽度；高度是宽度的一半。
        float glyphWidth = 8.0f;
        float strokeWidth = 1.3f;
        float cornerRadius = 3.0f;
        D2D1_COLOR_F glyphColor = D2D1::ColorF(0xE9E8E8, 0.8f);
        D2D1_COLOR_F hoverFill = D2D1::ColorF(0x414141);
        D2D1_COLOR_F pressedFill = D2D1::ColorF(0x353535);
    };

    void SetAppearance(const Appearance &appearance);
    void SetExpanded(bool expanded);
    void SetOnClick(std::function<void()> handler);
    void SetHoverEnabled(bool enabled);

    msimeui::SizeF Measure(const msimeui::SizeF &availableSize) override;
    void Arrange(const msimeui::RectF &finalRect) override;
    void Render(msimeui::DeviceResources &deviceResources) override;
    bool HitTest(const msimeui::PointF &point) const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    void OnMouseLeave() override;
    HCURSOR GetCursor() const override;

  private:
    msimeui::RectF Box() const;

    Appearance appearance_{};
    bool expanded_ = false;
    bool hoverEnabled_ = true;
    bool hovered_ = false;
    bool pressed_ = false;
    std::function<void()> onClick_;
};

// 卷轴展开皮肤滚动区里的内容：首行（当前页）在上，展开时下面接后续候选的网格，两者一起滚动。
// 收起或网格为空时只有首行。
class CandidateExpandContent : public msimeui::Panel
{
  public:
    CandidateExpandContent(std::shared_ptr<msimeui::CandidateList> list, std::shared_ptr<msimeui::CandidateList> grid);

    void SetShowGrid(bool show);
    bool ShowsGrid() const;
    void SetGap(float gap);
    float GetGap() const;
    // 最近一次测量出的首行高度。
    float GetListExtent() const;

    msimeui::SizeF Measure(const msimeui::SizeF &availableSize) override;
    void Arrange(const msimeui::RectF &finalRect) override;
    void Render(msimeui::DeviceResources &deviceResources) override;

  private:
    msimeui::CandidateList *list_ = nullptr;
    msimeui::CandidateList *grid_ = nullptr;
    bool showGrid_ = false;
    float gap_ = 2.0f;
    float listExtent_ = 0.0f;
    float gridExtent_ = 0.0f;
};

// 卷轴展开皮肤的候选区：左边是滚动区（首行加网格），右边一窄列放展开按钮，按钮不随内容滚动。
// 滚动区宽度跟首行走，展开时高度按「首行 + visibleRows 行网格」封顶，内容不足时收紧；
// 出现滚动条时再加上滚动条占的宽度，首行不会因此被挤到换行。
class CandidateExpandBody : public msimeui::Panel
{
  public:
    CandidateExpandBody(std::shared_ptr<msimeui::ScrollViewer> scroller,
                        std::shared_ptr<CandidateExpandContent> content, std::shared_ptr<msimeui::Visual> toggle);

    void SetGeometry(int visibleRows, float rowHeight, float rowGap, float scrollbarReserve);

    msimeui::SizeF Measure(const msimeui::SizeF &availableSize) override;
    void Arrange(const msimeui::RectF &finalRect) override;
    void Render(msimeui::DeviceResources &deviceResources) override;

  private:
    msimeui::ScrollViewer *scroller_ = nullptr;
    CandidateExpandContent *content_ = nullptr;
    msimeui::Visual *toggle_ = nullptr;
    int visibleRows_ = CandidateSkinCatalog::kDefaultExpandRows;
    float rowHeight_ = 28.0f;
    float rowGap_ = 2.0f;
    float scrollbarReserve_ = 10.0f;
    float toggleWidth_ = 0.0f;
    float viewport_ = 0.0f;
};

// 候选框的整棵 msimeui 场景与套皮肤的逻辑：外框（留给阴影与挂件的空白）→ 装饰图 + 卡片（底色、描边、阴影、
// 背景图或九宫格面板图，下层 / 上层挂件）→ 预编辑行与候选列表（翻页箭头或卷轴展开区）。
// 候选窗（CandidatePresenter）与设置页的皮肤预览共用它，所以两边画出来的一模一样；
// 这里不碰窗口、配置与全局主题，所需的一切都由 Settings 传进来。
class CandidateSkinScene
{
  public:
    // 套一次皮肤需要的全部输入。
    struct Settings
    {
        std::filesystem::path skinsRoot;
        std::string skinId;
        bool light = false;
        bool horizontal = false;
        float fontSize = 16.0f;
        float preeditFontSize = 16.0f;
        // 候选字体与回退字体（皮肤的 font_family 会排到最前）。
        std::wstring fontFamily;
        std::vector<std::wstring> fallbackFontFamilies;
        // 皮肤的 font_family 换算成可用字体名，由调用方决定怎么解析；为空时原样使用。
        std::function<std::wstring(const std::string &)> resolveFontFamily;
        // 设置里的候选文字色（auto 不覆盖）。
        std::string textColor = "auto";
        // 预编辑行隐藏（preedit_style = empty）。
        bool preeditHidden = false;
        // 设置里强制收紧宽度；皮肤自己也可以开。
        bool fitContent = false;
        int pageSize = 6;
        // 外框四周的留白：候选窗留给阴影，设置页预览不留。
        msimeui::Thickness framePadding{32.0f, 20.0f, 32.0f, 40.0f};
        // 能用系统材质（亚克力 / 云母）时为真；为假时要材质的皮肤退回不透明底色。
        bool backdropAvailable = false;
    };

    // 用户在场景里的操作，由调用方接到候选窗的消息或忽略（设置页预览）。
    struct Callbacks
    {
        std::function<void(size_t)> itemActivated;
        std::function<void(size_t, const POINT &)> itemContextMenu;
        std::function<void(bool next)> pageArrow;
        std::function<void()> expandToggle;
        std::function<void(size_t)> gridItemActivated;
    };

    // 套完皮肤后调用方要知道的结果。
    struct Result
    {
        // 场景结构变了（排列方式、翻页箭头、卷轴展开），控件都是新建的。
        bool rebuilt = false;
        CandSkinTokens tokens;
        CandidateSkinPalette palette{};
        bool skinAnimates = false;
        bool wantsBackdrop = false;
        bool backdropActive = false;
        msimeui::BackdropMaterial backdropMaterial;
        float cardMinWidthDip = 160.0f;
        bool fitContent = false;
        int expandRows = CandidateSkinCatalog::kDefaultExpandRows;
        float expandRowStepDip = 30.0f;
        // 卡片外缘到内容的内边距（与 card_->SetPadding 同值），供翻译面板复用，保证两框高亮间距一致。
        msimeui::Thickness cardContentPadding{5.0f, 5.0f, 5.0f, 5.0f};
    };

    // 当前页的一个候选：词本身与后面的标注（如整句模型名）。
    struct PageWord
    {
        std::wstring text;
        std::wstring badge;
    };

    explicit CandidateSkinScene(Callbacks callbacks = {});

    Result Apply(const Settings &settings);
    // 皮肤开了 candidate_overlay 时，按它指定的第几个候选从当前页取词画到装饰图上，太宽就缩字号；
    // 其余皮肤什么也不画。selectedIndex 是当前页高亮的候选。
    void SetDecorationCandidates(const std::vector<PageWord> &page, size_t selectedIndex);

    // 场景的根（外框所在的竖排面板）；第一次 Apply 之前为空。
    const std::shared_ptr<msimeui::StackPanel> &Root() const
    {
        return root_;
    }
    const std::shared_ptr<msimeui::Card> &CardVisual() const
    {
        return card_;
    }
    const std::shared_ptr<msimeui::TextBlock> &Preedit() const
    {
        return preedit_;
    }
    const std::shared_ptr<msimeui::CandidateList> &List() const
    {
        return list_;
    }
    const std::shared_ptr<msimeui::PagerArrows> &Pager() const
    {
        return pager_;
    }
    const std::shared_ptr<CandidateExpandToggle> &ExpandToggle() const
    {
        return expandToggle_;
    }
    const std::shared_ptr<msimeui::CandidateList> &ExpandGrid() const
    {
        return expandGrid_;
    }
    const std::shared_ptr<CandidateExpandContent> &ExpandContent() const
    {
        return expandContent_;
    }
    const std::shared_ptr<msimeui::ScrollViewer> &ExpandScroller() const
    {
        return expandScroller_;
    }
    const std::shared_ptr<CandidateExpandBody> &ExpandBody() const
    {
        return expandBody_;
    }

  private:
    void Build(bool horizontal, bool pageArrows, bool expandPanel);

    Callbacks callbacks_;
    bool built_ = false;
    bool horizontal_ = false;
    bool pageArrows_ = false;
    bool expandPanel_ = false;
    std::shared_ptr<msimeui::StackPanel> root_;
    // 装饰框：装饰图铺满，叠加的候选词按皮肤的对齐与偏移摆在上面。
    std::shared_ptr<msimeui::OverlayPanel> decorationBox_;
    std::shared_ptr<msimeui::Image> decoration_;
    std::shared_ptr<msimeui::TextBlock> decorationWord_;
    // 当前皮肤的叠加设置；皮肤没装饰图或没开 candidate_overlay 时为空。
    std::optional<CandidateSkinCatalog::DecorationCandidate> decorationCandidate_;
    float decorationBoxWidthDip_ = 0.0f;
    float decorationBoxHeightDip_ = 0.0f;
    // 正画着的词：同一个词不再重新量字号。
    std::wstring decorationWordShown_;
    std::shared_ptr<msimeui::Card> card_;
    // 卡片与 v2 挂件层放在同一格里：挂件层测量为 0，格子仍由卡片决定，挂件按 anchor 画到卡片外沿。
    // 网格按子元素顺序绘制：下层挂件、卡片、上层挂件。
    std::shared_ptr<msimeui::Grid> overlayGrid_;
    std::shared_ptr<CandidateSkinOverlay> skinOverlayBack_;
    std::shared_ptr<CandidateSkinOverlay> skinOverlayFront_;
    // 装饰图与卡片同列：列宽即卡片宽，装饰图在列内按 align 对齐。
    std::shared_ptr<msimeui::StackPanel> cardColumn_;
    std::shared_ptr<msimeui::Container> frame_;
    std::shared_ptr<msimeui::StackPanel> body_;
    std::shared_ptr<msimeui::TextBlock> preedit_;
    std::shared_ptr<msimeui::CandidateList> list_;
    std::shared_ptr<msimeui::PagerArrows> pager_;
    std::shared_ptr<CandidateExpandToggle> expandToggle_;
    std::shared_ptr<msimeui::CandidateList> expandGrid_;
    std::shared_ptr<CandidateExpandContent> expandContent_;
    std::shared_ptr<msimeui::ScrollViewer> expandScroller_;
    std::shared_ptr<CandidateExpandBody> expandBody_;
};

// body（预编辑行、候选列表、竖排翻页行）各行之间的固定行距。
inline constexpr float kCandidateBodyRowSpacingDip = 2.0f;
// 卡片默认的最小宽度；皮肤的 min_width_dip 与装饰图宽度可以把它撑大，fit_content 时没有。
inline constexpr float kCandidateMinWidthDip = 160.0f;
