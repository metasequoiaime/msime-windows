#include "tests/includes/test_framework.h"

#include "skin/candidate_skin_catalog.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
constexpr const char *kManifestHead = R"(schema_version = 2
id = "art"
name = "Art"
version = "1.0.0"
base = "canvas"

[supports]
layouts = ["horizontal", "vertical"]
themes = ["dark", "light"]
)";

std::filesystem::path WriteSkin(const std::wstring &leaf, const std::string &window)
{
    namespace fs = std::filesystem;
    const fs::path skins_root =
        fs::temp_directory_path() / (L"msime-skin-border-" + std::to_wstring(GetCurrentProcessId())) / leaf / L"skins";
    std::error_code ec;
    fs::remove_all(skins_root, ec);
    fs::create_directories(skins_root / L"art" / L"assets", ec);
    REQUIRE(!ec);
    for (const wchar_t *image : {L"assets\\horizontal.png", L"assets\\vertical.png"})
    {
        std::ofstream file(skins_root / L"art" / image, std::ios::binary | std::ios::trunc);
    }
    std::ofstream manifest(skins_root / L"art" / L"skin.toml", std::ios::binary | std::ios::trunc);
    manifest << kManifestHead << "[candidate_window]\n" << window;
    return skins_root;
}

void RemoveSkin(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

bool LoadFails(const std::wstring &leaf, const std::string &window)
{
    const auto root = WriteSkin(leaf, window);
    std::string error;
    const bool failed = !CandidateSkinCatalog::Load(root, "art", &error).has_value() && !error.empty();
    RemoveSkin(root);
    return failed;
}
} // namespace

TEST_CASE(candidate_skin_catalog_reads_vertical_border_image_override)
{
    const auto root = WriteSkin(L"override", R"(
[candidate_window.border_image]
source = "assets/horizontal.png"
slice_dip = [8, 45, 6, 66]

[candidate_window.border_image.vertical]
source = "assets/vertical.png"
slice_dip = [26, 10, 26, 10]
repeat = ["stretch", "repeat"]
fill = false
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->borderImage.has_value());
    REQUIRE_EQ(package->borderImage->source, std::string("assets/horizontal.png"));
    REQUIRE(package->borderImage->repeatX == CandidateSkinCatalog::BorderImageRepeat::Stretch);
    REQUIRE(package->borderImage->repeatY == CandidateSkinCatalog::BorderImageRepeat::Stretch);

    REQUIRE(package->borderImageVertical.has_value());
    const CandidateSkinCatalog::BorderImage &vertical = *package->borderImageVertical;
    REQUIRE_EQ(vertical.source, std::string("assets/vertical.png"));
    REQUIRE_EQ(vertical.sliceDip.size(), static_cast<size_t>(4));
    REQUIRE_EQ(vertical.sliceDip[0], 26.0);
    REQUIRE_EQ(vertical.sliceDip[2], 26.0);
    REQUIRE(vertical.repeatX == CandidateSkinCatalog::BorderImageRepeat::Stretch);
    REQUIRE(vertical.repeatY == CandidateSkinCatalog::BorderImageRepeat::Repeat);
    REQUIRE(!vertical.fill);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_vertical_border_image_inherits_unset_keys)
{
    const auto root = WriteSkin(L"inherit", R"(
[candidate_window.border_image]
source = "assets/horizontal.png"
slice_dip = [8, 45, 6, 66]
fill = true

[candidate_window.border_image.vertical]
source = "assets/vertical.png"
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->borderImageVertical.has_value());
    const CandidateSkinCatalog::BorderImage &vertical = *package->borderImageVertical;
    REQUIRE_EQ(vertical.source, std::string("assets/vertical.png"));
    REQUIRE_EQ(vertical.sliceDip.size(), static_cast<size_t>(4));
    REQUIRE_EQ(vertical.sliceDip[1], 45.0);
    REQUIRE(vertical.fill);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_repeat_accepts_one_value_for_both_axes)
{
    const auto root = WriteSkin(L"both", R"(
[candidate_window.border_image]
source = "assets/horizontal.png"
slice_dip = [8, 45, 6, 66]
repeat = "repeat"
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->borderImage->repeatX == CandidateSkinCatalog::BorderImageRepeat::Repeat);
    REQUIRE(package->borderImage->repeatY == CandidateSkinCatalog::BorderImageRepeat::Repeat);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_repeat_accepts_round_and_space)
{
    const auto root = WriteSkin(L"round-space", R"(
[candidate_window.border_image]
source = "assets/horizontal.png"
slice_dip = [8, 45, 6, 66]
repeat = ["round", "space"]
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->borderImage->repeatX == CandidateSkinCatalog::BorderImageRepeat::Round);
    REQUIRE(package->borderImage->repeatY == CandidateSkinCatalog::BorderImageRepeat::Space);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_border_image_repeat)
{
    const std::string head = "[candidate_window.border_image]\nsource = \"assets/horizontal.png\"\n"
                             "slice_dip = [8, 45, 6, 66]\n";
    REQUIRE(LoadFails(L"bad-value", head + "repeat = \"tile\"\n"));
    REQUIRE(LoadFails(L"too-many", head + "repeat = [\"stretch\", \"repeat\", \"stretch\"]\n"));
    REQUIRE(LoadFails(L"not-string", head + "repeat = 1\n"));
    REQUIRE(LoadFails(L"vertical-not-table", head + "vertical = \"assets/vertical.png\"\n"));
}

TEST_CASE(candidate_skin_catalog_rejects_missing_vertical_border_image_file)
{
    REQUIRE(LoadFails(L"missing", R"(
[candidate_window.border_image]
source = "assets/horizontal.png"
slice_dip = [8, 45, 6, 66]

[candidate_window.border_image.vertical]
source = "assets/missing.png"
)"));
}

TEST_CASE(candidate_skin_catalog_vertical_border_image_does_not_inherit_light_source)
{
    const auto root = WriteSkin(L"light", R"(
[candidate_window.border_image]
source = "assets/horizontal.png"
light_source = "assets/horizontal.png"
slice_dip = [8, 45, 6, 66]

[candidate_window.border_image.vertical]
source = "assets/vertical.png"
slice_dip = [26, 10, 26, 10]
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->borderImage->lightSource, std::string("assets/horizontal.png"));
    // 横排那张浅色图是按横排几何画的，竖排不能跟着继承。
    REQUIRE(package->borderImageVertical->lightSource.empty());
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_reads_horizontal_border_image_override)
{
    const auto root = WriteSkin(L"horizontal", R"(
[candidate_window.border_image]
source = "assets/horizontal.png"
slice_dip = [8, 45, 6, 66]

[candidate_window.border_image.horizontal]
slice_dip = [10, 20, 10, 20]
repeat = ["repeat", "round"]
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    // 横排子表只覆盖写了的键，没写 source 时沿用共用那张图。
    REQUIRE(package->borderImageHorizontal.has_value());
    const CandidateSkinCatalog::BorderImage &horizontal = *package->borderImageHorizontal;
    REQUIRE_EQ(horizontal.source, std::string("assets/horizontal.png"));
    REQUIRE_EQ(horizontal.sliceDip.size(), static_cast<size_t>(4));
    REQUIRE_EQ(horizontal.sliceDip[0], 10.0);
    REQUIRE_EQ(horizontal.sliceDip[1], 20.0);
    REQUIRE(horizontal.repeatX == CandidateSkinCatalog::BorderImageRepeat::Repeat);
    REQUIRE(horizontal.repeatY == CandidateSkinCatalog::BorderImageRepeat::Round);
    // 没写竖排子表时竖排仍用共用那份。
    REQUIRE(!package->borderImageVertical.has_value());
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_override_light_source_follows_the_source_image)
{
    // 覆盖表只改几何、不动 source：共用那张浅色图继续有效。
    const auto inherited = WriteSkin(L"light-inherit", R"(
[candidate_window.border_image]
source = "assets/horizontal.png"
light_source = "assets/horizontal.png"
slice_dip = [8, 45, 6, 66]

[candidate_window.border_image.horizontal]
slice_dip = [10, 20, 10, 20]
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(inherited, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->borderImageHorizontal->lightSource, std::string("assets/horizontal.png"));
    RemoveSkin(inherited);

    // 换了 source 又没写 light_source：浅色图是按另一张图的几何画的，不能沿用。
    const auto replaced = WriteSkin(L"light-drop", R"(
[candidate_window.border_image]
source = "assets/horizontal.png"
light_source = "assets/horizontal.png"
slice_dip = [8, 45, 6, 66]

[candidate_window.border_image.horizontal]
source = "assets/vertical.png"
)");
    const auto replacedPackage = CandidateSkinCatalog::Load(replaced, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(replacedPackage.has_value());
    REQUIRE(replacedPackage->borderImageHorizontal->lightSource.empty());
    RemoveSkin(replaced);
}

TEST_CASE(candidate_skin_catalog_reads_horizontal_content_insets_override)
{
    const auto root = WriteSkin(L"insets-horizontal", R"(
[candidate_window.border_image]
source = "assets/horizontal.png"
slice_dip = [8, 45, 6, 66]

[candidate_window.content_insets_dip]
top = 10
right = 150
bottom = 12
left = 66
clamp_to_border = true

[candidate_window.content_insets_dip.horizontal]
top = 4
clamp_to_border = false
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->contentInsetsDipHorizontal.has_value());
    const CandidateSkinCatalog::InsetsDip &horizontal = *package->contentInsetsDipHorizontal;
    // 只覆盖 top，其余沿用共用那份。
    REQUIRE_EQ(horizontal.top, 4.0);
    REQUIRE_EQ(horizontal.right, 150.0);
    REQUIRE_EQ(horizontal.bottom, 12.0);
    REQUIRE_EQ(horizontal.left, 66.0);
    REQUIRE(package->contentInsetsClampToBorderHorizontal.has_value());
    REQUIRE(!*package->contentInsetsClampToBorderHorizontal);
    // 共用表写的是 true；竖排子表没写 clamp_to_border，保持未设置（解析时沿用共用值）。
    REQUIRE(package->contentInsetsClampToBorder);
    REQUIRE(!package->contentInsetsClampToBorderVertical.has_value());
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_reads_horizontal_outer_insets_override)
{
    const auto root = WriteSkin(L"outer-horizontal", R"(
[candidate_window.outer_insets_dip]
top = 95
right = 8

[candidate_window.outer_insets_dip.horizontal]
top = 0
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->outerInsetsDipHorizontal.has_value());
    REQUIRE_EQ(package->outerInsetsDipHorizontal->top, 0.0);
    REQUIRE_EQ(package->outerInsetsDipHorizontal->right, 8.0);
    REQUIRE(!package->outerInsetsDipVertical.has_value());
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_reads_vertical_content_insets_override)
{
    const auto root = WriteSkin(L"insets", R"(
[candidate_window.border_image]
source = "assets/horizontal.png"
slice_dip = [8, 45, 6, 66]

[candidate_window.content_insets_dip]
top = 10
right = 150
bottom = 12
left = 66

[candidate_window.content_insets_dip.vertical]
right = 12
left = 10
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->contentInsetsDip.has_value());
    REQUIRE_EQ(package->contentInsetsDip->left, 66.0);
    REQUIRE(package->contentInsetsDipVertical.has_value());
    const CandidateSkinCatalog::InsetsDip &vertical = *package->contentInsetsDipVertical;
    REQUIRE_EQ(vertical.top, 10.0);
    REQUIRE_EQ(vertical.right, 12.0);
    REQUIRE_EQ(vertical.bottom, 12.0);
    REQUIRE_EQ(vertical.left, 10.0);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_vertical_content_insets)
{
    const std::string head = "[candidate_window.content_insets_dip]\ntop = 10\n";
    REQUIRE(LoadFails(L"insets-not-table", head + "vertical = 4\n"));
    REQUIRE(LoadFails(L"insets-negative", head + "vertical = { left = -1 }\n"));
    REQUIRE(LoadFails(L"insets-too-big", head + "vertical = { right = 501 }\n"));
}

TEST_CASE(candidate_skin_catalog_rejects_missing_horizontal_border_image)
{
    // horizontal 覆盖和 vertical 一样要检查图片在不在，写错路径时整套皮肤加载失败而不是画成透明。
    REQUIRE(LoadFails(L"horizontal-missing-source", R"(
[candidate_window.border_image]
source = "assets/vertical.png"
slice_dip = [8]

[candidate_window.border_image.horizontal]
source = "assets/typo.png"
)"));
    REQUIRE(LoadFails(L"horizontal-missing-light", R"(
[candidate_window.border_image]
source = "assets/vertical.png"
slice_dip = [8]

[candidate_window.border_image.horizontal]
source = "assets/horizontal.png"
light_source = "assets/typo.png"
)"));
    const auto root = WriteSkin(L"horizontal-present", R"(
[candidate_window.border_image]
source = "assets/vertical.png"
slice_dip = [8]

[candidate_window.border_image.horizontal]
source = "assets/horizontal.png"
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(package.has_value());
    REQUIRE(package->borderImageHorizontal.has_value());
    REQUIRE_EQ(package->borderImageHorizontal->source, std::string("assets/horizontal.png"));
    RemoveSkin(root);
}