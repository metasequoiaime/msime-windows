#include "tests/includes/test_framework.h"

#include "skin/candidate_skin_catalog.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
struct SkinFixture
{
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("msime-shared-bases-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    ~SkinFixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    void Write(const std::string &id, const std::string &base, const std::string &tables = "", int schema = 1,
               const std::string &themes = "['dark', 'light']") const
    {
        const auto directory = root / id;
        std::filesystem::create_directories(directory);
        std::ofstream manifest(directory / "skin.toml");
        REQUIRE(manifest.is_open());
        manifest << "schema_version = " << schema << "\nid = '" << id << "'\nname = 'Sample'\nversion = '1.0'\nbase = '"
                 << base << "'\n[supports]\nlayouts = ['horizontal', 'vertical']\nthemes = " << themes
                 << "\n[candidate_window]\n"
                 << tables;
        REQUIRE(manifest.good());
    }
};
} // namespace

TEST_CASE(candidate_skin_shared_system_uses_native_palette_and_preserves_overrides)
{
    SkinFixture fixture;
    fixture.Write("sample", "system", "[candidate.dark]\ntext = '#123456'\n[toolbar.light]\nicon = '#654321'\n");
    const auto package = CandidateSkinCatalog::Load(fixture.root, "sample");
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->base, std::string("fluent"));
    REQUIRE_EQ(package->dark.text, std::string("#123456"));
    REQUIRE_EQ(package->toolbarLight.icon, std::string("#654321"));
    REQUIRE(package->dark.surface.empty());
    REQUIRE(package->light.surface.empty());
    REQUIRE(package->toolbarDark.background.empty());
    REQUIRE(!package->cornerRadiusDip.has_value());
    for (const char *layout : {"horizontal", "vertical"})
    {
        for (const char *theme : {"dark", "light"})
        {
            REQUIRE(CandidateSkinCatalog::LoadSupported(fixture.root, "sample", layout, theme).has_value());
        }
    }
    REQUIRE(!CandidateSkinCatalog::LoadSupported(fixture.root, "sample", "unknown", "dark").has_value());
}

TEST_CASE(candidate_skin_shared_fixed_bases_inherit_candidate_menu_and_toolbar_colors)
{
    struct Expected
    {
        const char *id;
        const char *theme;
        const char *surface;
        const char *text;
        const char *accent;
        const char *number;
    };
    constexpr Expected expected[] = {
        {"shuishan", "dark", "#2A2B27", "#FFFFFF", "#7FE08E", "#9FB5A3"},
        {"light", "light", "#FFFFFF", "#1A1A1A", "#005FB8", "#6A6F76"},
        {"paper", "light", "#F7F5F0", "#1A1E1B", "#2C7A4B", "#6E6A5E"},
        {"night", "dark", "#16262F", "#E6F1F4", "#4FD1C5", "#86A6B0"},
        {"ink", "dark", "#1A1A1A", "#9A9A9A", "#FFFFFF", "#9A9A9A"},
    };
    SkinFixture fixture;
    for (int schema : {1, 2})
    {
        for (const auto &entry : expected)
        {
            fixture.Write("sample", entry.id, "", schema);
            const auto package = CandidateSkinCatalog::Load(fixture.root, "sample");
            REQUIRE(package.has_value());
            REQUIRE_EQ(package->base, std::string("fluent"));
            REQUIRE_EQ(package->schemaVersion, schema);
            REQUIRE_EQ(package->themes, std::vector<std::string>({entry.theme}));
            const bool light = std::string(entry.theme) == "light";
            const auto &colors = light ? package->light : package->dark;
            const auto &toolbar = light ? package->toolbarLight : package->toolbarDark;
            REQUIRE_EQ(colors.surface, std::string(entry.surface));
            REQUIRE_EQ(colors.border, std::string("#0000001F"));
            REQUIRE_EQ(colors.text, std::string(entry.text));
            REQUIRE_EQ(colors.accent, std::string(entry.accent));
            REQUIRE_EQ(colors.number, std::string(entry.number));
            REQUIRE_EQ(colors.translation, std::string(entry.number));
            REQUIRE_EQ(colors.selected, std::string(entry.accent) + "24");
            REQUIRE_EQ(colors.hover, std::string(entry.text) + "0F");
            REQUIRE_EQ(colors.selectedText, std::string(entry.accent));
            REQUIRE_EQ(colors.selectedNumber, std::string(entry.number));
            REQUIRE_EQ(colors.menuBackground, colors.surface);
            REQUIRE_EQ(colors.menuBorder, colors.border);
            REQUIRE_EQ(colors.menuText, colors.text);
            REQUIRE_EQ(colors.menuHover, colors.hover);
            REQUIRE_EQ(toolbar.background, colors.surface);
            REQUIRE_EQ(toolbar.border, colors.border);
            REQUIRE_EQ(toolbar.divider, colors.border);
            REQUIRE_EQ(toolbar.handle, colors.accent);
            REQUIRE_EQ(toolbar.icon, colors.text);
            REQUIRE_EQ(toolbar.hover, colors.hover);
            for (const char *layout : {"horizontal", "vertical"})
            {
                REQUIRE(CandidateSkinCatalog::LoadSupported(fixture.root, "sample", layout, entry.theme).has_value());
                REQUIRE(!CandidateSkinCatalog::LoadSupported(fixture.root, "sample", layout, light ? "dark" : "light")
                             .has_value());
            }
        }
    }
}

TEST_CASE(candidate_skin_shared_base_keeps_explicit_colors_and_derives_auxiliary_defaults)
{
    SkinFixture fixture;
    fixture.Write("sample", "paper", R"toml(
[candidate.light]
surface = "#123456"
text = "#234567"
number = "#345678"
accent = "#456789"
selected = "#56789a"
hover = "#6789ab"
selected_text = "#789abc"
selected_number = "#89abcd"
translation = "#9abcde"
show_selected_bar = false
[candidate.light.menu]
text = "#abcdef"
[toolbar.light]
background = "#fedcba"
icon = "#edcba9"
)toml");
    const auto package = CandidateSkinCatalog::Load(fixture.root, "sample");
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->light.surface, std::string("#123456"));
    REQUIRE_EQ(package->light.text, std::string("#234567"));
    REQUIRE_EQ(package->light.number, std::string("#345678"));
    REQUIRE_EQ(package->light.accent, std::string("#456789"));
    REQUIRE_EQ(package->light.selected, std::string("#56789a"));
    REQUIRE_EQ(package->light.hover, std::string("#6789ab"));
    REQUIRE_EQ(package->light.selectedText, std::string("#789abc"));
    REQUIRE_EQ(package->light.selectedNumber, std::string("#89abcd"));
    REQUIRE_EQ(package->light.translation, std::string("#9abcde"));
    REQUIRE_EQ(package->light.showSelectedBar, std::optional<bool>(false));
    REQUIRE_EQ(package->light.menuBackground, package->light.surface);
    REQUIRE_EQ(package->light.menuText, std::string("#abcdef"));
    REQUIRE_EQ(package->toolbarLight.background, std::string("#fedcba"));
    REQUIRE_EQ(package->toolbarLight.icon, std::string("#edcba9"));
    REQUIRE_EQ(package->toolbarLight.handle, package->light.accent);
    REQUIRE_EQ(package->toolbarLight.hover, package->light.hover);
}

TEST_CASE(candidate_skin_shared_base_derives_selection_and_hover_from_overridden_colors)
{
    SkinFixture fixture;
    fixture.Write("sample", "paper", "[candidate.light]\naccent = '#C0392B'\ntext = '#202020'\n");
    const auto package = CandidateSkinCatalog::Load(fixture.root, "sample");
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->light.selected, std::string("#C0392B24"));
    REQUIRE_EQ(package->light.hover, std::string("#2020200F"));
    REQUIRE_EQ(package->light.selectedText, std::string("#C0392B"));
    REQUIRE_EQ(package->light.menuHover, package->light.hover);
    REQUIRE_EQ(package->toolbarLight.hover, package->light.hover);

    fixture.Write("sample", "paper", "[candidate.light]\naccent = '#C0392B80'\ntext = '#abc'\n");
    auto normalized = CandidateSkinCatalog::Load(fixture.root, "sample");
    REQUIRE(normalized.has_value());
    REQUIRE_EQ(normalized->light.selected, std::string("#C0392B24"));
    REQUIRE_EQ(normalized->light.hover, std::string("#aabbcc0F"));

    fixture.Write("sample", "paper", "[candidate.light]\naccent = 'rgba(192, 57, 43, 1)'\ntext = 'black'\n");
    normalized = CandidateSkinCatalog::Load(fixture.root, "sample");
    REQUIRE(normalized.has_value());
    REQUIRE_EQ(normalized->light.selected, std::string("#2C7A4B24"));
    REQUIRE_EQ(normalized->light.hover, std::string("#1A1E1B0F"));
}

TEST_CASE(candidate_skin_shared_base_scan_rejects_unknown_base_and_keeps_legacy_defaults)
{
    SkinFixture fixture;
    for (const auto &base : CandidateSkinCatalog::BuiltInIds())
    {
        const auto id = "sample-" + base;
        fixture.Write(id, base);
        const auto package = CandidateSkinCatalog::Load(fixture.root, id);
        REQUIRE(package.has_value());
        REQUIRE_EQ(package->base, base);
        REQUIRE_EQ(package->themes.size(), size_t{2});
        REQUIRE(package->light.surface.empty());
        REQUIRE(package->dark.surface.empty());
        REQUIRE(package->toolbarLight.background.empty());
    }
    fixture.Write("sample-system", "system");
    fixture.Write("sample-paper", "paper");
    for (const char *base : {"unsupported-base", "custom", "native"})
    {
        fixture.Write("unknown", base);
        std::string error;
        REQUIRE(!CandidateSkinCatalog::Load(fixture.root, "unknown", &error).has_value());
        REQUIRE(!error.empty());
    }
    const auto catalog = CandidateSkinCatalog::Scan(fixture.root);
    REQUIRE_EQ(catalog.packages.size(), size_t{8});
    REQUIRE_EQ(catalog.issues.size(), size_t{1});
    REQUIRE_EQ(catalog.issues.front().folder, std::string("unknown"));
}

TEST_CASE(candidate_skin_shared_base_does_not_expand_declared_theme_support)
{
    SkinFixture fixture;
    fixture.Write("sample", "paper", "", 1, "['dark']");
    const auto package = CandidateSkinCatalog::Load(fixture.root, "sample");
    REQUIRE(package.has_value());
    REQUIRE(package->themes.empty());
    REQUIRE(!CandidateSkinCatalog::LoadSupported(fixture.root, "sample", "horizontal", "dark").has_value());
    REQUIRE(!CandidateSkinCatalog::LoadSupported(fixture.root, "sample", "horizontal", "light").has_value());
}

TEST_CASE(candidate_skin_shared_base_preserves_default_manifest_and_package_switch_precedence)
{
    SkinFixture fixture;
    const auto defaults = fixture.root / "default" / "fluent";
    std::filesystem::create_directories(defaults);
    {
        std::ofstream manifest(defaults / "skin.toml");
        manifest << "schema_version = 1\nid = 'fluent'\n[candidate_window]\npage_arrows = true\nfit_content = true\n";
        REQUIRE(manifest.good());
    }
    for (const char *base : {"system", "paper"})
    {
        fixture.Write("sample", base);
        auto package = CandidateSkinCatalog::Load(fixture.root, "sample");
        REQUIRE(package.has_value());
        REQUIRE(CandidateSkinCatalog::ResolvePageArrows(fixture.root, "sample", &*package));
        REQUIRE(CandidateSkinCatalog::ResolveFitContent(fixture.root, "sample", &*package));
        fixture.Write("sample", base, "page_arrows = false\nfit_content = false\n");
        package = CandidateSkinCatalog::Load(fixture.root, "sample");
        REQUIRE(package.has_value());
        REQUIRE(!CandidateSkinCatalog::ResolvePageArrows(fixture.root, "sample", &*package));
        REQUIRE(!CandidateSkinCatalog::ResolveFitContent(fixture.root, "sample", &*package));
    }
}
