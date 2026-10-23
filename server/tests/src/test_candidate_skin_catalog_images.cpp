#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "skin/candidate_skin_catalog.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

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

// `candidateWindow` is appended verbatim after the head, so each case owns the [candidate_window] tables.
std::filesystem::path WriteSkin(const std::wstring &leaf, const std::string &candidateWindow)
{
    namespace fs = std::filesystem;
    const fs::path skins_root =
        test::TempRoot() / (L"msime-skin-images-" + std::to_wstring(GetCurrentProcessId())) / leaf / L"skins";
    std::error_code ec;
    fs::remove_all(skins_root, ec);
    fs::create_directories(skins_root / L"art" / L"assets", ec);
    REQUIRE(!ec);
    for (const wchar_t *image : {L"assets\\character.png", L"assets\\paper.png"})
    {
        std::ofstream file(skins_root / L"art" / image, std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
    }
    std::ofstream manifest(skins_root / L"art" / L"skin.toml", std::ios::binary | std::ios::trunc);
    REQUIRE(manifest.is_open());
    manifest << kManifestHead << candidateWindow;
    return skins_root;
}

void RemoveSkin(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

bool LoadFails(const std::wstring &leaf, const std::string &candidateWindow)
{
    const auto root = WriteSkin(leaf, candidateWindow);
    std::string error;
    const bool failed = !CandidateSkinCatalog::Load(root, "art", &error).has_value() && !error.empty();
    RemoveSkin(root);
    return failed;
}
} // namespace

// Every image key is optional: a skin without them has no decoration, no background image and keeps the
// base theme's corner radius.
TEST_CASE(candidate_skin_catalog_image_tables_are_optional)
{
    const auto root = WriteSkin(L"plain", "[candidate_window]\n");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->decorationImage.empty());
    REQUIRE_EQ(package->decorationTopDip, 0.0);
    REQUIRE_EQ(package->decorationWidthDip, 0.0);
    REQUIRE(!package->cornerRadiusDip.has_value());
    REQUIRE(package->backgroundImage.empty());
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_reads_background_corner_and_decoration_placement)
{
    const auto root = WriteSkin(L"valid", R"(
[candidate_window]
corner_radius_dip = 0

[candidate_window.decoration]
image = "assets/character.png"
top_inset_dip = 88
width_dip = 136
align = "left"

[candidate_window.background]
image = "assets/paper.png"
fit = "contain"
opacity = 0.5
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->decorationImage, std::string("assets/character.png"));
    REQUIRE_EQ(package->decorationTopDip, 88.0);
    REQUIRE_EQ(package->decorationWidthDip, 136.0);
    REQUIRE_EQ(package->decorationAlign, std::string("left"));
    // An explicit 0 means square corners, which is different from "not set".
    REQUIRE(package->cornerRadiusDip.has_value());
    REQUIRE_EQ(*package->cornerRadiusDip, 0.0);
    REQUIRE_EQ(package->backgroundImage, std::string("assets/paper.png"));
    REQUIRE_EQ(package->backgroundFit, std::string("contain"));
    REQUIRE_EQ(package->backgroundOpacity, 0.5);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_rejects_incomplete_or_invalid_image_tables)
{
    const std::string window = "[candidate_window]\n";
    const std::string decoration = "[candidate_window.decoration]\n";
    const std::string sized = "top_inset_dip = 88\nwidth_dip = 136\n";
    const std::string character = "image = \"assets/character.png\"\n";
    const std::string background = "[candidate_window.background]\n";
    const std::string paper = "image = \"assets/paper.png\"\n";

    // A decoration needs its image and both sizes.
    REQUIRE(LoadFails(L"deco-empty", window + decoration));
    REQUIRE(LoadFails(L"deco-no-image", window + decoration + sized));
    REQUIRE(LoadFails(L"deco-no-top", window + decoration + character + "width_dip = 136\n"));
    REQUIRE(LoadFails(L"deco-no-width", window + decoration + character + "top_inset_dip = 88\n"));
    REQUIRE(LoadFails(L"deco-missing", window + decoration + sized + "image = \"assets/missing.png\"\n"));
    REQUIRE(LoadFails(L"deco-escape", window + decoration + sized + "image = \"../x.png\"\n"));
    REQUIRE(LoadFails(L"deco-align", window + decoration + sized + character + "align = \"top\"\n"));

    REQUIRE(LoadFails(L"radius", "[candidate_window]\ncorner_radius_dip = 40\n"));
    REQUIRE(LoadFails(L"radius-type", "[candidate_window]\ncorner_radius_dip = \"8px\"\n"));

    REQUIRE(LoadFails(L"bg-no-image", window + background + "fit = \"cover\"\n"));
    REQUIRE(LoadFails(L"bg-fit", window + background + paper + "fit = \"tile\"\n"));
    REQUIRE(LoadFails(L"bg-opacity", window + background + paper + "opacity = 2\n"));
    REQUIRE(LoadFails(L"bg-missing", window + background + "image = \"assets/missing.png\"\n"));
}

TEST_CASE(candidate_skin_catalog_reads_toolbar_colors_and_corner_radius)
{
    // The rgba() values end in `)"`, so the raw string needs its own delimiter.
    const auto root = WriteSkin(L"toolbar", R"toml([candidate_window]

[toolbar]
corner_radius_dip = 12

[toolbar.dark]
border = "rgba(224, 138, 168, 0.38)"
handle = "#e08aa8"

[toolbar.light]
background = "#fff7fa"
divider = "rgba(176, 80, 110, 0.22)"
icon = "#3a2a30"
hover = "rgba(196, 92, 122, 0.10)"
)toml");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->toolbarCornerRadiusDip.has_value());
    REQUIRE_EQ(*package->toolbarCornerRadiusDip, 12.0);
    REQUIRE_EQ(package->toolbarDark.border, std::string("rgba(224, 138, 168, 0.38)"));
    REQUIRE_EQ(package->toolbarDark.handle, std::string("#e08aa8"));
    REQUIRE(package->toolbarDark.background.empty());
    REQUIRE_EQ(package->toolbarLight.background, std::string("#fff7fa"));
    REQUIRE_EQ(package->toolbarLight.divider, std::string("rgba(176, 80, 110, 0.22)"));
    REQUIRE_EQ(package->toolbarLight.icon, std::string("#3a2a30"));
    REQUIRE_EQ(package->toolbarLight.hover, std::string("rgba(196, 92, 122, 0.10)"));
    REQUIRE(package->toolbarLight.handle.empty());
    RemoveSkin(root);

    // Without a [toolbar] table the toolbar keeps the base skin entirely.
    const auto plainRoot = WriteSkin(L"toolbar-absent", "[candidate_window]\n");
    const auto plain = CandidateSkinCatalog::Load(plainRoot, "art", &error);
    REQUIRE(plain.has_value());
    REQUIRE(!plain->toolbarCornerRadiusDip.has_value());
    REQUIRE(plain->toolbarDark.handle.empty());
    REQUIRE(plain->toolbarLight.background.empty());
    RemoveSkin(plainRoot);
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_toolbar_tables)
{
    const std::string window = "[candidate_window]\n";
    REQUIRE(LoadFails(L"tb-not-table", window + "[[toolbar]]\n"));
    REQUIRE(LoadFails(L"tb-theme-not-table", window + "[toolbar]\ndark = \"pink\"\n"));
    REQUIRE(LoadFails(L"tb-color-type", window + "[toolbar.dark]\nhandle = 7\n"));
    // Toolbar colours use the same CSS colour rules, so anything that could end a declaration is refused.
    REQUIRE(LoadFails(L"tb-color-inject", window + "[toolbar.light]\nicon = \"red; } body { display: none\"\n"));
    REQUIRE(LoadFails(L"tb-radius", window + "[toolbar]\ncorner_radius_dip = 40\n"));
    REQUIRE(LoadFails(L"tb-radius-type", window + "[toolbar]\ncorner_radius_dip = \"8px\"\n"));
}

TEST_CASE(candidate_skin_catalog_reads_v2_features)
{
    namespace fs = std::filesystem;
    const fs::path skins_root =
        test::TempRoot() / (L"msime-skin-v2-" + std::to_wstring(GetCurrentProcessId())) / L"skins";
    std::error_code ec;
    fs::remove_all(skins_root, ec);
    fs::create_directories(skins_root / L"art" / L"assets", ec);
    REQUIRE(!ec);
    for (const wchar_t *image : {L"assets\\border.png", L"assets\\char.png"})
    {
        std::ofstream file(skins_root / L"art" / image, std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
    }
    std::ofstream manifest(skins_root / L"art" / L"skin.toml", std::ios::binary | std::ios::trunc);
    REQUIRE(manifest.is_open());
    manifest << R"toml(schema_version = 2
id = "art"
name = "Art"
version = "2.0.0"
base = "canvas"

[supports]
layouts = ["horizontal"]
themes = ["dark"]

[candidate_window]
line_gap_dip = 6

[candidate_window.border_image]
source = "assets/border.png"
slice_dip = [16, 44, 16, 0]
stretch_dip = [16, 90, 16, 0]
width_dip = [16, 44, 16, 0]
fill = true

[candidate_window.content_insets_dip]
top = 4
right = 130
bottom = 14
left = 55

[candidate_window.outer_insets_dip]
top = 87
right = 16
bottom = 4
left = 0

[[candidate_window.decorations]]
image = "assets/char.png"
anchor = "right"
offset_x_dip = 10
offset_y_dip = -15
width_dip = 149
height_dip = 161
z_index = 3
)toml";
    manifest.close();

    std::string error;
    const auto package = CandidateSkinCatalog::Load(skins_root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->schemaVersion, 2);
    REQUIRE(package->borderImage.has_value());
    REQUIRE_EQ(package->borderImage->source, std::string("assets/border.png"));
    REQUIRE_EQ(package->borderImage->sliceDip.size(), static_cast<size_t>(4));
    REQUIRE_EQ(package->borderImage->sliceDip[1], 44.0);
    // stretch_dip 与 slice_dip 分开存：前者才是可拉伸区，渲染时优先用它。
    REQUIRE_EQ(package->borderImage->stretchDip.size(), static_cast<size_t>(4));
    REQUIRE_EQ(package->borderImage->stretchDip[1], 90.0);
    REQUIRE(package->borderImage->fill);

    REQUIRE(package->contentInsetsDip.has_value());
    REQUIRE_EQ(package->contentInsetsDip->left, 55.0);
    REQUIRE_EQ(package->contentInsetsDip->right, 130.0);

    REQUIRE(package->lineGapDip.has_value());
    REQUIRE_EQ(*package->lineGapDip, 6.0);

    REQUIRE(package->outerInsetsDip.has_value());
    REQUIRE_EQ(package->outerInsetsDip->top, 87.0);
    REQUIRE_EQ(package->outerInsetsDip->right, 16.0);

    REQUIRE_EQ(package->freeDecorations.size(), static_cast<size_t>(1));
    REQUIRE_EQ(package->freeDecorations[0].anchor, std::string("right"));
    REQUIRE_EQ(package->freeDecorations[0].widthDip, 149.0);
    REQUIRE_EQ(package->freeDecorations[0].zIndex, 3);

    fs::remove_all(skins_root, ec);
}

TEST_CASE(candidate_skin_catalog_defaults_optional_decoration_keys)
{
    namespace fs = std::filesystem;
    const fs::path skins_root =
        test::TempRoot() / (L"msime-skin-v2-decor-" + std::to_wstring(GetCurrentProcessId())) / L"skins";
    std::error_code ec;
    fs::remove_all(skins_root, ec);
    fs::create_directories(skins_root / L"art" / L"assets", ec);
    REQUIRE(!ec);
    {
        std::ofstream file(skins_root / L"art" / L"assets" / L"char.png", std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
    }
    std::ofstream manifest(skins_root / L"art" / L"skin.toml", std::ios::binary | std::ios::trunc);
    REQUIRE(manifest.is_open());
    // offset_x_dip、offset_y_dip、z_index 都可省略；缺省时不能解引用空节点。
    manifest << R"toml(schema_version = 2
id = "art"
name = "Art"
version = "2.0.0"
base = "canvas"

[supports]
layouts = ["horizontal"]
themes = ["dark"]

[[candidate_window.decorations]]
image = "assets/char.png"
width_dip = 40
height_dip = 30
)toml";
    manifest.close();

    std::string error;
    const auto package = CandidateSkinCatalog::Load(skins_root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->freeDecorations.size(), static_cast<size_t>(1));
    REQUIRE_EQ(package->freeDecorations[0].offsetXDip, 0.0);
    REQUIRE_EQ(package->freeDecorations[0].offsetYDip, 0.0);
    REQUIRE_EQ(package->freeDecorations[0].zIndex, 2);

    fs::remove_all(skins_root, ec);
}

namespace
{
// A schema 2 skin with the given base and [candidate_window] body, plus the two images it may reference.
std::filesystem::path WriteV2Skin(const std::wstring &leaf, const std::string &base, const std::string &window,
                                  int schema = 2)
{
    namespace fs = std::filesystem;
    const fs::path skins_root =
        test::TempRoot() / (L"msime-skin-canvas-" + std::to_wstring(GetCurrentProcessId())) / leaf / L"skins";
    std::error_code ec;
    fs::remove_all(skins_root, ec);
    fs::create_directories(skins_root / L"art" / L"assets", ec);
    REQUIRE(!ec);
    for (const wchar_t *image : {L"assets\\panel.png", L"assets\\mascot.apng", L"assets\\spark.GIF",
                                 L"assets\\leaf.webp", L"assets\\photo.jpg"})
    {
        std::ofstream file(skins_root / L"art" / image, std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
    }
    std::ofstream manifest(skins_root / L"art" / L"skin.toml", std::ios::binary | std::ios::trunc);
    REQUIRE(manifest.is_open());
    manifest << "schema_version = " << schema << "\nid = \"art\"\nname = \"Art\"\nversion = \"2.0.0\"\nbase = \""
             << base << "\"\n\n[supports]\nlayouts = [\"horizontal\", \"vertical\"]\nthemes = [\"dark\", \"light\"]\n\n"
             << "[candidate_window]\n"
             << window;
    return skins_root;
}

constexpr const char *kPanel = R"toml(
[candidate_window.border_image]
source = "assets/panel.png"
slice_dip = [12]
)toml";

bool V2LoadFails(const std::wstring &leaf, const std::string &window)
{
    const auto root = WriteV2Skin(leaf, "canvas", window);
    std::string error;
    const bool failed = !CandidateSkinCatalog::Load(root, "art", &error).has_value() && !error.empty();
    RemoveSkin(root);
    return failed;
}
} // namespace

TEST_CASE(candidate_skin_catalog_canvas_is_a_base_only)
{
    REQUIRE(CandidateSkinCatalog::IsCanvasBase("canvas"));
    REQUIRE(!CandidateSkinCatalog::IsCanvasBase("fluent"));
    // It cannot be picked on its own: it is not one of the built-in skins.
    REQUIRE(!CandidateSkinCatalog::IsBuiltIn("canvas"));
    const auto &ids = CandidateSkinCatalog::BuiltInIds();
    REQUIRE(std::find(ids.begin(), ids.end(), std::string("canvas")) == ids.end());

    const auto root = WriteV2Skin(L"canvas-ok", "canvas", kPanel);
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->base, std::string("canvas"));
    REQUIRE_EQ(package->schemaVersion, 2);
    REQUIRE(package->notices.empty());
    REQUIRE(package->borderImage.has_value());
    REQUIRE_EQ(package->borderImage->sliceDip, std::vector<double>({12.0}));
    REQUIRE(package->borderImage->widthDip.empty());
    REQUIRE(package->borderImage->fill);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_v1_cannot_use_the_canvas_base)
{
    const auto root = WriteV2Skin(L"canvas-v1", "canvas", "", 1);
    std::string error;
    REQUIRE(!CandidateSkinCatalog::Load(root, "art", &error).has_value());
    REQUIRE(!error.empty());
    const auto catalog = CandidateSkinCatalog::Scan(root);
    REQUIRE(catalog.packages.empty());
    REQUIRE_EQ(catalog.issues.size(), size_t{1});
    REQUIRE(catalog.issues.front().kind == CandidateSkinCatalog::Issue::Kind::LoadFailed);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_v2_without_canvas_loads_as_v1_and_reports_an_issue)
{
    // Even invalid v2 tables do not matter: they are not read at all.
    const std::string window = R"toml(line_gap_dip = 6

[candidate_window.border_image]
source = "assets/mascot.apng"
slice_dip = [12]

[candidate_window.outer_insets_dip]
top = 20

[[candidate_window.decorations]]
image = "assets/mascot.apng"
width_dip = 40
height_dip = 40
z_index = "high"
)toml";
    for (const char *base : {"fluent", "wechat", "paper"})
    {
        const auto root = WriteV2Skin(L"v2-fluent", base, window);
        std::string error;
        const auto package = CandidateSkinCatalog::Load(root, "art", &error);
        REQUIRE(error.empty());
        REQUIRE(package.has_value());
        REQUIRE_EQ(package->schemaVersion, 1);
        REQUIRE_EQ(package->notices.size(), size_t{1});
        // Every canvas-only key the manifest writes is named, in the loader's key order; line_gap_dip is one of the
        // window's own keys and a v1 skin would not read it either, so it is reported too.
        REQUIRE_EQ(package->notices.front(),
                   std::string("candidate_window.border_image、candidate_window.outer_insets_dip、"
                               "candidate_window.line_gap_dip、candidate_window.decorations 只在 base = \"canvas\" "
                               "时生效，已忽略"));
        REQUIRE(!package->borderImage.has_value());
        REQUIRE(!package->lineGapDip.has_value());
        REQUIRE(!package->outerInsetsDip.has_value());
        REQUIRE(package->freeDecorations.empty());

        const auto catalog = CandidateSkinCatalog::Scan(root);
        REQUIRE_EQ(catalog.packages.size(), size_t{1});
        REQUIRE_EQ(catalog.issues.size(), size_t{1});
        REQUIRE_EQ(catalog.issues.front().folder, std::string("art"));
        REQUIRE_EQ(catalog.issues.front().reason, package->notices.front());
        // The skin still loads: the settings page must count it under "notices", not as an invalid folder.
        REQUIRE(catalog.issues.front().kind == CandidateSkinCatalog::Issue::Kind::Noticed);
        RemoveSkin(root);
    }
}

TEST_CASE(candidate_skin_catalog_v2_motion_without_canvas_loads_static_without_an_issue)
{
    // [animation] and decoration motion are valid on any base in the shared schema; Windows draws such a skin still.
    const std::string window = R"toml(min_width_dip = 176

[candidate_window.decoration]
image = "assets/photo.jpg"
top_inset_dip = 40
width_dip = 40
align = "right"
motion = "float"
motion_period_ms = 3000

[animation]
enter = "slide_up"
exit = "fade"
selection = "spring"
duration_ms = 200
stagger_ms = 14
)toml";
    for (const char *base : {"fluent", "system", "paper"})
    {
        const auto root = WriteV2Skin(L"v2-motion", base, window);
        std::string error;
        const auto package = CandidateSkinCatalog::Load(root, "art", &error);
        REQUIRE(error.empty());
        REQUIRE(package.has_value());
        REQUIRE_EQ(package->schemaVersion, 1);
        REQUIRE(package->notices.empty());
        REQUIRE_EQ(package->decorationImage, std::string("assets/photo.jpg"));

        const auto catalog = CandidateSkinCatalog::Scan(root);
        REQUIRE_EQ(catalog.packages.size(), size_t{1});
        REQUIRE(catalog.issues.empty());
        RemoveSkin(root);
    }
}

TEST_CASE(candidate_skin_catalog_v2_accepts_animated_image_extensions_only)
{
    const auto root = WriteV2Skin(L"v2-ext", "canvas", R"toml(
[candidate_window.border_image]
source = "assets/mascot.apng"
slice_dip = [4, 8]
width_dip = [2, 4, 2, 4]
fill = false

[[candidate_window.decorations]]
image = "assets/spark.GIF"
width_dip = 10
height_dip = 10

[[candidate_window.decorations]]
image = "assets/leaf.webp"
anchor = "bottom-left"
offset_x_dip = -12.5
offset_y_dip = 3
width_dip = 10
height_dip = 10
z_index = -4
)toml");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(!package->borderImage->fill);
    REQUIRE_EQ(package->borderImage->widthDip.size(), size_t{4});
    REQUIRE_EQ(package->freeDecorations.size(), size_t{2});
    REQUIRE_EQ(package->freeDecorations[1].offsetXDip, -12.5);
    REQUIRE_EQ(package->freeDecorations[1].zIndex, -4);
    RemoveSkin(root);

    REQUIRE(CandidateSkinCatalog::IsAnimatableImagePath("a/b.PNG"));
    REQUIRE(CandidateSkinCatalog::IsAnimatableImagePath("b.apng"));
    REQUIRE(!CandidateSkinCatalog::IsAnimatableImagePath("b.jpg"));
    REQUIRE(!CandidateSkinCatalog::IsAnimatableImagePath("png"));
    REQUIRE(!CandidateSkinCatalog::IsAnimatableImagePath("a.png/b"));
}

TEST_CASE(candidate_skin_catalog_border_image_light_source_swaps_the_panel_for_light)
{
    const auto root = WriteV2Skin(L"v2-light", "canvas", R"toml(
[candidate_window.border_image]
source = "assets/panel.png"
light_source = "assets/mascot.apng"
slice_dip = [12]
)toml");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->borderImage->lightSource, std::string("assets/mascot.apng"));
    REQUIRE_EQ(CandidateSkinCatalog::BorderImageSource(*package->borderImage, false), std::string("assets/panel.png"));
    REQUIRE_EQ(CandidateSkinCatalog::BorderImageSource(*package->borderImage, true), std::string("assets/mascot.apng"));
    RemoveSkin(root);

    // Without light_source both themes draw source.
    CandidateSkinCatalog::BorderImage shared;
    shared.source = "assets/panel.png";
    REQUIRE_EQ(CandidateSkinCatalog::BorderImageSource(shared, true), std::string("assets/panel.png"));

    const std::string head = "[candidate_window.border_image]\nsource = \"assets/panel.png\"\nslice_dip = [4]\n";
    REQUIRE(V2LoadFails(L"bi-light-jpg", head + "light_source = \"assets/photo.jpg\"\n"));
    REQUIRE(V2LoadFails(L"bi-light-missing", head + "light_source = \"assets/missing.png\"\n"));
    REQUIRE(V2LoadFails(L"bi-light-escape", head + "light_source = \"../panel.png\"\n"));
}

TEST_CASE(candidate_skin_catalog_toolbar_handle_follows_the_accent)
{
    const auto root = WriteV2Skin(L"handle-accent", "canvas", std::string(kPanel) + R"toml(
[candidate.dark]
accent = "#5eead4"

[candidate.light]
accent = "#16a37f"

[toolbar.light]
handle = "#123456"
)toml");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    // Left out: the theme's accent. Written: kept as is.
    REQUIRE_EQ(package->toolbarDark.handle, std::string("#5eead4"));
    REQUIRE_EQ(package->toolbarLight.handle, std::string("#123456"));
    RemoveSkin(root);

    REQUIRE(CandidateSkinCatalog::IsOfficialPackage("green_leaf"));
    REQUIRE(!CandidateSkinCatalog::IsOfficialPackage("fluent"));
    // 微信绿(卷轴) 是内置皮肤，有自己的出厂清单，不是官方皮肤包。
    REQUIRE(CandidateSkinCatalog::IsBuiltIn("wechat_scroll"));
    REQUIRE(!CandidateSkinCatalog::IsOfficialPackage("wechat_scroll"));
    REQUIRE(!CandidateSkinCatalog::IsBuiltIn("green_leaf"));
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_v2_values)
{
    const std::string panel = kPanel;
    const std::string decoration = "\n[[candidate_window.decorations]]\nimage = \"assets/mascot.apng\"\n"
                                   "width_dip = 40\nheight_dip = 40\n";
    REQUIRE(
        V2LoadFails(L"bi-jpg", "[candidate_window.border_image]\nsource = \"assets/photo.jpg\"\nslice_dip = [4]\n"));
    REQUIRE(
        V2LoadFails(L"bi-missing", "[candidate_window.border_image]\nsource = \"assets/none.png\"\nslice_dip = [4]\n"));
    REQUIRE(V2LoadFails(L"bi-no-slice", "[candidate_window.border_image]\nsource = \"assets/panel.png\"\n"));
    REQUIRE(V2LoadFails(L"bi-slice-3", panel + "stretch_dip = [1, 2, 3]\n"));
    REQUIRE(V2LoadFails(L"bi-slice-neg", "[candidate_window.border_image]\nsource = \"assets/panel.png\"\n"
                                         "slice_dip = [-1]\n"));
    REQUIRE(V2LoadFails(L"bi-slice-big", panel + "width_dip = [501]\n"));
    REQUIRE(V2LoadFails(L"bi-width-scalar", panel + "width_dip = 4\n"));
    REQUIRE(V2LoadFails(L"bi-fill", panel + "fill = \"no\"\n"));
    REQUIRE(V2LoadFails(L"insets", "line_gap_dip = 4\n[candidate_window.content_insets_dip]\nleft = -2\n"));
    REQUIRE(V2LoadFails(L"gap", "line_gap_dip = 101\n"));
    REQUIRE(V2LoadFails(L"outer", "[candidate_window.outer_insets_dip]\ntop = 501\n"));
    REQUIRE(V2LoadFails(L"dec-ext", "\n[[candidate_window.decorations]]\nimage = \"assets/photo.jpg\"\n"
                                    "width_dip = 4\nheight_dip = 4\n"));
    REQUIRE(V2LoadFails(L"dec-anchor", decoration + "anchor = \"middle\"\n"));
    REQUIRE(V2LoadFails(L"dec-size", "\n[[candidate_window.decorations]]\nimage = \"assets/mascot.apng\"\n"
                                     "width_dip = 0\nheight_dip = 40\n"));
    REQUIRE(V2LoadFails(L"dec-offset", decoration + "offset_x_dip = 2001\n"));
    REQUIRE(V2LoadFails(L"dec-offset-type", decoration + "offset_y_dip = \"4px\"\n"));
    REQUIRE(V2LoadFails(L"dec-z-float", decoration + "z_index = 1.5\n"));
    REQUIRE(V2LoadFails(L"dec-z-range", decoration + "z_index = 1001\n"));
    std::string many;
    for (size_t i = 0; i <= CandidateSkinCatalog::kMaxFreeDecorations; ++i)
    {
        many += decoration;
    }
    REQUIRE(V2LoadFails(L"dec-many", many));
}