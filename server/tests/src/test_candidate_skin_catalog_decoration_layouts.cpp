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
    const fs::path skins_root = fs::temp_directory_path() /
                                (L"msime-skin-decoration-layout-" + std::to_wstring(GetCurrentProcessId())) / leaf /
                                L"skins";
    std::error_code ec;
    fs::remove_all(skins_root, ec);
    fs::create_directories(skins_root / L"art" / L"assets", ec);
    REQUIRE(!ec);
    for (const wchar_t *image : {L"assets\\dec.png", L"assets\\other.png"})
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

TEST_CASE(candidate_skin_catalog_reads_decoration_layouts)
{
    const auto root = WriteSkin(L"layouts", R"(
[[candidate_window.decorations]]
image = "assets/dec.png"
width_dip = 120
height_dip = 140

[[candidate_window.decorations]]
image = "assets/other.png"
width_dip = 40
height_dip = 40
layouts = ["horizontal"]
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->freeDecorations.size(), static_cast<size_t>(2));
    // 不写 layouts 的挂件横竖都画。
    REQUIRE(package->freeDecorations[0].layouts.empty());
    REQUIRE_EQ(package->freeDecorations[1].layouts, std::vector<std::string>({"horizontal"}));
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_decoration_layouts)
{
    REQUIRE(LoadFails(L"unknown", R"(
[[candidate_window.decorations]]
image = "assets/dec.png"
width_dip = 10
height_dip = 10
layouts = ["diagonal"]
)"));
    REQUIRE(LoadFails(L"empty", R"(
[[candidate_window.decorations]]
image = "assets/dec.png"
width_dip = 10
height_dip = 10
layouts = []
)"));
}

TEST_CASE(candidate_skin_catalog_reads_vertical_outer_insets_override)
{
    const auto root = WriteSkin(L"outer", R"(
[candidate_window.outer_insets_dip]
top = 95
right = 8
bottom = 0
left = 0

[candidate_window.outer_insets_dip.vertical]
top = 0
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->outerInsetsDip.has_value());
    REQUIRE_EQ(package->outerInsetsDip->top, 95.0);
    REQUIRE(package->outerInsetsDipVertical.has_value());
    // 竖排只覆盖写了的 top，其余沿用横排。
    REQUIRE_EQ(package->outerInsetsDipVertical->top, 0.0);
    REQUIRE_EQ(package->outerInsetsDipVertical->right, 8.0);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_reads_content_insets_clamp_switch)
{
    const auto root = WriteSkin(L"clamp", R"(
[candidate_window.content_insets_dip]
left = 16
clamp_to_border = true

[candidate_window.content_insets_dip.vertical]
left = 10
clamp_to_border = false
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->contentInsetsClampToBorder);
    REQUIRE(package->contentInsetsClampToBorderVertical.has_value());
    REQUIRE(!*package->contentInsetsClampToBorderVertical);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_content_insets_clamp)
{
    REQUIRE(LoadFails(L"clamp-not-bool", "[candidate_window.content_insets_dip]\nleft = 10\nclamp_to_border = 1\n"));
    REQUIRE(LoadFails(L"clamp-not-bool-vertical",
                      "[candidate_window.content_insets_dip.vertical]\nleft = 10\nclamp_to_border = \"no\"\n"));
}

TEST_CASE(candidate_skin_catalog_reads_negative_preedit_margin)
{
    const auto root = WriteSkin(L"preedit", R"(
[candidate_window.vertical]
preedit_margin_dip = [-44, 0, 0, 0]
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    // 负的上外边距把预编辑行推进卡片的内边距里，只影响竖排。
    REQUIRE(package->verticalGeometry.preeditMarginDip.has_value());
    REQUIRE_EQ(package->verticalGeometry.preeditMarginDip->top, -44.0);
    REQUIRE(!package->geometry.preeditMarginDip.has_value());
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_rejects_out_of_range_preedit_margin)
{
    REQUIRE(LoadFails(L"preedit-too-negative", "[candidate_window.vertical]\npreedit_margin_dip = [-501]\n"));
    REQUIRE(LoadFails(L"preedit-too-big", "[candidate_window.vertical]\npreedit_margin_dip = [501]\n"));
}

TEST_CASE(candidate_skin_catalog_reads_legacy_decoration_layouts)
{
    const auto root = WriteSkin(L"legacy-decoration-layouts", R"(
[candidate_window.decoration]
image = "assets/dec.png"
top_inset_dip = 40
width_dip = 120
layouts = ["vertical"]
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    // 卡片上方那张老式装饰图也能限定排布；不写时横竖都画。
    REQUIRE_EQ(package->decorationLayouts, std::vector<std::string>({"vertical"}));
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_legacy_decoration_layouts)
{
    REQUIRE(LoadFails(L"legacy-decoration-layouts-bad",
                      "[candidate_window.decoration]\nimage = \"assets/dec.png\"\ntop_inset_dip = 40\n"
                      "width_dip = 120\nlayouts = [\"both\"]\n"));
    REQUIRE(LoadFails(L"legacy-decoration-layouts-empty",
                      "[candidate_window.decoration]\nimage = \"assets/dec.png\"\ntop_inset_dip = 40\n"
                      "width_dip = 120\nlayouts = []\n"));
}

TEST_CASE(candidate_skin_catalog_reads_vertical_min_width)
{
    const auto root = WriteSkin(L"min-width", R"(
[candidate_window.vertical]
min_width_dip = 200
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->verticalGeometry.minWidthDip.has_value());
    REQUIRE_EQ(package->verticalGeometry.minWidthDip->value, 200.0);
    REQUIRE(!package->geometry.minWidthDip.has_value());
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_vertical_min_width)
{
    REQUIRE(LoadFails(L"min-width-negative", "[candidate_window.vertical]\nmin_width_dip = -1\n"));
    REQUIRE(LoadFails(L"min-width-too-big", "[candidate_window.vertical]\nmin_width_dip = 1001\n"));
    REQUIRE(LoadFails(L"min-width-not-number", "[candidate_window.vertical]\nmin_width_dip = \"wide\"\n"));
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_vertical_outer_insets)
{
    const std::string head = "[candidate_window.outer_insets_dip]\ntop = 10\n";
    REQUIRE(LoadFails(L"outer-not-table", head + "vertical = 4\n"));
    REQUIRE(LoadFails(L"outer-negative", head + "vertical = { left = -1 }\n"));
    REQUIRE(LoadFails(L"outer-too-big", head + "vertical = { top = 501 }\n"));
}
