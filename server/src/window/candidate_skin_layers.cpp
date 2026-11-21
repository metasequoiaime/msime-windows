#include "window/candidate_skin_layers.h"

#include <algorithm>

namespace CandidateSkinLayers
{
Edges ExpandBox(const std::vector<double> &values)
{
    Edges edges;
    if (values.size() == 1)
    {
        edges = {values[0], values[0], values[0], values[0]};
    }
    else if (values.size() == 2)
    {
        edges = {values[0], values[1], values[0], values[1]};
    }
    else if (values.size() == 4)
    {
        edges = {values[0], values[1], values[2], values[3]};
    }
    return edges;
}

NineSlice ResolveNineSlice(const CandidateSkinCatalog::BorderImage &borderImage)
{
    NineSlice result;
    const std::vector<double> &slice = borderImage.stretchDip.empty() ? borderImage.sliceDip : borderImage.stretchDip;
    result.slice = ExpandBox(slice);
    result.width = borderImage.widthDip.empty() ? result.slice : ExpandBox(borderImage.widthDip);
    result.fill = borderImage.fill;
    result.repeatX = borderImage.repeatX;
    result.repeatY = borderImage.repeatY;
    return result;
}

const CandidateSkinCatalog::BorderImage *ResolveBorderImage(const CandidateSkinCatalog::Package &package,
                                                            CandidateLayout layout)
{
    const std::optional<CandidateSkinCatalog::BorderImage> &overrideImage =
        layout == CandidateLayout::Horizontal ? package.borderImageHorizontal : package.borderImageVertical;
    if (overrideImage)
    {
        return &*overrideImage;
    }
    return package.borderImage ? &*package.borderImage : nullptr;
}

const CandidateSkinCatalog::InsetsDip *ResolveContentInsets(const CandidateSkinCatalog::Package &package,
                                                            CandidateLayout layout)
{
    const std::optional<CandidateSkinCatalog::InsetsDip> &overrideInsets =
        layout == CandidateLayout::Horizontal ? package.contentInsetsDipHorizontal : package.contentInsetsDipVertical;
    if (overrideInsets)
    {
        return &*overrideInsets;
    }
    return package.contentInsetsDip ? &*package.contentInsetsDip : nullptr;
}

bool ResolveContentInsetsClampToBorder(const CandidateSkinCatalog::Package &package, CandidateLayout layout)
{
    const std::optional<bool> &overrideClamp = layout == CandidateLayout::Horizontal
                                                   ? package.contentInsetsClampToBorderHorizontal
                                                   : package.contentInsetsClampToBorderVertical;
    return overrideClamp.value_or(package.contentInsetsClampToBorder);
}

const CandidateSkinCatalog::InsetsDip *ResolveOuterInsets(const CandidateSkinCatalog::Package &package,
                                                          CandidateLayout layout)
{
    const std::optional<CandidateSkinCatalog::InsetsDip> &overrideInsets =
        layout == CandidateLayout::Horizontal ? package.outerInsetsDipHorizontal : package.outerInsetsDipVertical;
    if (overrideInsets)
    {
        return &*overrideInsets;
    }
    return package.outerInsetsDip ? &*package.outerInsetsDip : nullptr;
}

Edges ResolveContentPadding(const CandidateSkinCatalog::Package &package, double basePadding, CandidateLayout layout)
{
    const CandidateSkinCatalog::BorderImage *image = ResolveBorderImage(package, layout);
    const CandidateSkinCatalog::InsetsDip *insetsPtr = ResolveContentInsets(package, layout);
    Edges border;
    if (image && !image->source.empty())
    {
        border = ResolveNineSlice(*image).width;
    }
    const bool clampToBorder = ResolveContentInsetsClampToBorder(package, layout);
    if (insetsPtr)
    {
        const CandidateSkinCatalog::InsetsDip &insets = *insetsPtr;
        if (!clampToBorder)
        {
            // 皮肤关了取大值：贴图的边宽里是黑底、可见边框更窄时，按写的数值让出空间。
            return {insets.top, insets.right, insets.bottom, insets.left};
        }
        return {(std::max)(insets.top, border.top), (std::max)(insets.right, border.right),
                (std::max)(insets.bottom, border.bottom), (std::max)(insets.left, border.left)};
    }
    return {border.top + basePadding, border.right + basePadding, border.bottom + basePadding,
            border.left + basePadding};
}

bool MatchesLayout(const std::vector<std::string> &layouts, CandidateLayout layout)
{
    const char *wanted = layout == CandidateLayout::Horizontal ? "horizontal" : "vertical";
    return layouts.empty() || std::find(layouts.begin(), layouts.end(), wanted) != layouts.end();
}

Layers SplitDecorations(const std::vector<CandidateSkinCatalog::FreeDecoration> &decorations,
                        std::optional<CandidateLayout> layout)
{
    // 按排布筛选和排序前的拷贝合成一步，不另建一份筛选结果。
    std::vector<CandidateSkinCatalog::FreeDecoration> sorted =
        layout ? FilterDecorationsForLayout(decorations, *layout) : decorations;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const CandidateSkinCatalog::FreeDecoration &a, const CandidateSkinCatalog::FreeDecoration &b) {
                         return a.zIndex < b.zIndex;
                     });
    Layers layers;
    for (auto &decoration : sorted)
    {
        (decoration.zIndex < CandidateSkinCatalog::kDecorationFrontZIndex ? layers.below : layers.above)
            .push_back(std::move(decoration));
    }
    return layers;
}

std::vector<CandidateSkinCatalog::FreeDecoration> FilterDecorationsForLayout(
    const std::vector<CandidateSkinCatalog::FreeDecoration> &decorations, CandidateLayout layout)
{
    std::vector<CandidateSkinCatalog::FreeDecoration> filtered;
    filtered.reserve(decorations.size());
    for (const auto &decoration : decorations)
    {
        if (MatchesLayout(decoration.layouts, layout))
        {
            filtered.push_back(decoration);
        }
    }
    return filtered;
}
} // namespace CandidateSkinLayers
