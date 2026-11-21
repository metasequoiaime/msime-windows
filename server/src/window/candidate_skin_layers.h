#pragma once

#include "skin/candidate_skin_catalog.h"

#include <optional>
#include <string>
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
    CandidateSkinCatalog::BorderImageRepeat repeatX = CandidateSkinCatalog::BorderImageRepeat::Stretch;
    CandidateSkinCatalog::BorderImageRepeat repeatY = CandidateSkinCatalog::BorderImageRepeat::Stretch;
};

// 皮肤按排布选择参数：共用表 + horizontal / vertical 覆盖子表，子表只改写了的键，两者互不继承。
enum class CandidateLayout
{
    Horizontal,
    Vertical,
};

NineSlice ResolveNineSlice(const CandidateSkinCatalog::BorderImage &borderImage);

// 当前排布实际使用的九宫格 / 内容内边距 / 外预留；没有对应覆盖时回落到共用那份。
// 返回的是 package 里的对象，可以直接拿去渲染。
const CandidateSkinCatalog::BorderImage *ResolveBorderImage(const CandidateSkinCatalog::Package &package,
                                                            CandidateLayout layout);
const CandidateSkinCatalog::InsetsDip *ResolveContentInsets(const CandidateSkinCatalog::Package &package,
                                                            CandidateLayout layout);
bool ResolveContentInsetsClampToBorder(const CandidateSkinCatalog::Package &package, CandidateLayout layout);
const CandidateSkinCatalog::InsetsDip *ResolveOuterInsets(const CandidateSkinCatalog::Package &package,
                                                          CandidateLayout layout);

// 面板外缘到内容的距离（DIP）。写了 content_insets_dip 时就是它，但不小于九宫格该边的边宽（文字不压在边框上，
// 与 CSS 的 border + padding 一致）；没写时是九宫格边宽加上基底自己的内边距。
Edges ResolveContentPadding(const CandidateSkinCatalog::Package &package, double basePadding,
                            CandidateLayout layout = CandidateLayout::Horizontal);

struct Layers
{
    // z_index < kDecorationFrontZIndex，画在面板之下。
    std::vector<CandidateSkinCatalog::FreeDecoration> below;
    // 其余，画在面板与候选文字之上。
    std::vector<CandidateSkinCatalog::FreeDecoration> above;
};

// 挂件 / 装饰图的 layouts：空表示横竖都显示，否则只在列出的排布里显示。
bool MatchesLayout(const std::vector<std::string> &layouts, CandidateLayout layout);

// 按 z_index 升序稳定排序（同层保持声明顺序），再以 kDecorationFrontZIndex 为界分成两层。
// 给了 layout 时先去掉不在这种排布里显示的挂件。
Layers SplitDecorations(const std::vector<CandidateSkinCatalog::FreeDecoration> &decorations,
                        std::optional<CandidateLayout> layout = std::nullopt);

// 挂件声明了 layouts 时只保留当前排布能用的那些；没声明的横竖都保留。保持声明顺序。
std::vector<CandidateSkinCatalog::FreeDecoration> FilterDecorationsForLayout(
    const std::vector<CandidateSkinCatalog::FreeDecoration> &decorations, CandidateLayout layout);
} // namespace CandidateSkinLayers
