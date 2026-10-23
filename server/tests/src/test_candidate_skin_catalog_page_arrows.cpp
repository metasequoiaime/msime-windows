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
namespace fs = std::filesystem;

fs::path MakeSkinsRoot(const std::wstring &leaf)
{
    const fs::path root =
        test::TempRoot() / (L"msime-skin-page-arrows-" + std::to_wstring(GetCurrentProcessId())) / leaf / L"skins";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    REQUIRE(!ec);
    return root;
}

void WriteFile(const fs::path &path, const std::string &text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    REQUIRE(!ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << text;
}

void WriteDefaultSkin(const fs::path &root, const std::string &id, const std::string &windowTable)
{
    WriteFile(root / L"default" / fs::u8path(id) / L"skin.toml",
              "schema_version = 1\nid = \"" + id + "\"\nname = \"" + id + "\"\n" + windowTable);
}

void WriteExternalSkin(const fs::path &root, const std::string &id, const std::string &base,
                       const std::string &windowKeys)
{
    WriteFile(root / fs::u8path(id) / L"skin.toml", "schema_version = 1\nid = \"" + id +
                                                        "\"\nname = \"Ext\"\nversion = \"1.0.0\"\nbase = \"" + base +
                                                        "\"\n\n[supports]\nlayouts = [\"horizontal\", \"vertical\"]\n"
                                                        "themes = [\"dark\", \"light\"]\n\n[candidate_window]\n" +
                                                        windowKeys);
}

void Cleanup(const fs::path &root)
{
    std::error_code ec;
    fs::remove_all(root.parent_path(), ec);
}
} // namespace

// A built-in skin takes the switch from its own manifest under skins/default; without one the shipped default applies.
TEST_CASE(candidate_skin_page_arrows_follow_default_manifest_for_built_in_skin)
{
    const auto root = MakeSkinsRoot(L"builtin");
    REQUIRE(CandidateSkinCatalog::ResolvePageArrows(root, "graphite", nullptr) ==
            CandidateSkinCatalog::kDefaultPageArrows);

    // Arrows are off unless a manifest turns them on.
    REQUIRE(!CandidateSkinCatalog::kDefaultPageArrows);
    WriteDefaultSkin(root, "graphite", "[candidate_window]\npage_arrows = true\n");
    REQUIRE(CandidateSkinCatalog::ResolvePageArrows(root, "graphite", nullptr));
    // Another built-in skin is not affected by graphite's manifest.
    REQUIRE(CandidateSkinCatalog::ResolvePageArrows(root, "fluent", nullptr) ==
            CandidateSkinCatalog::kDefaultPageArrows);

    const auto defaults = CandidateSkinCatalog::LoadDefault(root, "graphite");
    REQUIRE(defaults.has_value());
    REQUIRE(defaults->pageArrows.has_value() && *defaults->pageArrows);
    Cleanup(root);
}

// An external skin that leaves the key out inherits it from its base; its own value always wins.
TEST_CASE(candidate_skin_page_arrows_external_skin_overrides_or_inherits_base)
{
    const auto root = MakeSkinsRoot(L"external");
    WriteDefaultSkin(root, "wechat", "[candidate_window]\npage_arrows = true\n");
    WriteExternalSkin(root, "inherit", "wechat", "");
    WriteExternalSkin(root, "override", "wechat", "page_arrows = false\n");

    const auto inherit = CandidateSkinCatalog::Load(root, "inherit");
    REQUIRE(inherit.has_value());
    REQUIRE(!inherit->pageArrows.has_value());
    REQUIRE(CandidateSkinCatalog::ResolvePageArrows(root, "inherit", &*inherit));

    const auto explicitOff = CandidateSkinCatalog::Load(root, "override");
    REQUIRE(explicitOff.has_value());
    REQUIRE(!CandidateSkinCatalog::ResolvePageArrows(root, "override", &*explicitOff));
    Cleanup(root);
}

TEST_CASE(candidate_skin_page_arrows_reject_non_boolean_values)
{
    const auto root = MakeSkinsRoot(L"invalid");
    WriteExternalSkin(root, "bad", "fluent", "page_arrows = \"yes\"\n");
    std::string error;
    REQUIRE(!CandidateSkinCatalog::Load(root, "bad", &error).has_value());
    REQUIRE(!error.empty());

    WriteDefaultSkin(root, "fluent", "[candidate_window]\npage_arrows = 1\n");
    error.clear();
    REQUIRE(!CandidateSkinCatalog::LoadDefault(root, "fluent", &error).has_value());
    REQUIRE(!error.empty());
    // A broken default manifest falls back to the shipped default.
    REQUIRE(CandidateSkinCatalog::ResolvePageArrows(root, "fluent", nullptr) ==
            CandidateSkinCatalog::kDefaultPageArrows);
    Cleanup(root);
}

// fit_content resolves like page_arrows: the package's own value, else its base's default manifest, else off.
TEST_CASE(candidate_skin_fit_content_resolves_like_page_arrows)
{
    const auto root = MakeSkinsRoot(L"fit-content");
    REQUIRE(!CandidateSkinCatalog::kDefaultFitContent);
    REQUIRE(!CandidateSkinCatalog::ResolveFitContent(root, "graphite", nullptr));

    WriteDefaultSkin(root, "graphite", "[candidate_window]\nfit_content = true\n");
    REQUIRE(CandidateSkinCatalog::ResolveFitContent(root, "graphite", nullptr));
    REQUIRE(!CandidateSkinCatalog::ResolveFitContent(root, "fluent", nullptr));
    // The two switches are independent.
    REQUIRE(!CandidateSkinCatalog::ResolvePageArrows(root, "graphite", nullptr));

    WriteExternalSkin(root, "inherit", "graphite", "");
    WriteExternalSkin(root, "override", "graphite", "fit_content = false\n");
    const auto inherit = CandidateSkinCatalog::Load(root, "inherit");
    REQUIRE(inherit.has_value());
    REQUIRE(!inherit->fitContent.has_value());
    REQUIRE(CandidateSkinCatalog::ResolveFitContent(root, "inherit", &*inherit));
    const auto explicitOff = CandidateSkinCatalog::Load(root, "override");
    REQUIRE(explicitOff.has_value());
    REQUIRE(!CandidateSkinCatalog::ResolveFitContent(root, "override", &*explicitOff));

    WriteExternalSkin(root, "bad", "fluent", "fit_content = \"yes\"\n");
    std::string error;
    REQUIRE(!CandidateSkinCatalog::Load(root, "bad", &error).has_value());
    REQUIRE(!error.empty());
    Cleanup(root);
}

// expand_panel resolves like page_arrows; expand_rows is a package-only integer in [2, 8].
TEST_CASE(candidate_skin_expand_panel_resolves_like_page_arrows)
{
    const auto root = MakeSkinsRoot(L"expand-panel");
    REQUIRE(!CandidateSkinCatalog::kDefaultExpandPanel);
    REQUIRE(!CandidateSkinCatalog::ResolveExpandPanel(root, "wechat", nullptr));

    WriteDefaultSkin(root, "wechat", "[candidate_window]\nexpand_panel = true\n");
    REQUIRE(CandidateSkinCatalog::ResolveExpandPanel(root, "wechat", nullptr));
    REQUIRE(!CandidateSkinCatalog::ResolveExpandPanel(root, "fluent", nullptr));
    REQUIRE(!CandidateSkinCatalog::ResolvePageArrows(root, "wechat", nullptr));

    WriteExternalSkin(root, "inherit", "wechat", "");
    WriteExternalSkin(root, "override", "wechat", "expand_panel = false\n");
    WriteExternalSkin(root, "rows", "fluent", "expand_panel = true\nexpand_rows = 6\n");
    const auto inherit = CandidateSkinCatalog::Load(root, "inherit");
    REQUIRE(inherit.has_value());
    REQUIRE(!inherit->expandPanel.has_value());
    REQUIRE(!inherit->expandRows.has_value());
    REQUIRE(CandidateSkinCatalog::ResolveExpandPanel(root, "inherit", &*inherit));
    const auto explicitOff = CandidateSkinCatalog::Load(root, "override");
    REQUIRE(explicitOff.has_value());
    REQUIRE(!CandidateSkinCatalog::ResolveExpandPanel(root, "override", &*explicitOff));
    const auto rows = CandidateSkinCatalog::Load(root, "rows");
    REQUIRE(rows.has_value());
    REQUIRE(CandidateSkinCatalog::ResolveExpandPanel(root, "rows", &*rows));
    REQUIRE(rows->expandRows.has_value() && *rows->expandRows == 6);

    for (const char *bad :
         {"expand_panel = \"yes\"\n", "expand_rows = 1\n", "expand_rows = 9\n", "expand_rows = 4.5\n"})
    {
        WriteExternalSkin(root, "bad", "fluent", bad);
        std::string error;
        REQUIRE(!CandidateSkinCatalog::Load(root, "bad", &error).has_value());
        REQUIRE(!error.empty());
    }
    Cleanup(root);
}

// skins/default holds the built-in skins' settings, not a skin package: the scan must not list it as a broken
// external skin, and it cannot be selected as one either.
TEST_CASE(candidate_skin_scan_skips_default_settings_folder)
{
    const auto root = MakeSkinsRoot(L"scan");
    WriteDefaultSkin(root, "fluent", "[candidate_window]\npage_arrows = true\n");
    WriteExternalSkin(root, "ext", "fluent", "");

    const auto scan = CandidateSkinCatalog::Scan(root);
    REQUIRE(scan.issues.empty());
    REQUIRE(scan.packages.size() == 1);
    REQUIRE(scan.packages[0].id == "ext");
    REQUIRE(!CandidateSkinCatalog::Load(root, "default").has_value());
    Cleanup(root);
}

// The installer renames a bundled skin the user already had to <id>.bak (or <id>.2.bak) before overwriting it.
// Those backups keep the original manifest id, so without skipping them the settings page would list each one as
// a broken skin.
TEST_CASE(candidate_skin_scan_skips_installer_backups)
{
    const auto root = MakeSkinsRoot(L"backup");
    WriteExternalSkin(root, "ext", "fluent", "");
    fs::copy(root / L"ext", root / L"ext.bak", fs::copy_options::recursive);
    fs::copy(root / L"ext", root / L"ext.2.bak", fs::copy_options::recursive);
    fs::copy(root / L"ext", root / L"ext.3.BAK", fs::copy_options::recursive);

    const auto scan = CandidateSkinCatalog::Scan(root);
    REQUIRE(scan.issues.empty());
    REQUIRE(scan.packages.size() == 1);
    REQUIRE(scan.packages[0].id == "ext");
    REQUIRE(CandidateSkinCatalog::IsBackupFolder("ext.bak"));
    REQUIRE(!CandidateSkinCatalog::IsBackupFolder("bak"));
    REQUIRE(!CandidateSkinCatalog::IsBackupFolder("ext.backup"));
    Cleanup(root);
}
