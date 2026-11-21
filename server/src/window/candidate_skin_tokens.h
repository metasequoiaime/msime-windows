#pragma once

#include "skin/candidate_skin_catalog.h"

#include <d2d1.h>
#include <string>

// 预编辑行的外边距（DIP），顺序同 msimeui::Thickness。
struct CandSkinMargin
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

// 候选框 D2D 绘制用到的皮肤参数。全部来自皮肤清单：先是内置基底（skins/default/<base>/skin.toml）的外观，
// 再叠上外部皮肤自己写的键；成员的初值只是清单两边都没写时的缺省值。
struct CandSkinTokens
{
    D2D1_COLOR_F surface{};
    D2D1_COLOR_F border{};
    D2D1_COLOR_F text{};
    D2D1_COLOR_F number = D2D1::ColorF(0xE9E8E8, 0x9D / 255.0f);
    D2D1_COLOR_F selected = D2D1::ColorF(0x3E3E3E, 0xB9 / 255.0f);
    D2D1_COLOR_F hover = D2D1::ColorF(0x414141);
    D2D1_COLOR_F accent = D2D1::ColorF(0x6B69D6);
    // Alpha 0: follow the candidate text at the CSS opacity .62 (see CandidateList::Appearance).
    D2D1_COLOR_F translation = D2D1::ColorF(0, 0.0f);
    float radius = 6.0f;
    float borderWidth = 1.5f;
    float containerPad = 5.0f;
    // 当前排布的最小宽度（0 为不限）；与 [candidate_window] 的 min_width_dip 取大值。
    float minWidthDip = 0.0f;
    bool showSelectedBar = true;
    // 高亮圆角；outerItemRadius > 0 时，落在卡片四角上的那几个角改用它（跟卡片同圆角）。
    float itemRadius = 4.0f;
    float outerItemRadius = 0.0f;
    // 候选项的间距、比 1.35 倍字号多出的高度和左右内边距。
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
    CandSkinMargin preeditMargin{};
    // 选中行的文字与序号色；alpha 0 保持普通颜色（只铺选中条的皮肤）。
    D2D1_COLOR_F rowTextSelected = D2D1::ColorF(0, 0.0f);
    D2D1_COLOR_F rowLabelSelected = D2D1::ColorF(0, 0.0f);
    D2D1_COLOR_F menuFill = D2D1::ColorF(0x2D2D2D);
    D2D1_COLOR_F menuBorder = D2D1::ColorF(0x9B9B9B, 0x2E / 255.0f);
    D2D1_COLOR_F menuText = D2D1::ColorF(0xE9E8E8);
    D2D1_COLOR_F menuHover = D2D1::ColorF(0x414141);
    // 细分配色。alpha 0 表示没写：光标与选中条跟 accent，选中行翻译跟 translation，预编辑背景与分隔线不画。
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

// 候选列表在序号前固定留的空（CandidateList::Appearance::textPadLeft）；清单的 number_inset 从高亮左缘量到序号，
// 减去它才是列表的 contentPadLeft。
inline constexpr float kCandidateNumberTextPad = 5.0f;

// baseLook 是内置基底的外观（CandidateSkinCatalog::ResolveBuiltInLook），package 是外部皮肤（内置皮肤传 nullptr）。
// 配色、圆角与版式依次取基底、外部皮肤写了的值；em 长度按 fontSize 换算。surface / border / text 由
// ResolveCandidateSkinPalette 给出，这里不填。
CandSkinTokens ResolveCandidateSkinTokens(const CandidateSkinCatalog::Package &baseLook,
                                          const CandidateSkinCatalog::Package *package, bool light, bool horizontal,
                                          float fontSize);

// 卡片阴影档位：外部皮肤写了就用它的，否则用基底的；为空表示默认阴影。
std::string ResolveCandidateSkinShadow(const CandidateSkinCatalog::Package &baseLook,
                                       const CandidateSkinCatalog::Package *package);
