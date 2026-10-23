#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "skin/candidate_skin_catalog.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
constexpr const char *kManifestHead = R"(schema_version = 1
id = "gloss"
name = "Gloss"
version = "1.0.0"
base = "fluent"

[supports]
layouts = ["horizontal", "vertical"]
themes = ["dark", "light"]

[candidate_window]
)";

std::filesystem::path WriteSkin(const std::wstring &leaf, const std::string &candidateTables)
{
    namespace fs = std::filesystem;
    const fs::path skins_root =
        test::TempRoot() / (L"msime-skin-colors-" + std::to_wstring(GetCurrentProcessId())) / leaf / L"skins";
    std::error_code ec;
    fs::remove_all(skins_root, ec);
    fs::create_directories(skins_root / L"gloss", ec);
    REQUIRE(!ec);
    std::ofstream manifest(skins_root / L"gloss" / L"skin.toml", std::ios::binary | std::ios::trunc);
    REQUIRE(manifest.is_open());
    manifest << kManifestHead << candidateTables;
    return skins_root;
}

bool Rejects(const std::wstring &leaf, const std::string &candidateTables)
{
    const auto root = WriteSkin(leaf, candidateTables);
    std::string error;
    const bool rejected = !CandidateSkinCatalog::Load(root, "gloss", &error).has_value() && !error.empty();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return rejected;
}
} // namespace

// The translation after a candidate has its own manifest key so a skin can colour it independently of
// the candidate text; an unset key stays empty so the renderers keep deriving it from the text colour.
TEST_CASE(candidate_skin_catalog_reads_translation_color_per_theme)
{
    const auto root = WriteSkin(L"valid", R"(
[candidate.dark]
text = "#e0e0e0"
translation = "#e6a817"

[candidate.light]
text = "#202020"
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "gloss", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->dark.translation, std::string("#e6a817"));
    REQUIRE_EQ(package->dark.text, std::string("#e0e0e0"));
    REQUIRE(package->light.translation.empty());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// 词性与分隔符各有一组键；留空时保持为空，由渲染端跟随翻译色。
TEST_CASE(candidate_skin_catalog_reads_translation_part_colors)
{
    const auto root = WriteSkin(L"parts", R"toml(
[candidate.dark]
translation = "#529cca"
translation_pos = "#529cca99"
translation_separator = "#529cca73"
selected_translation_pos = "rgba(255, 255, 255, 0.5)"
selected_translation_separator = "#ffffff66"

[candidate.light]
translation = "#0b6e99"
)toml");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "gloss", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->dark.translationPos, std::string("#529cca99"));
    REQUIRE_EQ(package->dark.translationSeparator, std::string("#529cca73"));
    REQUIRE_EQ(package->dark.selectedTranslationPos, std::string("rgba(255, 255, 255, 0.5)"));
    REQUIRE_EQ(package->dark.selectedTranslationSeparator, std::string("#ffffff66"));
    REQUIRE(package->light.translationPos.empty());
    REQUIRE(package->light.translationSeparator.empty());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    REQUIRE(Rejects(L"parts-inject", "[candidate.dark]\ntranslation_pos = \"red; } body { display: none\"\n"));
}

TEST_CASE(candidate_skin_catalog_rejects_non_string_translation_color)
{
    const auto root = WriteSkin(L"invalid", R"(
[candidate.dark]
translation = 42
)");
    std::string error;
    REQUIRE(!CandidateSkinCatalog::Load(root, "gloss", &error).has_value());
    REQUIRE(!error.empty());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// Candidate colours follow CSS colour syntax shared by every platform's skin.toml, so a value that could close a
// declaration or rule must reject the whole skin.
TEST_CASE(candidate_skin_catalog_rejects_candidate_color_that_escapes_css)
{
    const auto root = WriteSkin(L"inject", R"(
[candidate.dark]
selected = "red; } body { display: none"
)");
    std::string error;
    REQUIRE(!CandidateSkinCatalog::Load(root, "gloss", &error).has_value());
    REQUIRE(!error.empty());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(candidate_skin_catalog_reads_detailed_colors_menu_and_window_options)
{
    const auto root = WriteSkin(L"options", R"toml(border_width_dip = 2
item_corner_radius_dip = 8
shadow = "soft"
backdrop = "acrylic"
font_family = "LXGW WenKai"

[candidate.dark]
candidate_text = "#dddddd"
preedit_text = "#aaaaaa"
preedit_caret = "#ff8800"
selected_text = "#ffffff"
selected_number = "#cccccc"
selected_translation = "#ffe0a0"
selected_bar = "#00aaff"
preedit_background = "rgba(255, 255, 255, 0.08)"
preedit_divider = "#444444"

backdrop_opacity = 0.7
backdrop_luminosity = 0.9

[candidate.dark.menu]
background = "#202020"
border = "#333333"
text = "#eeeeee"
hover = "#2a2a2a"
)toml");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "gloss", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->borderWidthDip.has_value());
    REQUIRE(*package->borderWidthDip == 2.0);
    REQUIRE(package->itemCornerRadiusDip.has_value());
    REQUIRE(*package->itemCornerRadiusDip == 8.0);
    REQUIRE_EQ(package->shadow, std::string("soft"));
    REQUIRE_EQ(package->backdrop, std::string("acrylic"));
    REQUIRE(package->dark.backdropOpacity.has_value());
    REQUIRE(*package->dark.backdropOpacity == 0.7);
    REQUIRE(*package->dark.backdropLuminosity == 0.9);
    REQUIRE(!package->light.backdropOpacity.has_value());
    REQUIRE_EQ(package->fontFamily, std::string("LXGW WenKai"));
    REQUIRE_EQ(package->dark.candidateText, std::string("#dddddd"));
    REQUIRE_EQ(package->dark.preeditText, std::string("#aaaaaa"));
    REQUIRE_EQ(package->dark.preeditCaret, std::string("#ff8800"));
    REQUIRE_EQ(package->dark.selectedText, std::string("#ffffff"));
    REQUIRE_EQ(package->dark.selectedNumber, std::string("#cccccc"));
    REQUIRE_EQ(package->dark.selectedTranslation, std::string("#ffe0a0"));
    REQUIRE_EQ(package->dark.selectedBar, std::string("#00aaff"));
    REQUIRE_EQ(package->dark.preeditBackground, std::string("rgba(255, 255, 255, 0.08)"));
    REQUIRE_EQ(package->dark.preeditDivider, std::string("#444444"));
    REQUIRE_EQ(package->dark.menuBackground, std::string("#202020"));
    REQUIRE_EQ(package->dark.menuBorder, std::string("#333333"));
    REQUIRE_EQ(package->dark.menuText, std::string("#eeeeee"));
    REQUIRE_EQ(package->dark.menuHover, std::string("#2a2a2a"));
    REQUIRE(package->light.candidateText.empty());
    REQUIRE(package->light.menuBackground.empty());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(candidate_skin_catalog_leaves_window_options_unset_by_default)
{
    const auto root = WriteSkin(L"defaults", "");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "gloss", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(!package->borderWidthDip.has_value());
    REQUIRE(!package->itemCornerRadiusDip.has_value());
    REQUIRE(package->shadow.empty());
    REQUIRE(package->backdrop.empty());
    REQUIRE(package->fontFamily.empty());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// Out-of-range geometry, unknown shadow levels and font names that could leave the CSS font-family value all
// reject the skin, matching how bad colours are handled.
TEST_CASE(candidate_skin_catalog_rejects_invalid_window_options)
{
    REQUIRE(Rejects(L"border", "border_width_dip = 5\n"));
    REQUIRE(Rejects(L"border-negative", "border_width_dip = -1\n"));
    REQUIRE(Rejects(L"item-radius", "item_corner_radius_dip = 17\n"));
    REQUIRE(Rejects(L"shadow", "shadow = \"huge\"\n"));
    REQUIRE(Rejects(L"backdrop", "backdrop = \"glass\"\n"));
    REQUIRE(Rejects(L"backdrop-opacity", "\n[candidate.dark]\nbackdrop_opacity = 1.5\n"));
    REQUIRE(Rejects(L"backdrop-luminosity", "\n[candidate.dark]\nbackdrop_luminosity = \"high\"\n"));
    REQUIRE(Rejects(L"font", "font_family = \"Foo; color: red\"\n"));
    REQUIRE(Rejects(L"font-quote", "font_family = \"Foo'\"\n"));
    REQUIRE(Rejects(L"menu", "\n[candidate.dark]\nmenu = \"#fff\"\n"));
    REQUIRE(Rejects(L"menu-color", "\n[candidate.dark.menu]\nhover = \"red; }\"\n"));
}
