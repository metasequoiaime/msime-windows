#include "window/candidate_skin_tokens.h"

#include "window/candidate_skin_palette.h"

#include <algorithm>

namespace
{
void ApplyColors(const CandidateSkinCatalog::CandidateColors &colors, CandSkinTokens &tokens)
{
    const auto apply = [](const std::string &value, D2D1_COLOR_F &out) {
        if (!value.empty())
            out = ParseCandidateCssColor(value, out);
    };
    apply(colors.preeditCaret, tokens.preeditCaret);
    apply(colors.selectedBar, tokens.selectedBar);
    apply(colors.selectedText, tokens.rowTextSelected);
    apply(colors.selectedNumber, tokens.rowLabelSelected);
    // 换了翻译色就不再用下层（基底）的词性/分隔符配色：普通行回到与翻译同色，选中行跟随 selected_translation。
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
    apply(colors.pageArrowDivider, tokens.pagerDivider);
    apply(colors.menuBackground, tokens.menuFill);
    apply(colors.menuBorder, tokens.menuBorder);
    apply(colors.menuText, tokens.menuText);
    apply(colors.menuHover, tokens.menuHover);
    apply(colors.accent, tokens.accent);
    apply(colors.selected, tokens.selected);
    apply(colors.hover, tokens.hover);
    apply(colors.number, tokens.number);
    apply(colors.translation, tokens.translation);
    if (colors.showSelectedBar.has_value())
    {
        tokens.showSelectedBar = *colors.showSelectedBar;
    }
}

void ApplyGeometry(const CandidateSkinCatalog::CandidateGeometry &geometry, float fontSize, CandSkinTokens &tokens,
                   bool &cornersFollowCard)
{
    const auto length = [fontSize](const std::optional<CandidateSkinCatalog::Length> &value, float &out) {
        if (value)
            out = CandidateSkinCatalog::ResolveLength(*value, fontSize);
    };
    length(geometry.padding, tokens.containerPad);
    length(geometry.minWidthDip, tokens.minWidthDip);
    length(geometry.itemGap, tokens.itemGap);
    length(geometry.itemExtraHeight, tokens.itemExtraHeight);
    if (geometry.numberInset)
    {
        tokens.itemPadLeft = (std::max)(0.0f, CandidateSkinCatalog::ResolveLength(*geometry.numberInset, fontSize) -
                                                  kCandidateNumberTextPad);
    }
    length(geometry.itemPaddingRight, tokens.itemPadRight);
    length(geometry.itemMinWidth, tokens.minItemWidth);
    length(geometry.numberGap, tokens.labelGap);
    length(geometry.selectedBarWidth, tokens.selectedBarWidth);
    if (geometry.numberScale)
        tokens.labelScale = static_cast<float>(*geometry.numberScale);
    if (geometry.selectedBarHeightEm)
        tokens.selectedBarHeightEm = static_cast<float>(*geometry.selectedBarHeightEm);
    if (geometry.selectedBarInside)
        tokens.selectedBarInside = *geometry.selectedBarInside;
    if (geometry.itemCornersFollowCard)
        cornersFollowCard = *geometry.itemCornersFollowCard;
    if (geometry.justifyRows)
        tokens.justifyRows = *geometry.justifyRows;
    if (geometry.trimHiddenPreeditGap)
        tokens.trimHiddenPreeditGap = *geometry.trimHiddenPreeditGap;
    if (!geometry.pageArrowStyle.empty())
        tokens.trianglePager = geometry.pageArrowStyle == "triangle";
    if (geometry.preeditMarginDip)
    {
        const CandidateSkinCatalog::InsetsDip &margin = *geometry.preeditMarginDip;
        tokens.preeditMargin = {static_cast<float>(margin.left), static_cast<float>(margin.top),
                                static_cast<float>(margin.right), static_cast<float>(margin.bottom)};
    }
}

void ApplyLayer(const CandidateSkinCatalog::Package &layer, bool light, bool horizontal, float fontSize,
                CandSkinTokens &tokens, bool &cornersFollowCard)
{
    ApplyGeometry(layer.geometry, fontSize, tokens, cornersFollowCard);
    ApplyGeometry(horizontal ? layer.horizontalGeometry : layer.verticalGeometry, fontSize, tokens, cornersFollowCard);
    if (layer.cornerRadiusDip)
        tokens.radius = static_cast<float>(*layer.cornerRadiusDip);
    if (layer.borderWidthDip)
        tokens.borderWidth = static_cast<float>(*layer.borderWidthDip);
    if (layer.itemCornerRadiusDip)
        tokens.itemRadius = static_cast<float>(*layer.itemCornerRadiusDip);
    ApplyColors(light ? layer.light : layer.dark, tokens);
}
} // namespace

CandSkinTokens ResolveCandidateSkinTokens(const CandidateSkinCatalog::Package &baseLook,
                                          const CandidateSkinCatalog::Package *package, bool light, bool horizontal,
                                          float fontSize)
{
    CandSkinTokens tokens;
    bool cornersFollowCard = false;
    ApplyLayer(baseLook, light, horizontal, fontSize, tokens, cornersFollowCard);
    if (package)
        ApplyLayer(*package, light, horizontal, fontSize, tokens, cornersFollowCard);
    // 贴角规则以 outerItemRadius > 0 为开关；圆角为 0 时仍要保持开启，否则贴角的高亮会退回普通圆角、
    // 从直角外框里探出来。
    tokens.outerItemRadius = cornersFollowCard ? (std::max)(tokens.radius, 0.01f) : 0.0f;
    return tokens;
}

std::string ResolveCandidateSkinShadow(const CandidateSkinCatalog::Package &baseLook,
                                       const CandidateSkinCatalog::Package *package)
{
    return package && !package->shadow.empty() ? package->shadow : baseLook.shadow;
}
