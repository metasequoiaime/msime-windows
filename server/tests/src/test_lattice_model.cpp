// 词格语言模型 sc.lm 的按需下载：路径解析只认已落位的文件、校验拒绝字节不符的文件、删除后回到
// 未下载；Server 映射着文件时删除改留标记，Server 立即停用、文件释放后再删。消息 schema 由
// webview_contract 的 fixtures 覆盖。
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "src/session/engine_input_session.h"
#include "src/settings/lattice_model.h"

#include <Windows.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
// data_directory() 经 _wdupenv_s 读 CRT 环境副本，必须用 _wputenv_s 写，同 test_neural_model。
class ScopedDataDirectory
{
  public:
    ScopedDataDirectory()
        : root_(test::TempRoot() / (L"msime-lattice-model-测试-" + std::to_wstring(GetCurrentProcessId())))
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
void SeedModelFile()
{
    std::error_code ec;
    std::filesystem::create_directories(lattice_model::ModelFile().parent_path(), ec);
    REQUIRE(!ec);
    std::ofstream output(lattice_model::ModelFile(), std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(output));
    output << "kenlm";
}

// 模拟 Server 的只读映射：kenlm 打开文件时不带 FILE_SHARE_DELETE，文件因此删不掉。
class HeldFile
{
  public:
    explicit HeldFile(const std::filesystem::path &file)
        : handle_(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr))
    {
        REQUIRE(handle_ != INVALID_HANDLE_VALUE);
    }
    ~HeldFile()
    {
        Release();
    }
    HeldFile(const HeldFile &) = delete;
    HeldFile &operator=(const HeldFile &) = delete;

    void Release()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE handle_;
};
} // namespace

TEST_CASE(lattice_model_lives_under_data_directory_models)
{
    ScopedDataDirectory env;
    REQUIRE(lattice_model::ModelFile() == env.root() / L"models" / L"language-model" / L"sc.lm");
}

TEST_CASE(lattice_model_path_resolves_only_after_download)
{
    ScopedDataDirectory env;
    REQUIRE(ResolveLatticeLanguageModelPath().empty());
    REQUIRE(lattice_model::GetModelStatus().state == "absent");

    SeedModelFile();
    const std::string path = ResolveLatticeLanguageModelPath();
    REQUIRE(!path.empty());
    // 数据目录可能在非 ASCII 的用户目录下：解析结果是 UTF-8，按 UTF-8 转回来必须是同一个文件。
    REQUIRE(metasequoia::path_from_utf8(path.c_str()) == lattice_model::ModelFile());
    REQUIRE(lattice_model::GetModelStatus().state == "ready");
}

TEST_CASE(lattice_model_verification_rejects_mismatched_bytes)
{
    ScopedDataDirectory env;
    const std::filesystem::path wrong_size = env.root() / L"short.lm";
    {
        std::ofstream output(wrong_size, std::ios::binary | std::ios::trunc);
        output << "not a model";
    }
    std::string error;
    REQUIRE(!lattice_model::VerifyModelFile(wrong_size, error));
    REQUIRE(!error.empty());

    // 字节数对上但内容不对，必须由校验和拦下。
    const std::filesystem::path wrong_bytes = env.root() / L"zeros.lm";
    {
        std::ofstream output(wrong_bytes, std::ios::binary | std::ios::trunc);
    }
    std::filesystem::resize_file(wrong_bytes, 34736327);
    error.clear();
    REQUIRE(!lattice_model::VerifyModelFile(wrong_bytes, error));
    REQUIRE(error.find("校验和") != std::string::npos);

    REQUIRE(!lattice_model::ImportModel(env.root() / L"missing.lm"));
}

TEST_CASE(lattice_model_delete_returns_to_absent)
{
    ScopedDataDirectory env;
    SeedModelFile();
    REQUIRE(lattice_model::GetModelStatus().state == "ready");
    REQUIRE(lattice_model::DeleteModel());
    REQUIRE(lattice_model::GetModelStatus().state == "absent");
    REQUIRE(!lattice_model::GetModelStatus().delete_pending);
    REQUIRE(ResolveLatticeLanguageModelPath().empty());
    // 删除是幂等的。
    REQUIRE(lattice_model::DeleteModel());
}

TEST_CASE(lattice_model_delete_while_in_use_is_deferred_until_the_file_is_released)
{
    ScopedDataDirectory env;
    SeedModelFile();
    HeldFile held(lattice_model::ModelFile());

    // 文件被占用：删除仍算成功，但只是留下标记。
    REQUIRE(lattice_model::DeleteModel());
    REQUIRE(std::filesystem::exists(lattice_model::ModelFile()));
    const auto status = lattice_model::GetModelStatus();
    REQUIRE(status.state == "absent");
    REQUIRE(status.delete_pending);
    // Server 立即停用它；释放之前反复解析都不能把标记弄丢（否则下一次解析又会用上文件）。
    REQUIRE(ResolveLatticeLanguageModelPath().empty());
    REQUIRE(ResolveLatticeLanguageModelPath().empty());
    REQUIRE(lattice_model::GetModelStatus().delete_pending);
    // 待删除期间不接新的下载与导入：新文件落不了位。
    REQUIRE(!lattice_model::StartDownload());

    // 相当于 Server 重启、文件尚未映射：解析时把整个目录删掉，回到未下载。
    held.Release();
    REQUIRE(ResolveLatticeLanguageModelPath().empty());
    REQUIRE(!std::filesystem::exists(lattice_model::ModelFile().parent_path()));
    REQUIRE(lattice_model::GetModelStatus().state == "absent");
    REQUIRE(!lattice_model::GetModelStatus().delete_pending);
}

TEST_CASE(lattice_model_source_is_the_pinned_release_asset)
{
    REQUIRE(lattice_model::SourceUrl() ==
            "https://github.com/metasequoiaime/chinese-ime-lm/releases/download/language-model-libime-20260629/sc.lm");
}
