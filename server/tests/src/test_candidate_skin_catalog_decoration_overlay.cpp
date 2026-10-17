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

std::filesystem::path WriteSkin(const std::wstring &leaf, const std::string &decoration)
{
    namespace fs = std::filesystem;
    const fs::path skins_root =
        fs::temp_directory_path() / (L"msime-skin-deco-" + std::to_wstring(GetCurrentProcessId())) / leaf / L"skins";
    std::error_code ec;
    fs::remove_all(skins_root, ec);
    fs::create_directories(skins_root / L"art" / L"assets", ec);
    std::ofstream file(skins_root / L"art" / L"assets" / L"deco.png", std::ios::binary | std::ios::trunc);
    std::ofstream manifest(skins_root / L"art" / L"skin.toml", std::ios::binary | std::ios::trunc);
    manifest << kManifestHead << "[candidate_window]\n" << decoration;
    return skins_root;
}

void RemoveSkin(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

bool LoadFails(const std::wstring &leaf, const std::string &decoration)
{
    const auto root = WriteSkin(leaf, decoration);
    std::string error;
    const bool failed = !CandidateSkinCatalog::Load(root, "art", &error).has_value();
    RemoveSkin(root);
    return failed;
}
} // namespace

TEST_CASE(candidate_skin_catalog_reads_decoration_candidate_overlay)
{
    const auto root = WriteSkin(L"valid", R"(
[candidate_window.decoration]
image = "assets/deco.png"
top_inset_dip = 88
width_dip = 136
candidate_overlay = true
candidate_overlay_font_size_dip = 18
candidate_overlay_color = "#de4045"
candidate_overlay_align_h = "right"
candidate_overlay_align_v = "bottom"
candidate_overlay_offset_x_dip = -3
candidate_overlay_offset_y_dip = 5
candidate_overlay_strip_badge = false
candidate_overlay_max_width_dip = 120
candidate_overlay_min_font_size_dip = 6
candidate_overlay_index = 2
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->decorationCandidateOverlay.has_value());
    REQUIRE(*package->decorationCandidateOverlay);
    REQUIRE_EQ(package->decorationCandidateFontSizeDip, 18.0);
    REQUIRE_EQ(package->decorationCandidateColor, std::string("#de4045"));
    REQUIRE_EQ(package->decorationCandidateAlignH, std::string("right"));
    REQUIRE_EQ(package->decorationCandidateAlignV, std::string("bottom"));
    REQUIRE_EQ(package->decorationCandidateOffsetXDip, -3.0);
    REQUIRE_EQ(package->decorationCandidateOffsetYDip, 5.0);
    REQUIRE(!package->decorationCandidateStripBadge);
    REQUIRE_EQ(package->decorationCandidateMaxWidthDip, 120.0);
    REQUIRE_EQ(package->decorationCandidateMinFontSizeDip, 6.0);
    REQUIRE_EQ(package->decorationCandidateIndex, 2);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_decoration_candidate_overlay_defaults)
{
    const auto root = WriteSkin(L"defaults", R"(
[candidate_window.decoration]
image = "assets/deco.png"
top_inset_dip = 88
width_dip = 136
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(!package->decorationCandidateOverlay.has_value());
    REQUIRE_EQ(package->decorationCandidateFontSizeDip, 16.0);
    REQUIRE_EQ(package->decorationCandidateAlignH, std::string("center"));
    REQUIRE_EQ(package->decorationCandidateAlignV, std::string("center"));
    REQUIRE(package->decorationCandidateStripBadge);
    REQUIRE_EQ(package->decorationCandidateIndex, 1);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_decoration_candidate_overlay)
{
    REQUIRE(LoadFails(L"bad-align-h", R"(
[candidate_window.decoration]
image = "assets/deco.png"
top_inset_dip = 88
width_dip = 136
candidate_overlay_align_h = "middle"
)"));
    REQUIRE(LoadFails(L"bad-overlay", R"(
[candidate_window.decoration]
image = "assets/deco.png"
top_inset_dip = 88
width_dip = 136
candidate_overlay = "yes"
)"));
}
