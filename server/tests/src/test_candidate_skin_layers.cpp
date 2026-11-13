#include "tests/includes/test_framework.h"

#include "window/candidate_skin_layers.h"

#include <string>
#include <vector>

namespace
{
CandidateSkinCatalog::FreeDecoration Decoration(const std::string &image, int zIndex)
{
    CandidateSkinCatalog::FreeDecoration decoration;
    decoration.image = image;
    decoration.zIndex = zIndex;
    return decoration;
}

std::vector<std::string> Images(const std::vector<CandidateSkinCatalog::FreeDecoration> &layer)
{
    std::vector<std::string> images;
    for (const auto &decoration : layer)
        images.push_back(decoration.image);
    return images;
}

bool SameEdges(const CandidateSkinLayers::Edges &edges, double top, double right, double bottom, double left)
{
    return edges.top == top && edges.right == right && edges.bottom == bottom && edges.left == left;
}
} // namespace

TEST_CASE(candidate_skin_layers_expand_css_box_shorthand)
{
    REQUIRE(SameEdges(CandidateSkinLayers::ExpandBox({7}), 7, 7, 7, 7));
    REQUIRE(SameEdges(CandidateSkinLayers::ExpandBox({1, 2}), 1, 2, 1, 2));
    REQUIRE(SameEdges(CandidateSkinLayers::ExpandBox({1, 2, 3, 4}), 1, 2, 3, 4));
    REQUIRE(SameEdges(CandidateSkinLayers::ExpandBox({}), 0, 0, 0, 0));
}

TEST_CASE(candidate_skin_layers_nine_slice_follows_border_image)
{
    CandidateSkinCatalog::BorderImage image;
    image.sliceDip = {16, 44, 16, 0};
    // Without width_dip the borders are as many DIPs wide as the slices are image pixels.
    auto nine = CandidateSkinLayers::ResolveNineSlice(image);
    REQUIRE(SameEdges(nine.slice, 16, 44, 16, 0));
    REQUIRE(SameEdges(nine.width, 16, 44, 16, 0));
    REQUIRE(nine.fill);
    // stretch_dip replaces slice_dip; width_dip sets the drawn width (a 2x panel drawn at half its pixels).
    image.stretchDip = {32, 88};
    image.widthDip = {16, 44};
    image.fill = false;
    nine = CandidateSkinLayers::ResolveNineSlice(image);
    REQUIRE(SameEdges(nine.slice, 32, 88, 32, 88));
    REQUIRE(SameEdges(nine.width, 16, 44, 16, 44));
    REQUIRE(!nine.fill);
}

TEST_CASE(candidate_skin_layers_content_padding_is_measured_from_the_panel_edge)
{
    CandidateSkinCatalog::Package package;
    package.schemaVersion = 2;
    package.base = "canvas";
    // No panel, no insets: the base padding.
    REQUIRE(SameEdges(CandidateSkinLayers::ResolveContentPadding(package, 5.0), 5, 5, 5, 5));

    CandidateSkinCatalog::BorderImage image;
    image.source = "assets/panel.png";
    image.sliceDip = {24};
    image.widthDip = {12, 20};
    package.borderImage = image;
    // The panel's borders plus the base padding.
    REQUIRE(SameEdges(CandidateSkinLayers::ResolveContentPadding(package, 5.0), 17, 25, 17, 25));

    // content_insets_dip counts from the outer edge, but never puts text on the border.
    package.contentInsetsDip = CandidateSkinCatalog::InsetsDip{4.0, 130.0, 14.0, 55.0};
    REQUIRE(SameEdges(CandidateSkinLayers::ResolveContentPadding(package, 5.0), 12, 130, 14, 55));

    package.borderImage.reset();
    REQUIRE(SameEdges(CandidateSkinLayers::ResolveContentPadding(package, 5.0), 4, 130, 14, 55));
}

TEST_CASE(candidate_skin_layers_content_padding_can_skip_the_border_clamp)
{
    CandidateSkinCatalog::Package package;
    package.schemaVersion = 2;
    package.base = "canvas";
    CandidateSkinCatalog::BorderImage image;
    image.source = "assets/panel.png";
    image.sliceDip = {24};
    image.widthDip = {30, 40};
    package.borderImage = image;
    package.contentInsetsDip = CandidateSkinCatalog::InsetsDip{10.0, 12.0, 14.0, 16.0};
    // 默认仍与边宽取大值。
    REQUIRE(SameEdges(CandidateSkinLayers::ResolveContentPadding(package, 5.0), 30, 40, 30, 40));
    // 皮肤显式关掉后完全按写的数值，横竖各自一份。
    package.contentInsetsClampToBorder = false;
    REQUIRE(SameEdges(CandidateSkinLayers::ResolveContentPadding(package, 5.0), 10, 12, 14, 16));

    package.contentInsetsDipVertical = CandidateSkinCatalog::InsetsDip{4.0, 6.0, 8.0, 2.0};
    package.contentInsetsClampToBorderVertical = false;
    REQUIRE(SameEdges(
        CandidateSkinLayers::ResolveContentPadding(package, 5.0, CandidateSkinLayers::CandidateLayout::Vertical), 4, 6,
        8, 2));
    package.contentInsetsClampToBorder = true;
    REQUIRE(SameEdges(CandidateSkinLayers::ResolveContentPadding(package, 5.0), 30, 40, 30, 40));
}

TEST_CASE(candidate_skin_layers_vertical_content_padding_prefers_the_vertical_override)
{
    CandidateSkinCatalog::Package package;
    package.schemaVersion = 2;
    package.base = "canvas";
    package.contentInsetsDip = CandidateSkinCatalog::InsetsDip{10.0, 150.0, 12.0, 66.0};
    package.contentInsetsDipVertical = CandidateSkinCatalog::InsetsDip{6.0, 12.0, 8.0, 10.0};

    // 横排不受竖排覆盖影响。
    REQUIRE(SameEdges(CandidateSkinLayers::ResolveContentPadding(package, 5.0), 10, 150, 12, 66));
    REQUIRE(SameEdges(
        CandidateSkinLayers::ResolveContentPadding(package, 5.0, CandidateSkinLayers::CandidateLayout::Vertical), 6, 12,
        8, 10));

    // 竖排的九宫格边宽仍会把文字推到边框里面。
    CandidateSkinCatalog::BorderImage vertical;
    vertical.source = "assets/vertical.png";
    vertical.sliceDip = {26, 10, 26, 10};
    vertical.widthDip = {20};
    package.borderImageVertical = vertical;
    REQUIRE(SameEdges(CandidateSkinLayers::ResolveContentPadding(package, 5.0), 10, 150, 12, 66));
    REQUIRE(SameEdges(
        CandidateSkinLayers::ResolveContentPadding(package, 5.0, CandidateSkinLayers::CandidateLayout::Vertical), 20,
        20, 20, 20));

    // 没写竖排内边距时竖排沿用横排的值，只换成竖排的边宽。
    package.contentInsetsDipVertical.reset();
    REQUIRE(SameEdges(
        CandidateSkinLayers::ResolveContentPadding(package, 5.0, CandidateSkinLayers::CandidateLayout::Vertical), 20,
        150, 20, 66));
}

// 横排 / 竖排覆盖子表各自生效，互不继承；没写的排布回落到共用那份。
TEST_CASE(candidate_skin_layers_resolve_per_layout_overrides)
{
    using CandidateSkinLayout = CandidateSkinLayers::CandidateLayout;
    CandidateSkinCatalog::Package package;
    CandidateSkinCatalog::BorderImage base;
    base.source = "base.png";
    base.sliceDip = {8};
    CandidateSkinCatalog::BorderImage horizontal = base;
    horizontal.source = "horizontal.png";
    CandidateSkinCatalog::BorderImage vertical = base;
    vertical.source = "vertical.png";
    package.borderImage = base;
    package.borderImageHorizontal = horizontal;
    package.borderImageVertical = vertical;
    REQUIRE_EQ(CandidateSkinLayers::ResolveBorderImage(package, CandidateSkinLayout::Horizontal)->source,
               std::string("horizontal.png"));
    REQUIRE_EQ(CandidateSkinLayers::ResolveBorderImage(package, CandidateSkinLayout::Vertical)->source,
               std::string("vertical.png"));
    package.borderImageVertical.reset();
    REQUIRE_EQ(CandidateSkinLayers::ResolveBorderImage(package, CandidateSkinLayout::Vertical)->source,
               std::string("base.png"));

    package.contentInsetsDip = CandidateSkinCatalog::InsetsDip{1, 2, 3, 4};
    package.contentInsetsDipHorizontal = CandidateSkinCatalog::InsetsDip{5, 6, 7, 8};
    package.contentInsetsClampToBorderHorizontal = false;
    REQUIRE_EQ(CandidateSkinLayers::ResolveContentInsets(package, CandidateSkinLayout::Horizontal)->left, 8.0);
    REQUIRE_EQ(CandidateSkinLayers::ResolveContentInsets(package, CandidateSkinLayout::Vertical)->left, 4.0);
    REQUIRE(!CandidateSkinLayers::ResolveContentInsetsClampToBorder(package, CandidateSkinLayout::Horizontal));
    REQUIRE(CandidateSkinLayers::ResolveContentInsetsClampToBorder(package, CandidateSkinLayout::Vertical));

    package.outerInsetsDip = CandidateSkinCatalog::InsetsDip{95, 8, 0, 0};
    package.outerInsetsDipVertical = CandidateSkinCatalog::InsetsDip{0, 8, 0, 0};
    REQUIRE_EQ(CandidateSkinLayers::ResolveOuterInsets(package, CandidateSkinLayout::Horizontal)->top, 95.0);
    REQUIRE_EQ(CandidateSkinLayers::ResolveOuterInsets(package, CandidateSkinLayout::Vertical)->top, 0.0);
}

TEST_CASE(candidate_skin_layers_split_decorations_around_the_panel)
{
    const std::vector<CandidateSkinCatalog::FreeDecoration> decorations = {
        Decoration("a", 2), Decoration("b", 0), Decoration("c", -3), Decoration("d", 1),
        Decoration("e", 0), Decoration("f", 5), Decoration("g", 2),
    };
    const CandidateSkinLayers::Layers layers = CandidateSkinLayers::SplitDecorations(decorations);
    // Below the panel: z_index < 1, lowest first, ties in declaration order.
    REQUIRE_EQ(Images(layers.below), std::vector<std::string>({"c", "b", "e"}));
    // Above: z_index >= 1, the panel itself sitting at 1.
    REQUIRE_EQ(Images(layers.above), std::vector<std::string>({"d", "a", "g", "f"}));
    REQUIRE_EQ(CandidateSkinCatalog::kDecorationFrontZIndex, 1);

    // The default z_index (2) draws above the panel.
    REQUIRE(CandidateSkinLayers::SplitDecorations({CandidateSkinCatalog::FreeDecoration{}}).above.size() == 1);
}

TEST_CASE(candidate_skin_layers_filter_decorations_by_layout)
{
    CandidateSkinCatalog::FreeDecoration both = Decoration("both", 2);
    CandidateSkinCatalog::FreeDecoration horizontal = Decoration("horizontal", 2);
    horizontal.layouts = {"horizontal"};
    CandidateSkinCatalog::FreeDecoration vertical = Decoration("vertical", 2);
    vertical.layouts = {"vertical"};
    const std::vector<CandidateSkinCatalog::FreeDecoration> decorations = {both, horizontal, vertical};

    using Layout = CandidateSkinLayers::CandidateLayout;
    REQUIRE_EQ(Images(CandidateSkinLayers::FilterDecorationsForLayout(decorations, Layout::Horizontal)),
               std::vector<std::string>({"both", "horizontal"}));
    REQUIRE_EQ(Images(CandidateSkinLayers::FilterDecorationsForLayout(decorations, Layout::Vertical)),
               std::vector<std::string>({"both", "vertical"}));
    // 没写 layouts 的横竖都画；全都写了不匹配的排布时为空。
    REQUIRE(CandidateSkinLayers::FilterDecorationsForLayout({both}, Layout::Vertical).size() == 1);
    REQUIRE(CandidateSkinLayers::FilterDecorationsForLayout({vertical}, Layout::Horizontal).empty());
    // SplitDecorations 给了排布时先筛再分层，与老式装饰图共用同一个 MatchesLayout。
    const CandidateSkinLayers::Layers layers = CandidateSkinLayers::SplitDecorations(decorations, Layout::Vertical);
    REQUIRE_EQ(Images(layers.above), std::vector<std::string>({"both", "vertical"}));
    REQUIRE(CandidateSkinLayers::MatchesLayout({}, Layout::Horizontal));
    REQUIRE(CandidateSkinLayers::MatchesLayout({"horizontal", "vertical"}, Layout::Vertical));
    REQUIRE(!CandidateSkinLayers::MatchesLayout({"horizontal"}, Layout::Vertical));
}
