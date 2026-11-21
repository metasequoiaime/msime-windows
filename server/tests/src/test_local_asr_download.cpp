// 本地语音模型安装器（settings/local_asr_download.*）的测试：导入识别、错误态、删除。网络会话被替换成
// 总是失败的桩，测试不联网；下载路径因此只验证「失败后进 error 态、不留残件」。
//
// 可选的真包安装：设置 MSIME_LOCAL_ASR_TEST_ARCHIVES 为一个目录，里面放
//   runtime.tar.bz2（sherpa-onnx 运行库压缩包）、silero_vad.onnx，以及任意几个模型压缩包（原文件名），
// 会逐个导入并检查安装结果（tar 解压、SHA256 校验、落位）。CI 不设这个变量。
#include "settings/local_asr_download.h"
#include "utils/network_proxy.h"
#include "voice-input/local_asr_catalog.h"

#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace NetworkProxy
{
HINTERNET OpenWinHttpSession(const wchar_t *)
{
    return nullptr;
}
} // namespace NetworkProxy

namespace
{
#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
            throw std::runtime_error("CHECK failed: " #expr);                                                          \
    } while (false)

namespace fs = std::filesystem;

local_asr_download::ModelStatus StatusOf(const std::string &id)
{
    return local_asr_download::GetStatuses().at(id);
}

bool IsBusy(const std::string &state)
{
    return state == "downloading" || state == "extracting" || state == "importing";
}

local_asr_download::ModelStatus WaitIdle(const std::string &id)
{
    for (int i = 0; i < 6000; ++i)
    {
        const auto status = StatusOf(id);
        if (!IsBusy(status.state))
            return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("task did not finish");
}

void TestWithoutNetwork(const fs::path &root)
{
    const std::string id = "sense-voice-small";
    CHECK(StatusOf(id).state == "absent");
    CHECK(StatusOf(id).detail.empty());
    CHECK(!local_asr_download::StartDownload("missing"));

    // 下载失败：进 error 态，不留下载残件。
    CHECK(local_asr_download::StartDownload(id));
    const auto failed = WaitIdle(id);
    CHECK(failed.state == "error");
    CHECK(!failed.error.empty());
    CHECK(!fs::exists(local_asr::Root() / ".download" / (std::string(local_asr::RuntimeArchive().name) + ".part")));

    // 导入一个不认识的文件：拒绝，给出原因。
    const fs::path bogus = root / L"不是模型.onnx";
    std::ofstream(bogus, std::ios::binary) << "definitely not a model";
    CHECK(local_asr_download::Import(id, bogus));
    const auto rejected = WaitIdle(id);
    CHECK(rejected.state == "error");
    CHECK(rejected.error.find("不是") != std::string::npos);
    CHECK(!local_asr_download::Import(id, root / "missing.onnx"));

    // 删除会清掉错误态。
    CHECK(local_asr_download::Delete(id));
    CHECK(StatusOf(id).state == "absent");
}

int TestRealArchives()
{
    const wchar_t *directory = _wgetenv(L"MSIME_LOCAL_ASR_TEST_ARCHIVES");
    if (!directory || !*directory)
        return 0;
    const fs::path archives = directory;
    const auto import = [](const std::string &id, const fs::path &file) {
        CHECK(local_asr_download::Import(id, file));
        const auto status = WaitIdle(id);
        if (status.state == "error")
            throw std::runtime_error("import of " + file.u8string() + " failed: " + status.error);
        return status;
    };
    int models = 0;
    for (const local_asr::ModelEntry &model : local_asr::Catalog())
    {
        const fs::path archive = archives / model.archive.name;
        if (!fs::exists(archive))
            continue;
        ++models;
        auto status = import(model.id, archive);
        CHECK(status.model_installed);
        CHECK(local_asr::ModelFilesInstalled(model));
        for (const local_asr::ModelFileEntry &file : model.files)
            CHECK(fs::exists(local_asr::ModelDirectory(model) / file.path));
        // test_wavs 等不需要的成员不解出来。
        CHECK(!fs::exists(local_asr::ModelDirectory(model) / "test_wavs"));
        if (!local_asr::RuntimeInstalled())
        {
            CHECK(status.state == "absent");
            CHECK(status.detail.find("运行库") != std::string::npos);
            import(model.id, archives / "runtime.tar.bz2");
        }
        if (local_asr::NeedsVad(model) && !local_asr::VadInstalled())
            import(model.id, archives / "silero_vad.onnx");
        CHECK(StatusOf(model.id).state == "ready");
        for (const local_asr::RuntimeLibrary &library : local_asr::RuntimeLibraries())
            CHECK(fs::file_size(local_asr::RuntimeDirectory() / library.name) == static_cast<uintmax_t>(library.size));
        std::cout << "installed " << model.id << " into " << local_asr::ModelDirectory(model).u8string() << "\n";
    }
    CHECK(models > 0);
    CHECK(!fs::exists(local_asr::Root() / ".download"));
    return models;
}
} // namespace

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    try
    {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        // 中文路径：tar 经标准输入读包、以工作目录落盘，不该受控制台代码页影响。
        const wchar_t *keep = _wgetenv(L"MSIME_LOCAL_ASR_TEST_KEEP_DIR");
        const fs::path root = keep && *keep ? fs::path(keep) : fs::path(temp) / L"msime 本地语音测试";
        fs::remove_all(root);
        fs::create_directories(root);
        _wputenv_s(L"METASEQUOIA_IME_DATA_DIR", root.c_str());
        TestWithoutNetwork(root);
        if (const int models = TestRealArchives())
            std::cout << "real archive install passed (" << models << " models)\n";
        if (!keep || !*keep)
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << "\n";
        return 1;
    }
    std::cout << "local_asr_download tests passed\n";
    return 0;
}
