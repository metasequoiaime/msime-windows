// 神经整句高精度档模型的按需下载：路径解析只认已落位的文件、状态跟随磁盘、校验拒绝字节
// 不符的文件、删除后回到未下载。消息 schema 由 webview_contract 的 fixtures 覆盖。
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "src/session/engine_input_session.h"
#include "src/settings/neural_model.h"

#include <Windows.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
// data_directory() 经 _wdupenv_s 读 CRT 环境副本，必须用 _wputenv_s 写，同 test_collocation_model。
class ScopedDataDirectory
{
  public:
    ScopedDataDirectory()
        : root_(test::TempRoot() / (L"msime-neural-model-测试-" + std::to_wstring(GetCurrentProcessId())))
    {
        wchar_t *previous = nullptr;
        size_t size = 0;
        if (_wdupenv_s(&previous, &size, L"METASEQUOIA_IME_DATA_DIR") == 0 && previous != nullptr)
        {
            had_previous_ = true;
            previous_ = previous;
            free(previous);
        }
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_, ec);
        REQUIRE(!ec);
        _wputenv_s(L"METASEQUOIA_IME_DATA_DIR", root_.wstring().c_str());
    }
    ~ScopedDataDirectory()
    {
        _wputenv_s(L"METASEQUOIA_IME_DATA_DIR", had_previous_ ? previous_.c_str() : L"");
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    ScopedDataDirectory(const ScopedDataDirectory &) = delete;
    ScopedDataDirectory &operator=(const ScopedDataDirectory &) = delete;

    const std::filesystem::path &root() const
    {
        return root_;
    }

  private:
    std::filesystem::path root_;
    std::wstring previous_;
    bool had_previous_ = false;
};

// 查询侧把「文件存在」直接当 ready（落位只发生在校验之后），写几个字节即可驱动同一条判定。
void SeedFile(const std::filesystem::path &file)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    REQUIRE(!ec);
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(output));
    output << "safetensors";
}

void SeedModelFile()
{
    SeedFile(neural_model::ModelFile());
}
} // namespace

TEST_CASE(neural_desktop_model_lives_under_data_directory_models)
{
    ScopedDataDirectory env;
    REQUIRE(neural_model::ModelFile() ==
            env.root() / L"models" / L"neural-desktop" / L"sentence-model-desktop-v2.safetensors");
    // 旧版 model-v1 的落位名，老用户的数据目录里就是这个文件；新版必须换名，否则看不出磁盘上是哪一版。
    REQUIRE(neural_model::LegacyModelFile() ==
            env.root() / L"models" / L"neural-desktop" / L"sentence-model-desktop.safetensors");
    REQUIRE(neural_model::ModelFile() != neural_model::LegacyModelFile());
}

TEST_CASE(neural_desktop_model_keeps_using_legacy_until_update)
{
    ScopedDataDirectory env;
    // 老用户升级上来：只有 model-v1。引擎继续用它，设置页提示更新。
    SeedFile(neural_model::LegacyModelFile());
    REQUIRE(metasequoia::path_from_utf8(ResolveNeuralDesktopModelPath().c_str()) == neural_model::LegacyModelFile());
    auto status = neural_model::GetModelStatus();
    REQUIRE(status.state == "absent");
    REQUIRE(status.update_available);

    // 新版落位后改用新版，不再提示。
    SeedModelFile();
    REQUIRE(metasequoia::path_from_utf8(ResolveNeuralDesktopModelPath().c_str()) == neural_model::ModelFile());
    status = neural_model::GetModelStatus();
    REQUIRE(status.state == "ready");
    REQUIRE(!status.update_available);

    // 删除模型连旧版一起删，回到未下载。
    REQUIRE(neural_model::DeleteModel());
    REQUIRE(!std::filesystem::exists(neural_model::LegacyModelFile()));
    REQUIRE(ResolveNeuralDesktopModelPath().empty());
    status = neural_model::GetModelStatus();
    REQUIRE(status.state == "absent");
    REQUIRE(!status.update_available);
}

TEST_CASE(neural_desktop_model_without_legacy_has_no_update)
{
    ScopedDataDirectory env;
    // 新用户：什么都没有就是「未下载」，不是「有更新」。
    REQUIRE(!neural_model::GetModelStatus().update_available);
}

TEST_CASE(neural_desktop_model_path_resolves_only_after_download)
{
    ScopedDataDirectory env;
    REQUIRE(ResolveNeuralDesktopModelPath().empty());
    REQUIRE(neural_model::GetModelStatus().state == "absent");

    SeedModelFile();
    const std::string path = ResolveNeuralDesktopModelPath();
    REQUIRE(!path.empty());
    // 数据目录可能在非 ASCII 的用户目录下：解析结果是 UTF-8，按 UTF-8 转回来必须是同一个文件。
    REQUIRE(metasequoia::path_from_utf8(path.c_str()) == neural_model::ModelFile());
    REQUIRE(neural_model::GetModelStatus().state == "ready");
}

TEST_CASE(neural_desktop_model_verification_rejects_mismatched_bytes)
{
    ScopedDataDirectory env;
    const std::filesystem::path wrong_size = env.root() / L"short.safetensors";
    {
        std::ofstream output(wrong_size, std::ios::binary | std::ios::trunc);
        output << "not a model";
    }
    std::string error;
    REQUIRE(!neural_model::VerifyModelFile(wrong_size, error));
    REQUIRE(!error.empty());

    // 字节数对上但内容不对，必须由校验和拦下。
    const std::filesystem::path wrong_bytes = env.root() / L"zeros.safetensors";
    {
        std::ofstream output(wrong_bytes, std::ios::binary | std::ios::trunc);
    }
    std::filesystem::resize_file(wrong_bytes, 25710000);
    error.clear();
    REQUIRE(!neural_model::VerifyModelFile(wrong_bytes, error));
    REQUIRE(error.find("校验和") != std::string::npos);

    REQUIRE(!neural_model::ImportModel(env.root() / L"missing.safetensors"));
}

TEST_CASE(neural_desktop_model_delete_returns_to_absent)
{
    ScopedDataDirectory env;
    SeedModelFile();
    REQUIRE(neural_model::GetModelStatus().state == "ready");
    REQUIRE(neural_model::DeleteModel());
    REQUIRE(neural_model::GetModelStatus().state == "absent");
    REQUIRE(ResolveNeuralDesktopModelPath().empty());
    // 删除是幂等的。
    REQUIRE(neural_model::DeleteModel());
}

TEST_CASE(neural_desktop_model_source_is_the_locked_release_asset)
{
    REQUIRE(neural_model::SourceUrl() ==
            "https://github.com/metasequoiaime/chinese-ime-lm/releases/download/rerank-v2-safetensors/"
            "rerank-8x448.safetensors");
}

// 真正的下载落位要走网络，这里用导入走同一段落位代码：仅当本机备好了发布文件（环境变量指向它）
// 时才跑，验证新版落位后旧版被删掉。
TEST_CASE(neural_desktop_model_install_removes_legacy)
{
    wchar_t *source = nullptr;
    size_t size = 0;
    if (_wdupenv_s(&source, &size, L"MSIME_TEST_NEURAL_DESKTOP_MODEL") != 0 || source == nullptr)
        return;
    const std::filesystem::path release_file = source;
    free(source);
    if (!std::filesystem::is_regular_file(release_file))
        return;

    ScopedDataDirectory env;
    SeedFile(neural_model::LegacyModelFile());
    REQUIRE(neural_model::ImportModel(release_file));
    for (int i = 0; i < 600 && neural_model::GetModelStatus().state == "importing"; ++i)
        Sleep(100);
    const auto status = neural_model::GetModelStatus();
    REQUIRE(status.state == "ready");
    REQUIRE(!status.update_available);
    REQUIRE(!std::filesystem::exists(neural_model::LegacyModelFile()));
    REQUIRE(std::filesystem::exists(neural_model::ModelFile().parent_path() / "NOTICE.md"));
}
