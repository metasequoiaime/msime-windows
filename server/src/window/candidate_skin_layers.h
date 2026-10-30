#pragma once

#include "skin/candidate_skin_catalog.h"

#include <vector>

// v2（canvas）皮肤在候选窗里的几何：九宫格切片与边宽、内容内边距、挂件分层。只做数值换算，
// 不碰窗口与渲染，D2D 候选窗和设置页预览共用这一份规则。
namespace CandidateSkinLayers
{
struct Edges
{
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double left = 0.0;
};

// CSS 四值简写：1 个值四边相同，2 个值是「上下 左右」，4 个值是「上右下左」。其余个数按缺省 0。
Edges ExpandBox(const std::vector<double> &values);

struct NineSlice
{
    // 源图像素：stretch_dip 给出时用它，否则用 slice_dip。
    Edges slice;
    // 屏幕上的边宽（DIP）：width_dip 给出时用它，否则取切片数值当 DIP（CSS border-image-width 缺省即 border-width，
    // v2 规定 border-width 等于切片数值）。
    Edges width;
    bool fill = true;
    // 同 CSS border-image-repeat。
    bool repeatX = false;
    bool repeatY = false;
};

NineSlice ResolveNineSlice(const CandidateSkinCatalog::BorderImage &borderImage);

// 面板外缘到内容的距离（DIP）。写了 content_insets_dip 时就是它，但不小于九宫格该边的边宽（文字不压在边框上，
// 与 CSS 的 border + padding 一致）；没写时是九宫格边宽加上基底自己的内边距。
// vertical 为 true 时，写了 border_image.vertical / content_insets_dip.vertical 就按竖排那套算，
// 没写的部分仍用横排的值。
Edges ResolveContentPadding(const CandidateSkinCatalog::Package &package, double basePadding, bool vertical = false);

struct Layers
{
    // z_index < kDecorationFrontZIndex，画在面板之下。
    std::vector<CandidateSkinCatalog::FreeDecoration> below;
    // 其余，画在面板与候选文字之上。
    std::vector<CandidateSkinCatalog::FreeDecoration> above;
};

// 按 z_index 升序稳定排序（同层保持声明顺序），再以 kDecorationFrontZIndex 为界分成两层。
Layers SplitDecorations(const std::vector<CandidateSkinCatalog::FreeDecoration> &decorations);
} // namespace CandidateSkinLayers
