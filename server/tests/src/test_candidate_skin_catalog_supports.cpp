#include "tests/includes/test_framework.h"

#include "skin/candidate_skin_catalog.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

// 皮肤在 skin.toml 里用 supports.layouts / supports.themes 声明自己支持什么。一份只为横排
// 画的皮肤写 layouts = ["horizontal"]，本意是「别在竖排套用我」。两个渲染后端与两个辅助
// 浮层都必须读同一条判定，否则用户在默认后端下切到竖排会拿到一张错位的皮肤。
// LoadSupported 就是这条判定，这里钉住它的行为。

namespace
{
namespace fs = std::filesystem;

fs::path MakeSkinsRoot(const std::wstring &leaf)
{
    const fs::path root =
        fs::temp_directory_path() / (L"msime-skin-support-" + std::to_wstring(GetCurrentProcessId())) / leaf / L"skins";
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

void WriteSkin(const fs::path &root, const std::string &id, const std::string &supportsBody)
{
    WriteFile(root / fs::u8path(id) / L"skin.toml", "schema_version = 1\nid = \"" + id +
                                                        "\"\nname = \"Ext\"\n"
                                                        "version = \"1.0.0\"\nbase = \"fluent\"\n\n[supports]\n" +
                                                        supportsBody + "\n[candidate_window]\n");
}

// root 是 <temp>/msime-skin-support-<pid>/<leaf>/skins，连同按进程号建的那层一起删掉。
void Cleanup(const fs::path &root)
{
    std::error_code ec;
    fs::remove_all(root.parent_path().parent_path(), ec);
}
} // namespace

// 声明只支持横排的皮肤：横排拿得到，竖排拿不到、回退 fluent。
TEST_CASE(candidate_skin_load_supported_rejects_unsupported_layout)
{
    const auto root = MakeSkinsRoot(L"layout");
    WriteSkin(root, "horizontal-only", "layouts = [\"horizontal\"]\nthemes = [\"dark\", \"light\"]\n");

    REQUIRE(CandidateSkinCatalog::LoadSupported(root, "horizontal-only", "horizontal", "dark").has_value());
    REQUIRE(CandidateSkinCatalog::LoadSupported(root, "horizontal-only", "horizontal", "light").has_value());
    REQUIRE(!CandidateSkinCatalog::LoadSupported(root, "horizontal-only", "vertical", "dark").has_value());
    REQUIRE(!CandidateSkinCatalog::LoadSupported(root, "horizontal-only", "vertical", "light").has_value());

    // 只 Load 不校验时仍然读得到，LoadSupported 的拒绝来自布局判定而不是别的。
    REQUIRE(CandidateSkinCatalog::Load(root, "horizontal-only").has_value());
    Cleanup(root);
}

// 主题同理：只写一份深色配色的皮肤不应被套到浅色上。
TEST_CASE(candidate_skin_load_supported_rejects_unsupported_theme)
{
    const auto root = MakeSkinsRoot(L"theme");
    WriteSkin(root, "dark-only", "layouts = [\"horizontal\", \"vertical\"]\nthemes = [\"dark\"]\n");

    REQUIRE(CandidateSkinCatalog::LoadSupported(root, "dark-only", "vertical", "dark").has_value());
    REQUIRE(!CandidateSkinCatalog::LoadSupported(root, "dark-only", "vertical", "light").has_value());
    Cleanup(root);
}

// 声明支持两种布局两种主题的皮肤，四种组合都要能读到，避免判定写得过紧。
TEST_CASE(candidate_skin_load_supported_accepts_every_declared_combination)
{
    const auto root = MakeSkinsRoot(L"both");
    WriteSkin(root, "universal", "layouts = [\"horizontal\", \"vertical\"]\nthemes = [\"dark\", \"light\"]\n");

    for (const char *layout : {"horizontal", "vertical"})
    {
        for (const char *theme : {"dark", "light"})
        {
            REQUIRE(CandidateSkinCatalog::LoadSupported(root, "universal", layout, theme).has_value());
        }
    }
    Cleanup(root);
}

// 读不出来的皮肤（缺 manifest、ID 非法）同样返回空，调用方回退 fluent。
TEST_CASE(candidate_skin_load_supported_returns_nothing_when_manifest_is_unusable)
{
    const auto root = MakeSkinsRoot(L"unusable");
    WriteSkin(root, "ok", "layouts = [\"horizontal\"]\nthemes = [\"dark\"]\n");

    // 同一目录下的正常皮肤读得到，下面几条的拒绝来自 ID 本身，而不是目录没建好。
    REQUIRE(CandidateSkinCatalog::LoadSupported(root, "ok", "horizontal", "dark").has_value());
    REQUIRE(!CandidateSkinCatalog::LoadSupported(root, "missing", "horizontal", "dark").has_value());
    REQUIRE(!CandidateSkinCatalog::LoadSupported(root, "../escape", "horizontal", "dark").has_value());
    REQUIRE(!CandidateSkinCatalog::LoadSupported(root, "", "horizontal", "dark").has_value());
    Cleanup(root);
}
