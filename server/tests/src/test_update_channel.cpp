#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "settings/native/native_settings_update_download.h"
#include "settings/update_channel.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
using update_channel::Compare;
using update_channel::Kind;
using update_channel::Version;

Version Parsed(const std::string &text)
{
    Version version;
    REQUIRE(update_channel::Parse(text, version));
    return version;
}
} // namespace

TEST_CASE(update_channel_classifies_each_release_line)
{
    // 正式版没有任何预发布标签；nightly 和 dev 由 dev-build-version.py 生成，形如
    // MAJOR.MINOR.PATCH-nightly.YYYYMMDD.<短提交>。
    REQUIRE(Parsed("0.7.1").kind == Kind::Release);
    REQUIRE(Parsed("v0.7.1").kind == Kind::Release);
    REQUIRE(Parsed("0.7.1-nightly.20261022.7fa6fb1a").kind == Kind::Nightly);
    REQUIRE(Parsed("0.7.1-dev.20261022.7fa6fb1a").kind == Kind::Dev);
    REQUIRE(Parsed("0.7.1-nightly.20261022.7fa6fb1a").prerelease == "nightly.20261022.7fa6fb1a");
    REQUIRE(Parsed("0.7.1-nightly.20261022.7fa6fb1a").label == "nightly");

    // 构建元数据不参与比较。
    REQUIRE(Parsed("0.7.1+build.5").text == "0.7.1");
}

TEST_CASE(update_channel_rejects_malformed_versions)
{
    Version version;
    REQUIRE(!update_channel::Parse("", version));
    REQUIRE(!version.valid);
    REQUIRE(!update_channel::Parse("abc", version));
    REQUIRE(!update_channel::Parse("0.x.1", version));
    REQUIRE(!update_channel::Parse("0.7.1-", version));
}

TEST_CASE(update_channel_orders_versions_inside_one_line)
{
    // 正式版高于同号的预发布版本，这是 semver 的定义，也是「nightly 不会把自己认成比正式版新」
    // 的前提：nightly 只和 nightly 比，正式版只和正式版比。
    REQUIRE(Compare(Parsed("0.7.1"), Parsed("0.7.0")) > 0);
    REQUIRE(Compare(Parsed("0.7.1"), Parsed("0.7.1-nightly.20261022.7fa6fb1a")) > 0);
    REQUIRE(Compare(Parsed("0.8.0"), Parsed("0.7.9")) > 0);
    // 三段补齐：0.7 等价于 0.7.0。
    REQUIRE(Compare(Parsed("0.7"), Parsed("0.7.0")) == 0);

    const auto nightly_a = Parsed("0.7.1-nightly.20261022.7fa6fb1a");
    const auto nightly_b = Parsed("0.7.1-nightly.20261023.abcd1234");
    REQUIRE(Compare(nightly_b, nightly_a) > 0);
    // 日期相同就比提交，数字段按数值比较，与字符串长度无关。
    REQUIRE(Compare(Parsed("0.7.1-nightly.20261022.b"), Parsed("0.7.1-nightly.20261022.aa")) > 0);
}

TEST_CASE(update_channel_auto_update_only_runs_on_release_lines)
{
    // dev 构建永远不参与自动更新：它的版本号每天变，比较结果没有意义。
    REQUIRE(update_channel::SupportsAutoUpdate(Parsed("0.7.1")));
    REQUIRE(update_channel::SupportsAutoUpdate(Parsed("0.7.1-nightly.20261022.7fa6fb1a")));
    REQUIRE(!update_channel::SupportsAutoUpdate(Parsed("0.7.1-dev.20261022.7fa6fb1a")));
    Version invalid;
    REQUIRE(!update_channel::SupportsAutoUpdate(invalid));

    // 认不出的预发布标签不属于任何通道，不能被当成正式版参与自动下载。
    Version beta;
    REQUIRE(!update_channel::Parse("0.7.1-beta.3", beta));
    REQUIRE(!update_channel::SupportsAutoUpdate(beta));

    // 界面用这个名字决定渲染不渲染勾选框。
    REQUIRE_EQ(std::string(update_channel::ChannelNameOf("0.7.1")), std::string("release"));
    REQUIRE_EQ(std::string(update_channel::ChannelNameOf("0.7.1-nightly.20261022.7fa6fb1a")), std::string("nightly"));
    REQUIRE_EQ(std::string(update_channel::ChannelNameOf("0.7.1-dev.20261022.7fa6fb1a")), std::string());
    REQUIRE_EQ(std::string(update_channel::ChannelNameOf("0.7.1-beta.3")), std::string());
}

TEST_CASE(update_channel_survives_absurd_version_numbers)
{
    // 清单来自网络。超长数字段过去会让 std::stol 抛 out_of_range，穿透到轮询线程里
    // 把常驻的 Server 打崩；现在必须只是「解析失败」。
    Version version;
    REQUIRE(!update_channel::Parse("999999999999999999999999.1", version));
    REQUIRE(!update_channel::Parse("1.99999999999999999999999.1", version));
    REQUIRE(update_channel::Parse("20261022.7.1", version));
    REQUIRE(version.parts.size() == 3);
}

TEST_CASE(update_channel_reads_the_tag_out_of_a_release_url)
{
    REQUIRE(update_channel::TagFromReleaseUrl("https://github.com/metasequoiaime/MSIME-Windows/releases/tag/v0.7.1") ==
            "v0.7.1");
    REQUIRE(update_channel::TagFromReleaseUrl(
                "https://github.com/metasequoiaime/MSIME-Windows/releases/tag/0.7.1-nightly.20261022.7fa6fb1a") ==
            "0.7.1-nightly.20261022.7fa6fb1a");
    // 别的仓库、别的路径形状一律不认。
    REQUIRE(update_channel::TagFromReleaseUrl("https://example.com/releases/tag/v1").empty());
    REQUIRE(update_channel::TagFromReleaseUrl("https://github.com/metasequoiaime/x/releases/latest").empty());
}

TEST_CASE(update_check_puts_installers_under_the_data_directory)
{
    namespace fs = std::filesystem;
    const fs::path root = test::TempRoot() / L"update-check-directory";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    REQUIRE(!ec);

    // 数据目录由环境变量决定；测试里指向临时目录，避免碰到真实安装的位置。
    const std::wstring previous = [&] {
        DWORD needed = GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", nullptr, 0);
        std::wstring value(needed, L'\0');
        if (needed != 0)
            GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", value.data(), needed);
        if (needed != 0 && !value.empty() && value.back() == L'\0')
            value.pop_back();
        SetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", root.c_str());
        return value;
    }();

    // 安装包落在 updates/<版本>/，跨进程、跨设置窗口关闭都还在。
    const fs::path directory = update_channel::InstallerDirectory("0.11.1");
    REQUIRE(!directory.empty());
    REQUIRE(directory == root / L"updates" / L"0.11.1");

    // 版本号当目录名之前先过路径穿越检查。
    REQUIRE(update_channel::InstallerDirectory("..\\..\\windows").empty());
    REQUIRE(update_channel::InstallerDirectory("../..").empty());
    REQUIRE(update_channel::InstallerDirectory(".hidden").empty());
    REQUIRE(update_channel::InstallerDirectory("").empty());

    SetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", previous.empty() ? nullptr : previous.c_str());
    fs::remove_all(root, ec);
}

TEST_CASE(update_progress_round_trips_through_the_data_directory)
{
    namespace fs = std::filesystem;
    const fs::path root = test::TempRoot() / L"update-progress";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    REQUIRE(!ec);

    const std::wstring previous = [&] {
        DWORD needed = GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", nullptr, 0);
        std::wstring value(needed, L'\0');
        if (needed != 0)
            GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", value.data(), needed);
        if (needed != 0 && !value.empty() && value.back() == L'\0')
            value.pop_back();
        SetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", root.c_str());
        return value;
    }();

    const fs::path path = update_channel::ProgressPath();
    REQUIRE(!path.empty());
    REQUIRE(path == root / L"updates" / L"progress.txt");

    // 没有文件时是 Idle，而不是上一次的错误。
    REQUIRE(update_channel::ReadProgress().state == update_channel::DownloadProgress::State::Idle);

    update_channel::DownloadProgress writing;
    writing.state = update_channel::DownloadProgress::State::Downloading;
    writing.version = "0.11.2";
    writing.received = 42 * 1024 * 1024;
    writing.total = 108 * 1024 * 1024;
    writing.updated_at = 1700000000;
    update_channel::WriteProgress(writing);

    const update_channel::DownloadProgress read_back = update_channel::ReadProgress();
    REQUIRE(read_back.state == update_channel::DownloadProgress::State::Downloading);
    REQUIRE_EQ(read_back.version, std::string("0.11.2"));
    REQUIRE_EQ(read_back.received, std::uint64_t{42 * 1024 * 1024});
    REQUIRE_EQ(read_back.total, std::uint64_t{108 * 1024 * 1024});
    REQUIRE_EQ(read_back.updated_at, std::uint64_t{1700000000});

    // 三种终态也要能读回来，页面靠它决定显示哪句话。
    writing.state = update_channel::DownloadProgress::State::Ready;
    writing.reason.clear();
    update_channel::WriteProgress(writing);
    REQUIRE(update_channel::ReadProgress().state == update_channel::DownloadProgress::State::Ready);

    writing.state = update_channel::DownloadProgress::State::Failed;
    writing.reason = "所有下载通道都失败了";
    update_channel::WriteProgress(writing);
    const update_channel::DownloadProgress failed = update_channel::ReadProgress();
    REQUIRE(failed.state == update_channel::DownloadProgress::State::Failed);
    REQUIRE_EQ(failed.reason, std::string("所有下载通道都失败了"));

    // 内容坏了按 Idle 处理，不抛异常：一个展示用的文件不值得让页面出错。
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "state=nonsense\nversion=oops\n";
    }
    REQUIRE(update_channel::ReadProgress().state == update_channel::DownloadProgress::State::Idle);

    SetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", previous.empty() ? nullptr : previous.c_str());
    fs::remove_all(root, ec);
}
