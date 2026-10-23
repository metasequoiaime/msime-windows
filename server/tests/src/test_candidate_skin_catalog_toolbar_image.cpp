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

// `tables` is appended verbatim after the head, so each case owns its [candidate_window] and [toolbar] tables.
std::filesystem::path WriteSkin(const std::wstring &leaf, const std::string &tables)
{
    namespace fs = std::filesystem;
    const fs::path skins_root =
        test::TempRoot() / (L"msime-skin-toolbar-" + std::to_wstring(GetCurrentProcessId())) / leaf / L"skins";
    std::error_code ec;
    fs::remove_all(skins_root, ec);
    fs::create_directories(skins_root / L"art" / L"assets", ec);
    REQUIRE(!ec);
    for (const wchar_t *image :
         {L"assets\\bar.png", L"assets\\cn.png", L"assets\\en.png", L"assets\\gear.apng", L"assets\\deco.png"})
    {
        std::ofstream file(skins_root / L"art" / image, std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
    }
    std::ofstream manifest(skins_root / L"art" / L"skin.toml", std::ios::binary | std::ios::trunc);
    REQUIRE(manifest.is_open());
    manifest << kManifestHead << tables;
    return skins_root;
}

void RemoveSkin(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// The skin is rejected with a reason the settings page can show.
bool LoadFails(const std::wstring &leaf, const std::string &tables)
{
    const auto root = WriteSkin(leaf, tables);
    std::string error;
    const bool failed = !CandidateSkinCatalog::Load(root, "art", &error).has_value() && !error.empty();
    RemoveSkin(root);
    return failed;
}

bool HasNotice(const CandidateSkinCatalog::Package &package, const std::string &fragment)
{
    return std::any_of(package.notices.begin(), package.notices.end(),
                       [&](const std::string &notice) { return notice.find(fragment) != std::string::npos; });
}

constexpr const char *kImageToolbar = R"([candidate_window]

[toolbar]
mode = "image"
image = "assets/bar.png"
image_width_dip = 160
image_height_dip = 40
)";
} // namespace

TEST_CASE(candidate_skin_catalog_reads_toolbar_image_mode)
{
    const auto root = WriteSkin(L"valid", std::string(kImageToolbar) + R"(
[toolbar.buttons.status]
x_dip = 8
y_dip = 6
width_dip = 30
height_dip = 28
icon_size_dip = 18.5
image = "assets/cn.png"

[toolbar.buttons.status.images]
en = "assets/en.png"

[toolbar.buttons.settings]
x_dip = 120.5
y_dip = 7
image = "assets/gear.apng"

[toolbar.buttons.emoji]
visible = false
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->toolbarMode, std::string("image"));
    REQUIRE_EQ(package->toolbarImage, std::string("assets/bar.png"));
    REQUIRE_EQ(package->toolbarImageWidthDip, 160.0);
    REQUIRE_EQ(package->toolbarImageHeightDip, 40.0);

    const auto &status = package->toolbarButtons.at("status");
    REQUIRE_EQ(*status.xDip, 8.0);
    REQUIRE_EQ(*status.yDip, 6.0);
    REQUIRE_EQ(*status.widthDip, 30.0);
    REQUIRE_EQ(*status.heightDip, 28.0);
    REQUIRE_EQ(*status.iconSizeDip, 18.5);
    REQUIRE_EQ(status.image, std::string("assets/cn.png"));
    REQUIRE_EQ(status.images.at("en"), std::string("assets/en.png"));

    const auto &settings = package->toolbarButtons.at("settings");
    REQUIRE_EQ(*settings.xDip, 120.5);
    REQUIRE(!settings.widthDip.has_value());
    REQUIRE(!settings.iconSizeDip.has_value());
    REQUIRE_EQ(settings.image, std::string("assets/gear.apng"));

    REQUIRE(!*package->toolbarButtons.at("emoji").visible);
    REQUIRE(package->notices.empty());
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_toolbar_defaults_to_bar)
{
    const auto root = WriteSkin(L"bar", "[candidate_window]\n");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->toolbarMode, std::string("bar"));
    REQUIRE(package->toolbarImage.empty());
    REQUIRE(package->toolbarButtons.empty());
    RemoveSkin(root);
}

// The image mode is opened at the declared size, so the size and the image itself are required, and every
// broken key names itself instead of failing with an empty reason.
TEST_CASE(candidate_skin_catalog_rejects_invalid_toolbar_image_mode)
{
    REQUIRE(LoadFails(L"no-size", "[candidate_window]\n[toolbar]\nmode = \"image\"\nimage = \"assets/bar.png\"\n"));
    REQUIRE(LoadFails(L"no-image", "[candidate_window]\n[toolbar]\nmode = \"image\"\nimage_width_dip = 160\n"
                                   "image_height_dip = 40\n"));
    REQUIRE(LoadFails(L"bad-mode", "[candidate_window]\n[toolbar]\nmode = \"floating\"\n"));
    REQUIRE(LoadFails(L"missing-file", "[candidate_window]\n[toolbar]\nmode = \"image\"\nimage = \"assets/none.png\"\n"
                                       "image_width_dip = 160\nimage_height_dip = 40\n"));
    REQUIRE(LoadFails(L"bad-extension", "[candidate_window]\n[toolbar]\nmode = \"image\"\nimage = \"assets/bar.bmp\"\n"
                                        "image_width_dip = 160\nimage_height_dip = 40\n"));
    REQUIRE(LoadFails(L"buttons-array", std::string(kImageToolbar) + "buttons = [\"status\"]\n"));
    REQUIRE(LoadFails(L"button-zero-size",
                      std::string(kImageToolbar) + "[toolbar.buttons.status]\nx_dip = 0\ny_dip = 0\nwidth_dip = 0\n"));
    REQUIRE(LoadFails(L"button-negative", std::string(kImageToolbar) + "[toolbar.buttons.status]\nx_dip = -4\n"));
    REQUIRE(
        LoadFails(L"button-zero-icon", std::string(kImageToolbar) + "[toolbar.buttons.status]\nicon_size_dip = 0\n"));
    // A stateless button has nothing to key the images by.
    REQUIRE(LoadFails(L"stateless-images",
                      std::string(kImageToolbar) + "[toolbar.buttons.settings.images]\ncn = \"assets/cn.png\"\n"));
    REQUIRE(LoadFails(L"missing-button-image",
                      std::string(kImageToolbar) + "[toolbar.buttons.status.images]\ncn = \"assets/none.png\"\n"));
}

// Keys a newer skin may use are noticed and skipped rather than making the whole skin unusable.
TEST_CASE(candidate_skin_catalog_notices_unused_toolbar_keys)
{
    const auto root = WriteSkin(L"notices", std::string(kImageToolbar) + R"(
[toolbar.buttons.voice]
x_dip = 0
y_dip = 0

[toolbar.buttons.status]
x_dip = 150
y_dip = 6

[toolbar.buttons.status.images]
kana = "assets/cn.png"

[toolbar.buttons.punctuation]
x_dip = 40
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(!package->toolbarButtons.count("voice"));
    REQUIRE(package->toolbarButtons.at("status").images.empty());
    REQUIRE(HasNotice(*package, "toolbar.buttons.voice"));
    REQUIRE(HasNotice(*package, "kana"));
    REQUIRE(HasNotice(*package, "toolbar.buttons.punctuation"));
    // A button past the image widens the window instead of being cut off, so it is not noticed.
    REQUIRE(!HasNotice(*package, "背景图"));
    RemoveSkin(root);

    const auto bar = WriteSkin(L"bar-buttons", "[candidate_window]\n[toolbar.buttons.status]\nx_dip = 0\ny_dip = 0\n");
    const auto barPackage = CandidateSkinCatalog::Load(bar, "art", &error);
    REQUIRE(barPackage.has_value());
    REQUIRE_EQ(barPackage->toolbarMode, std::string("bar"));
    REQUIRE(HasNotice(*barPackage, "mode = \"image\""));
    RemoveSkin(bar);
}

TEST_CASE(candidate_skin_catalog_reads_toolbar_button_defaults_and_flow)
{
    const auto root = WriteSkin(L"flow", std::string(kImageToolbar) + R"(
[toolbar.button_defaults]
width_dip = 30
height_dip = 30
icon_size_dip = 20
background = "assets/cn.png"

[toolbar.button_flow]
x_dip = 170
y_dip = 2
direction = "vertical"
gap_dip = 2.5

[toolbar.buttons.settings]
background = "assets/en.png"

[toolbar.buttons.punctuation]
x_dip = 40
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    const auto &defaults = package->toolbarButtonDefaults;
    REQUIRE_EQ(*defaults.widthDip, 30.0);
    REQUIRE_EQ(*defaults.heightDip, 30.0);
    REQUIRE_EQ(*defaults.iconSizeDip, 20.0);
    REQUIRE_EQ(defaults.background, std::string("assets/cn.png"));
    REQUIRE(package->toolbarButtonFlow.has_value());
    REQUIRE_EQ(package->toolbarButtonFlow->xDip, 170.0);
    REQUIRE_EQ(package->toolbarButtonFlow->yDip, 2.0);
    REQUIRE(package->toolbarButtonFlow->vertical);
    REQUIRE_EQ(package->toolbarButtonFlow->gapDip, 2.5);
    REQUIRE_EQ(package->toolbarButtons.at("settings").background, std::string("assets/en.png"));
    // With a flow, a button with only one coordinate is laid out by the flow instead of dropped.
    REQUIRE(!HasNotice(*package, "toolbar.buttons.punctuation"));
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_toolbar_button_defaults_and_flow)
{
    REQUIRE(LoadFails(L"flow-not-table", std::string(kImageToolbar) + "button_flow = 1\n"));
    REQUIRE(
        LoadFails(L"flow-direction", std::string(kImageToolbar) + "[toolbar.button_flow]\ndirection = \"diagonal\"\n"));
    REQUIRE(LoadFails(L"flow-negative", std::string(kImageToolbar) + "[toolbar.button_flow]\nx_dip = -1\n"));
    REQUIRE(LoadFails(L"flow-gap", std::string(kImageToolbar) + "[toolbar.button_flow]\ngap_dip = -2\n"));
    REQUIRE(LoadFails(L"defaults-not-table", std::string(kImageToolbar) + "button_defaults = \"round\"\n"));
    REQUIRE(
        LoadFails(L"defaults-zero-size", std::string(kImageToolbar) + "[toolbar.button_defaults]\nwidth_dip = 0\n"));
    REQUIRE(LoadFails(L"defaults-missing-background",
                      std::string(kImageToolbar) + "[toolbar.button_defaults]\nbackground = \"assets/none.png\"\n"));
    REQUIRE(LoadFails(L"button-missing-background",
                      std::string(kImageToolbar) + "[toolbar.buttons.emoji]\nbackground = \"assets/none.png\"\n"));
}

TEST_CASE(candidate_skin_catalog_reads_decoration_candidate_overlay)
{
    const auto root = WriteSkin(L"overlay", R"([candidate_window.decoration]
image = "assets/deco.png"
top_inset_dip = 88
width_dip = 136
candidate_overlay = true
candidate_overlay_font_size_dip = 18
candidate_overlay_min_font_size_dip = 6
candidate_overlay_max_width_dip = 120
candidate_overlay_color = "#de4045"
candidate_overlay_align_h = "right"
candidate_overlay_align_v = "bottom"
candidate_overlay_offset_x_dip = -3
candidate_overlay_offset_y_dip = 5
candidate_overlay_strip_badge = false
candidate_overlay_index = 0
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->decorationCandidate.has_value());
    const auto &overlay = *package->decorationCandidate;
    REQUIRE_EQ(overlay.fontSizeDip, 18.0);
    REQUIRE_EQ(overlay.minFontSizeDip, 6.0);
    REQUIRE_EQ(overlay.maxWidthDip, 120.0);
    REQUIRE_EQ(overlay.color, std::string("#de4045"));
    REQUIRE_EQ(overlay.alignH, std::string("right"));
    REQUIRE_EQ(overlay.alignV, std::string("bottom"));
    REQUIRE_EQ(overlay.offsetXDip, -3.0);
    REQUIRE_EQ(overlay.offsetYDip, 5.0);
    REQUIRE(!overlay.stripBadge);
    REQUIRE_EQ(overlay.index, 0);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_decoration_candidate_overlay_is_opt_in)
{
    // The candidate_overlay_* keys mean nothing until candidate_overlay = true.
    const auto root = WriteSkin(L"overlay-off", R"([candidate_window.decoration]
image = "assets/deco.png"
top_inset_dip = 88
width_dip = 136
candidate_overlay_font_size_dip = 18
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(package.has_value());
    REQUIRE(!package->decorationCandidate.has_value());
    RemoveSkin(root);

    const auto defaults = WriteSkin(L"overlay-defaults", R"([candidate_window.decoration]
image = "assets/deco.png"
top_inset_dip = 88
width_dip = 136
candidate_overlay = true
)");
    const auto defaultsPackage = CandidateSkinCatalog::Load(defaults, "art", &error);
    REQUIRE(defaultsPackage.has_value());
    const auto &overlay = *defaultsPackage->decorationCandidate;
    REQUIRE_EQ(overlay.index, 1);
    REQUIRE_EQ(overlay.fontSizeDip, 16.0);
    REQUIRE_EQ(overlay.maxWidthDip, 0.0);
    REQUIRE_EQ(overlay.alignH, std::string("center"));
    REQUIRE(overlay.stripBadge);
    RemoveSkin(defaults);
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_decoration_candidate_overlay)
{
    const std::string head = "[candidate_window.decoration]\nimage = \"assets/deco.png\"\ntop_inset_dip = 88\n"
                             "width_dip = 136\ncandidate_overlay = true\n";
    REQUIRE(LoadFails(L"overlay-flag", "[candidate_window.decoration]\nimage = \"assets/deco.png\"\n"
                                       "top_inset_dip = 88\nwidth_dip = 136\ncandidate_overlay = \"yes\"\n"));
    // DirectWrite has no text format for size 0.
    REQUIRE(LoadFails(L"overlay-zero-font", head + "candidate_overlay_font_size_dip = 0\n"));
    REQUIRE(LoadFails(L"overlay-zero-min-font", head + "candidate_overlay_min_font_size_dip = 0\n"));
    REQUIRE(LoadFails(L"overlay-align", head + "candidate_overlay_align_h = \"middle\"\n"));
    REQUIRE(LoadFails(L"overlay-index", head + "candidate_overlay_index = 1.5\n"));
    REQUIRE(LoadFails(L"overlay-offset", head + "candidate_overlay_offset_x_dip = 3000\n"));
}
