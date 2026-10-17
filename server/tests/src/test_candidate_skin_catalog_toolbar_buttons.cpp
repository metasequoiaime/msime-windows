#include "tests/includes/test_framework.h"

#include "skin/candidate_skin_catalog.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
constexpr const char *kManifestHead = R"(schema_version = 1
id = "art"
name = "Art"
version = "1.0.0"
base = "fluent"

[supports]
layouts = ["horizontal", "vertical"]
themes = ["dark", "light"]
)";

std::filesystem::path WriteSkin(const std::wstring &leaf, const std::string &toolbar)
{
    namespace fs = std::filesystem;
    const fs::path skins_root =
        fs::temp_directory_path() / (L"msime-skin-toolbar-" + std::to_wstring(GetCurrentProcessId())) / leaf / L"skins";
    std::error_code ec;
    fs::remove_all(skins_root, ec);
    fs::create_directories(skins_root / L"art" / L"assets", ec);
    for (const wchar_t *image : {L"assets\\bar.png", L"assets\\quan.png", L"assets\\ban.png"})
    {
        std::ofstream file(skins_root / L"art" / image, std::ios::binary | std::ios::trunc);
    }
    std::ofstream manifest(skins_root / L"art" / L"skin.toml", std::ios::binary | std::ios::trunc);
    manifest << kManifestHead << "[candidate_window]\n" << toolbar;
    return skins_root;
}

void RemoveSkin(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

bool LoadFails(const std::wstring &leaf, const std::string &toolbar)
{
    const auto root = WriteSkin(leaf, toolbar);
    std::string error;
    const bool failed = !CandidateSkinCatalog::Load(root, "art", &error).has_value();
    RemoveSkin(root);
    return failed;
}
} // namespace

TEST_CASE(candidate_skin_catalog_reads_toolbar_image_mode_and_buttons)
{
    const auto root = WriteSkin(L"valid", R"(
[toolbar]
mode = "image"
image = "assets/bar.png"
image_width_dip = 129
image_height_dip = 134

[toolbar.buttons.fullwidth]
x_dip = 90
y_dip = 85.7
size_dip = 22.9

[toolbar.buttons.fullwidth.images]
full = "assets/quan.png"
half = "assets/ban.png"
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->toolbarMode, std::string("image"));
    REQUIRE_EQ(package->toolbarImage, std::string("assets/bar.png"));
    REQUIRE_EQ(package->toolbarImageWidthDip, 129.0);
    REQUIRE_EQ(package->toolbarImageHeightDip, 134.0);
    REQUIRE_EQ(package->toolbarButtons.items.size(), static_cast<size_t>(1));
    const auto &button = package->toolbarButtons.items.at("fullwidth");
    REQUIRE(button.xDip.has_value());
    REQUIRE_EQ(*button.xDip, 90.0);
    REQUIRE(button.yDip.has_value());
    REQUIRE_EQ(*button.yDip, 85.7);
    REQUIRE(button.sizeDip.has_value());
    REQUIRE_EQ(*button.sizeDip, 22.9);
    REQUIRE_EQ(button.images.states.at("full"), std::string("assets/quan.png"));
    REQUIRE_EQ(button.images.states.at("half"), std::string("assets/ban.png"));
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_toolbar_image_mode_requires_image_and_size)
{
    REQUIRE(LoadFails(L"no-image", "[toolbar]\nmode = \"image\"\nimage_width_dip = 129\nimage_height_dip = 134\n"));
    REQUIRE(LoadFails(L"no-size", "[toolbar]\nmode = \"image\"\nimage = \"assets/bar.png\"\n"));
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_toolbar_mode)
{
    REQUIRE(LoadFails(L"bad-mode", "[toolbar]\nmode = \"fancy\"\n"));
}

TEST_CASE(candidate_skin_catalog_default_toolbar_mode_is_bar)
{
    const auto root = WriteSkin(L"bar", "[toolbar]\n");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->toolbarMode, std::string("bar"));
    REQUIRE(package->toolbarImage.empty());
    REQUIRE(package->toolbarButtons.items.empty());
    RemoveSkin(root);
}
