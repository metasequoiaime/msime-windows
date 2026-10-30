#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "settings/native/native_settings_update_download.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_CASE(update_download_rejects_installer_names_it_would_never_write)
{
    using native_settings::update_download::IsInstallerName;
    REQUIRE(IsInstallerName("MetasequoiaIME_Setup_v0.11.1.exe"));
    REQUIRE(IsInstallerName("MetasequoiaIME_Setup_v0.11.1-dev.20261010.d1adf7df7682.exe"));
    // 只要不是这个名字形状，就说明清单不可信，不下载。
    REQUIRE(!IsInstallerName("setup.exe"));
    REQUIRE(!IsInstallerName("MetasequoiaIME_Setup_v0.11.1.exe.part"));
    REQUIRE(!IsInstallerName("MetasequoiaIME_Setup_v0.11.1.zip"));
    REQUIRE(!IsInstallerName("..\\MetasequoiaIME_Setup_v1.exe"));
    REQUIRE(!IsInstallerName(std::string(200, 'a') + ".exe"));
}

TEST_CASE(update_download_rejects_bad_digests_before_touching_the_network)
{
    using native_settings::update_download::IsSha256Hex;
    REQUIRE(IsSha256Hex(std::string(64, 'a')));
    REQUIRE(IsSha256Hex("321b09fab2c86c1072ae35e56158d8616698af2416a8a0ffbed6dafecb6f2a38"));
    REQUIRE(!IsSha256Hex(std::string(63, 'a')));
    REQUIRE(!IsSha256Hex(std::string(65, 'a')));
    // 大写不接受：清单一直是小写，大小写混用说明来源变了。
    REQUIRE(!IsSha256Hex(std::string(64, 'A')));
    REQUIRE(!IsSha256Hex(std::string(64, 'g')));
    REQUIRE(!IsSha256Hex(""));
}

TEST_CASE(update_download_builds_github_first_then_the_mirror)
{
    using namespace native_settings::update_download;
    const std::string release = "https://github.com/metasequoiaime/MSIME-Windows/releases/tag/v0.11.1";
    const std::vector<std::string> channels = ChannelUrls(release, "MetasequoiaIME_Setup_v0.11.1.exe");
    REQUIRE_EQ(channels.size(), size_t{2});
    REQUIRE_EQ(channels[0], std::string("https://github.com/metasequoiaime/MSIME-Windows/releases/download/v0.11.1/"
                                        "MetasequoiaIME_Setup_v0.11.1.exe"));
    REQUIRE_EQ(channels[1], "https://dl.msime.app/gh/" + channels[0]);

    // 不是本仓库的发布页地址时不产生任何通道，调用方据此报错而不是乱下。
    REQUIRE(
        ChannelUrls("https://github.com/other/repo/releases/tag/v1.0.0", "MetasequoiaIME_Setup_v1.0.0.exe").empty());
    REQUIRE(ChannelUrls("https://msime.app/download/", "MetasequoiaIME_Setup_v1.0.0.exe").empty());
    // 安装包名不合法时同样不产生通道。
    REQUIRE(ChannelUrls(release, "setup.exe").empty());
}

TEST_CASE(update_download_refuses_to_start_without_a_digest)
{
    using namespace native_settings::update_download;
    Request request;
    request.current_version = "0.11.1";
    request.release_url = "https://github.com/metasequoiaime/MSIME-Windows/releases/tag/v0.11.1";
    request.installer_name = "MetasequoiaIME_Setup_v0.11.1.exe";
    request.directory = test::TempRoot() / L"update-download-unit";

    // 没有摘要：直接失败，一次网络请求都不发。
    request.installer_sha256.clear();
    Result result = Download(request);
    REQUIRE(result.outcome == Outcome::Failed);
    REQUIRE(result.attempts.empty());
    REQUIRE(!result.message.empty());

    // 摘要格式不对同理。
    request.installer_sha256 = "not-a-digest";
    result = Download(request);
    REQUIRE(result.outcome == Outcome::Failed);
    REQUIRE(result.attempts.empty());
}

TEST_CASE(update_download_only_runs_on_release_builds)
{
    using native_settings::update_download::IsReleaseVersion;

    // 正式版放行。发布页的 tag 带 v，清单里的 version 字段两种写法都出现过。
    REQUIRE(IsReleaseVersion("0.11.1"));
    REQUIRE(IsReleaseVersion("v0.11.1"));
    REQUIRE(IsReleaseVersion("V0.11.1"));
    REQUIRE(IsReleaseVersion(" 0.11.1 "));

    // nightly 与 dev 构建一律不下载：厂商不给它们的清单，版本号也每天变。
    REQUIRE(!IsReleaseVersion("0.11.1-dev.20261010.d1adf7df7682"));
    REQUIRE(!IsReleaseVersion("0.11.1-nightly.20261010.d1adf7df7682"));
    REQUIRE(!IsReleaseVersion("0.11.1-beta.3"));
    REQUIRE(!IsReleaseVersion("0.11.1+build.5"));

    // 不是三段数字就谈不上正式版。
    REQUIRE(!IsReleaseVersion(""));
    REQUIRE(!IsReleaseVersion("0.11"));
    REQUIRE(!IsReleaseVersion("0.11.1.2"));
    REQUIRE(!IsReleaseVersion("abc"));
}

TEST_CASE(update_download_refuses_nightly_builds_before_touching_the_network)
{
    using namespace native_settings::update_download;
    Request request;
    request.current_version = "0.11.1-nightly.20261010.d1adf7df7682";
    request.release_url = "https://github.com/metasequoiaime/MSIME-Windows/releases/tag/0.11.1";
    request.installer_name = "MetasequoiaIME_Setup_v0.11.1.exe";
    request.installer_sha256 = std::string(64, 'a');
    request.directory = test::TempRoot() / L"update-download-nightly";

    // 版本号这一关排在摘要与地址检查之前：nightly 构建连一次 HTTP 都不该发。
    const Result result = Download(request);
    REQUIRE(result.outcome == Outcome::Failed);
    REQUIRE(result.attempts.empty());
    REQUIRE(!result.message.empty());
}

TEST_CASE(update_download_keeps_the_installer_out_of_the_data_directory)
{
    using native_settings::update_download::DefaultDirectory;
    const std::filesystem::path directory = DefaultDirectory();
    REQUIRE(!directory.empty());
    // 安装包不落在数据目录里：那里是中等完整性、对同机可写的，从那里拉起 exe 等于
    // 把「校验通过」和「用户执行」之间的空窗交给别人。
    REQUIRE(directory.filename().wstring().rfind(L"MetasequoiaImeUpdate-", 0) == 0);
    // 同一进程内重复调用给出同一个目录：第二次点击要落在同一个地方，靠临时文件名区分。
    REQUIRE(DefaultDirectory() == directory);
    // 目录必须真能建出来：下载前会 create_directories，建不出来就直接失败。
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    REQUIRE(!ec);
    REQUIRE(std::filesystem::is_directory(directory, ec));
    std::filesystem::remove_all(directory, ec);
}

TEST_CASE(update_download_gives_each_job_its_own_part_file)
{
    using native_settings::update_download::PartFileName;
    const std::string first = PartFileName("MetasequoiaIME_Setup_v0.11.1.exe");
    const std::string second = PartFileName("MetasequoiaIME_Setup_v0.11.1.exe");
    // 同一个版本被点了两次，两个任务各写各的临时文件，不会互相截断。
    REQUIRE(!first.empty());
    REQUIRE(first != second);
    REQUIRE(first.rfind("MetasequoiaIME_Setup_v0.11.1.exe.part", 0) == 0);
    // 临时名不能反过来被当成正式安装包名，否则清理时会把它当作目标保留下来。
    REQUIRE(!native_settings::update_download::IsInstallerName(first));
}

TEST_CASE(update_download_pruning_leaves_only_the_kept_installer)
{
    namespace fs = std::filesystem;
    using namespace native_settings::update_download;
    const fs::path directory = test::TempRoot() / L"update-download-prune";
    std::error_code ec;
    fs::remove_all(directory, ec);
    fs::create_directories(directory, ec);
    REQUIRE(!ec);

    const fs::path keep = directory / "MetasequoiaIME_Setup_v0.11.1.exe";
    const fs::path older = directory / "MetasequoiaIME_Setup_v0.11.0.exe";
    const fs::path part = directory / "MetasequoiaIME_Setup_v0.11.1.exe.part";
    const fs::path unrelated = directory / "notes.txt";
    for (const fs::path &path : {keep, older, part, unrelated})
    {
        std::ofstream file(path, std::ios::binary);
        file << "x";
    }

    PruneOldInstallers(directory, keep.filename().u8string());
    REQUIRE(fs::is_regular_file(keep));
    // 旧安装包与上次中断留下的临时文件都清掉，别的文件一概不动。
    REQUIRE(!fs::exists(older));
    REQUIRE(!fs::exists(part));
    REQUIRE(fs::is_regular_file(unrelated));

    // 打开前的重核对：内容一致返回 true，换掉一个字节返回 false。
    const std::string expected = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"; // "hello"
    const fs::path probe = directory / "MetasequoiaIME_Setup_v0.11.1.exe";
    {
        std::ofstream file(probe, std::ios::binary | std::ios::trunc);
        file << "hello";
    }
    REQUIRE(FileMatchesDigest(probe, expected));
    {
        std::ofstream file(probe, std::ios::binary | std::ios::trunc);
        file << "hellp";
    }
    REQUIRE(!FileMatchesDigest(probe, expected));
    // 摘要格式不对时直接判失败，不去读文件。
    REQUIRE(!FileMatchesDigest(probe, "not-a-digest"));

    fs::remove_all(directory, ec);
}
