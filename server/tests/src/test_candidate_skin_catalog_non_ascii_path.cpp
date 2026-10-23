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
constexpr const char *kManifest = R"(schema_version = 1
id = "demo"
name = "Demo"
version = "1.0.0"
author = "tester"
description = "fixture"
base = "fluent"

[supports]
layouts = ["horizontal", "vertical"]
themes = ["dark", "light"]

[candidate_window]
min_width_dip = 320.0
)";

// Writes a minimal but complete external skin under <root>/skins/demo.
void WriteSkin(const std::filesystem::path &skins_root)
{
    std::error_code ec;
    std::filesystem::create_directories(skins_root / L"demo", ec);
    REQUIRE(!ec);
    std::ofstream manifest(skins_root / L"demo" / L"skin.toml", std::ios::binary | std::ios::trunc);
    REQUIRE(manifest.is_open());
    manifest << kManifest;
}
} // namespace

// Skins live under the user profile, so the catalog must load them whatever the profile path looks
// like. Reading the manifest with toml::parse_file(manifest.string()) routed the path through the
// ANSI code page, which corrupts a non-ASCII (e.g. Chinese) path and throws on a code page that
// cannot represent the characters -- and that throw escapes the toml handlers around it.
TEST_CASE(candidate_skin_catalog_loads_under_non_ascii_path)
{
    namespace fs = std::filesystem;
    // The root stays ASCII so the ascii_root control below really is ASCII end to end: only the
    // non_ascii_root leg carries Chinese characters, which is what the assertions must isolate.
    const fs::path unique_root = test::TempRoot() / (L"msime-skin-test-" + std::to_wstring(GetCurrentProcessId()));

    std::error_code ec;
    fs::remove_all(unique_root, ec);

    const fs::path ascii_root = unique_root / L"ascii" / L"skins";
    const fs::path non_ascii_root = unique_root / L"用户目录" / L"skins";
    WriteSkin(ascii_root);
    WriteSkin(non_ascii_root);

    std::string ascii_error;
    const auto ascii_package = CandidateSkinCatalog::Load(ascii_root, "demo", &ascii_error);
    REQUIRE(ascii_package.has_value());

    // The Chinese path must behave exactly like the ASCII one, not throw and not report an error.
    std::string non_ascii_error;
    const auto non_ascii_package = CandidateSkinCatalog::Load(non_ascii_root, "demo", &non_ascii_error);
    REQUIRE(non_ascii_error.empty());
    REQUIRE(non_ascii_package.has_value());
    REQUIRE_EQ(non_ascii_package->id, ascii_package->id);
    REQUIRE_EQ(non_ascii_package->name, ascii_package->name);
    REQUIRE_EQ(non_ascii_package->base, ascii_package->base);

    // Scan() walks the same manifests, so it must survive the Chinese root as well.
    const auto scan = CandidateSkinCatalog::Scan(non_ascii_root);
    REQUIRE_EQ(scan.packages.size(), std::size_t{1});
    REQUIRE_EQ(scan.packages.front().id, std::string("demo"));

    fs::remove_all(unique_root, ec);
}
