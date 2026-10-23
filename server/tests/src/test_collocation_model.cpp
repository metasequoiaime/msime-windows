// octagram 模型目录（catalog）与激活链路的测试：快照集合形状、modelId 下载/删除守卫、
// 激活写入与「空激活 = 未选择任何模型」的解析语义。消息 schema 的形状（collocationModel
// Download/Delete 带 modelId）由 webview_contract 的 fixtures 覆盖，这里钉 Server 侧行为。
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "src/config/ime_config.h"
#include "src/session/engine_input_session.h"
#include "src/settings/collocation_model.h"

#include <Windows.h>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
// 环境变量隔离：进入时保存原值，退出时恢复（没有原值就删除）。
// 读写必须走 CRT 一侧：data_directory() 经 _wdupenv_s 读 CRT 环境副本，Win32 的
// SetEnvironmentVariableW 对它不可见（UCRT 只在 _wputenv_s 时同步缓存）——这是
// test_custom_shuangpin 同款写法的原因。
class ScopedEnv
{
  public:
    ScopedEnv(const wchar_t *name, const std::wstring &value) : name_(name)
    {
        wchar_t *previous = nullptr;
        size_t size = 0;
        if (_wdupenv_s(&previous, &size, name) == 0 && previous != nullptr)
        {
            had_previous_ = true;
            previous_ = previous;
            free(previous);
        }
        _wputenv_s(name, value.c_str());
    }
    ~ScopedEnv()
    {
        restore();
    }

    // 显式还原（幂等）：拆卸时环境变量必须先于 InitImeConfig() 还原，g_config_path 才能
    // 重算回真实位置；成员析构阶段会再调一次，靠标记跳过。
    void restore()
    {
        if (restored_)
        {
            return;
        }
        restored_ = true;
        // CRT 语义：空值即删除该变量。
        _wputenv_s(name_.c_str(), had_previous_ ? previous_.c_str() : L"");
    }

    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;

  private:
    std::wstring name_;
    std::wstring previous_;
    bool had_previous_ = false;
    bool restored_ = false;
};

// 一次性数据根 + 配置根。模型目录经 data_directory() 读环境变量（每次调用都读，切换
// 立即生效）；配置 setter 走 g_config_path，需要 InitImeConfig() 在环境变量就位后重算
// ——这是 test_config_non_ascii_path 验证过的顺序。
class ScopedCollocationEnvironment
{
  public:
    ScopedCollocationEnvironment()
        : root_(test::TempRoot() / (L"msime-collocation-model-test-" + std::to_wstring(GetCurrentProcessId()))),
          local_app_data_(L"LOCALAPPDATA", root_.wstring()),
          config_dir_(L"METASEQUOIA_IME_CONFIG_DIR", (root_ / L"metasequoiaime").wstring()),
          data_dir_(L"METASEQUOIA_IME_DATA_DIR", (root_ / L"metasequoiaime").wstring())
    {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_ / L"metasequoiaime", ec);
        std::filesystem::copy_file(MSIME_DEFAULT_CONFIG_PATH, root_ / L"metasequoiaime" / L"config.default.toml",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        REQUIRE(!ec);
        InitImeConfig();
    }
    ~ScopedCollocationEnvironment()
    {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        // 先还原环境变量再重算 g_config_path：配置路径是进程级缓存，悬在已删除的测试根上
        // 会让本测试之后的所有配置写入失败（顺序约定同 test_config_non_ascii_path）。
        data_dir_.restore();
        config_dir_.restore();
        local_app_data_.restore();
        InitImeConfig();
    }
    ScopedCollocationEnvironment(const ScopedCollocationEnvironment &) = delete;
    ScopedCollocationEnvironment &operator=(const ScopedCollocationEnvironment &) = delete;

    std::filesystem::path models_dir() const
    {
        return root_ / L"metasequoiaime" / L"models";
    }

  private:
    std::filesystem::path root_;
    ScopedEnv local_app_data_;
    ScopedEnv config_dir_;
    ScopedEnv data_dir_;
};

// 在数据根里放一个「已落位」的模型包。落位只在格式校验之后发生，所以查询侧把文件存在
// 直接当 ready；测试里写几个字节即可驱动同一条判定，不需要真实 .gram。
void SeedModelFile(const std::filesystem::path &models_dir, const std::wstring &id)
{
    std::error_code ec;
    std::filesystem::create_directories(models_dir / id, ec);
    REQUIRE(!ec);
    std::ofstream output(models_dir / id / (id + L".gram"), std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(output));
    output << "gram";
}

void WriteTextFile(const std::filesystem::path &file, const std::string &text)
{
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(output));
    output << text;
}

// 最小的合法 octagram 头：魔数 + 空双数组（44 字节，边界检查恰好落在文件内）。
// 导入链路会真的跑 GramDb::open 的格式校验，随便几个字节过不了。
void WriteMinimalGramFile(const std::filesystem::path &file)
{
    std::string bytes(44, '\0');
    const std::string magic = "Rime::Grammar/";
    bytes.replace(0, magic.size(), magic);
    WriteTextFile(file, bytes);
}
} // namespace

TEST_CASE(collocation_catalog_lists_four_builtin_models)
{
    const auto &catalog = collocation::Catalog();
    REQUIRE_EQ(catalog.size(), static_cast<std::size_t>(4));
    // 次序即设置页展示次序；首条目是推荐包（仅展示身份，不承载默认语义）。
    REQUIRE_EQ(std::string(catalog[0].id), std::string(collocation::kRecommendedModelId));
    REQUIRE_EQ(std::string(catalog[1].id), std::string("zh-hans-t-essay-bgw"));
    REQUIRE_EQ(std::string(catalog[2].id), std::string("zh-hans-t-essay-bgw-compact"));
    REQUIRE_EQ(std::string(catalog[3].id), std::string("zh-moqi"));
    // 每个条目都要带齐下载直链与展示字段；zh-moqi 的许可状态必须如实标注
    // （构建链未声明许可），NOTICE 与设置页都从这里取词。
    for (const auto &entry : catalog)
    {
        REQUIRE(entry.host != nullptr && *entry.host != L'\0');
        REQUIRE(entry.path != nullptr && entry.path[0] == L'/');
        REQUIRE(entry.size_hint != nullptr && *entry.size_hint != '\0');
        REQUIRE(entry.license != nullptr && *entry.license != '\0');
        REQUIRE(entry.badge != nullptr && *entry.badge != '\0');
    }
    REQUIRE(std::string(catalog[3].license_note).find("构建链未声明许可") != std::string::npos);
}

TEST_CASE(collocation_badge_is_per_model_and_falls_back_to_octagram_name)
{
    // 候选窗的整句来源标签按模型包各自声明，不靠 id 前缀猜：万象〔万象〕、八股文两个包
    // 都〔八股〕、白霜〔墨奇〕。
    REQUIRE_EQ(std::string(collocation::BadgeForModel(collocation::kRecommendedModelId)), std::string("万象"));
    REQUIRE_EQ(std::string(collocation::BadgeForModel("zh-hans-t-essay-bgw")), std::string("八股"));
    REQUIRE_EQ(std::string(collocation::BadgeForModel("zh-hans-t-essay-bgw-compact")), std::string("八股"));
    REQUIRE_EQ(std::string(collocation::BadgeForModel("zh-moqi")), std::string("墨奇"));
    // 目录外手填的 id（自备模型）落回格式通称，不冒充任何一家。
    REQUIRE_EQ(std::string(collocation::BadgeForModel("not-in-catalog")), std::string("八股"));
    REQUIRE_EQ(std::string(collocation::BadgeForModel("")), std::string("八股"));
}

TEST_CASE(collocation_statuses_cover_catalog_and_follow_disk)
{
    ScopedCollocationEnvironment env;
    auto statuses = collocation::GetModelStatuses();
    // 快照集合形状：逐目录条目给出一项，不多不少，未下载即 absent。
    REQUIRE_EQ(statuses.size(), collocation::Catalog().size());
    for (const auto &entry : collocation::Catalog())
    {
        REQUIRE(statuses.count(entry.id) == 1);
        REQUIRE_EQ(statuses[entry.id].state, std::string("absent"));
    }
    // 磁盘优先：文件存在即 ready（落位发生在格式校验之后），progress 顶格。
    SeedModelFile(env.models_dir(), L"zh-moqi");
    statuses = collocation::GetModelStatuses();
    REQUIRE_EQ(statuses["zh-moqi"].state, std::string("ready"));
    REQUIRE_EQ(statuses["zh-moqi"].progress, 100);
}

TEST_CASE(collocation_download_and_delete_reject_ids_outside_catalog)
{
    ScopedCollocationEnvironment env;
    // modelId 消息的 schema 形状由 webview_contract 的 fixtures 把守；这里钉 Server 侧
    // 对未知 id 的拒绝，下载与删除走同一守卫入口，路径拼接不允许被带出 models/。
    REQUIRE(!collocation::StartDownload("not-in-catalog"));
    REQUIRE(!collocation::DeleteModel("not-in-catalog"));
    REQUIRE(!collocation::DeleteModel("../../escape"));
    // 已就绪的包不重下：幂等返回 true 且不启动下载线程（磁盘检查先于任何 I/O）。
    SeedModelFile(env.models_dir(), L"zh-moqi");
    REQUIRE(collocation::StartDownload("zh-moqi"));
}

TEST_CASE(collocation_activation_write_and_delete_guard)
{
    ScopedCollocationEnvironment env;
    // 激活写入：setter 落盘并同步全局。空串 = 未选择任何模型（没有默认/回退包）。
    REQUIRE(SetConfiguredAssocSentenceCollocationModel("zh-moqi"));
    REQUIRE_EQ(GetConfiguredAssocSentenceCollocationModel(), std::string("zh-moqi"));
    REQUIRE(SetConfiguredAssocSentenceCollocationModel(""));
    REQUIRE(GetConfiguredAssocSentenceCollocationModel().empty());

    // 未选择时任何包都可删：目录条目没有受保护的目标。
    SeedModelFile(env.models_dir(), L"wanxiang-lts-zh-hans");
    REQUIRE(collocation::DeleteModel(collocation::kRecommendedModelId));
    REQUIRE(!std::filesystem::exists(env.models_dir() / L"wanxiang-lts-zh-hans"));

    // 删除当前激活的包：文件与激活一并清掉——配置指向不存在的包只会留下僵尸选中态。
    SeedModelFile(env.models_dir(), L"zh-moqi");
    REQUIRE(SetConfiguredAssocSentenceCollocationModel("zh-moqi"));
    REQUIRE(collocation::DeleteModel("zh-moqi"));
    REQUIRE(!std::filesystem::exists(env.models_dir() / L"zh-moqi"));
    REQUIRE(GetConfiguredAssocSentenceCollocationModel().empty());

    // 删除非激活的包不动激活值：显式激活 zh-moqi 后删推荐包，激活保持不变。
    SeedModelFile(env.models_dir(), L"zh-moqi");
    SeedModelFile(env.models_dir(), L"wanxiang-lts-zh-hans");
    REQUIRE(SetConfiguredAssocSentenceCollocationModel("zh-moqi"));
    REQUIRE(collocation::DeleteModel(collocation::kRecommendedModelId));
    REQUIRE_EQ(GetConfiguredAssocSentenceCollocationModel(), std::string("zh-moqi"));
    REQUIRE(std::filesystem::exists(env.models_dir() / L"zh-moqi" / L"zh-moqi.gram"));
    // 还原激活状态，不把测试残留带给同进程的后续用例。
    REQUIRE(SetConfiguredAssocSentenceCollocationModel(""));
}

TEST_CASE(collocation_path_resolution_treats_empty_as_unselected)
{
    ScopedCollocationEnvironment env;
    // 激活值留空 = 未选择任何模型：即使推荐包就在盘上也不回退，解析返回空串，
    // 调用方按整句加成全关处理。
    SeedModelFile(env.models_dir(), L"wanxiang-lts-zh-hans");
    REQUIRE(ResolveCollocationModelPath("").empty());
    // 显式 id 只解析自己的文件，缺席仍返回空。
    REQUIRE(ResolveCollocationModelPath("zh-moqi").empty());
    REQUIRE(ResolveCollocationModelPath(collocation::kRecommendedModelId).find("wanxiang-lts-zh-hans") !=
            std::string::npos);
}

TEST_CASE(collocation_source_url_is_https_direct_link)
{
    // 设置页的「浏览器下载」与 NOTICE.md 的来源署名用同一个地址。
    const auto &entry = collocation::Catalog()[0];
    REQUIRE_EQ(collocation::SourceUrl(entry),
               std::string("https://github.com/amzxyz/RIME-LMDG/releases/download/LTS/wanxiang-lts-zh-hans.gram"));
}

TEST_CASE(collocation_import_rejects_bad_ids_and_sources_and_validates_format)
{
    ScopedCollocationEnvironment env;
    const std::filesystem::path source = env.models_dir().parent_path() / L"浏览器下载.gram";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(output));
        output << "this is not an octagram model";
    }
    // 守卫与下载同一入口：目录外 id、缺失的源文件都同步拒绝，不起线程。
    REQUIRE(!collocation::ImportModel("not-in-catalog", source));
    REQUIRE(!collocation::ImportModel("zh-moqi", env.models_dir().parent_path() / L"missing.gram"));

    // 格式锁对导入同样生效：坏字节进 error 态，模型目录里既不留 .gram 也不留 .part。
    REQUIRE(collocation::ImportModel("zh-moqi", source));
    std::string state;
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        state = collocation::GetModelStatuses()["zh-moqi"].state;
        if (state != "importing")
            break;
        Sleep(25);
    }
    REQUIRE_EQ(state, std::string("error"));
    REQUIRE(!std::filesystem::exists(env.models_dir() / L"zh-moqi" / L"zh-moqi.gram"));
    REQUIRE(!std::filesystem::exists(env.models_dir() / L"zh-moqi" / L"zh-moqi.gram.part"));
    // 用户选的原文件不被挪走。
    REQUIRE(std::filesystem::exists(source));

    // 已就绪的包不重做：幂等返回 true，原文件保持不变。
    SeedModelFile(env.models_dir(), L"wanxiang-lts-zh-hans");
    REQUIRE(collocation::ImportModel(collocation::kRecommendedModelId, source));
    REQUIRE_EQ(std::filesystem::file_size(env.models_dir() / L"wanxiang-lts-zh-hans" / L"wanxiang-lts-zh-hans.gram"),
               static_cast<std::uintmax_t>(4));
}

TEST_CASE(collocation_remote_identity_verdicts_follow_etag_modified_length)
{
    const collocation::RemoteIdentity installed{"\"0x8DF2D93621D626A\"", "Mon, 19 Oct 2026 03:45:13 GMT", 398497836LL};
    collocation::RemoteIdentity remote = installed;
    REQUIRE(collocation::CompareRemoteIdentity(installed, 398497836LL, remote) == collocation::UpdateVerdict::Current);

    // 三个判据任一变化都算上游重传过。
    remote.etag = "\"0xOTHER\"";
    REQUIRE(collocation::CompareRemoteIdentity(installed, 398497836LL, remote) == collocation::UpdateVerdict::Update);
    remote = installed;
    remote.last_modified = "Tue, 20 Oct 2026 03:45:13 GMT";
    REQUIRE(collocation::CompareRemoteIdentity(installed, 398497836LL, remote) == collocation::UpdateVerdict::Update);
    remote = installed;
    remote.length = 398497837LL;
    REQUIRE(collocation::CompareRemoteIdentity(installed, 398497836LL, remote) == collocation::UpdateVerdict::Update);

    // 旧安装没有 .remote：长度相等按最新处理（同时补记录），不等才提示更新。
    const collocation::RemoteIdentity empty;
    REQUIRE(collocation::CompareRemoteIdentity(empty, 398497836LL, installed) == collocation::UpdateVerdict::Current);
    REQUIRE(collocation::CompareRemoteIdentity(empty, 398497835LL, installed) == collocation::UpdateVerdict::Update);
    // 远端一个信号都没有（离线 / 代理剥掉头）：不误报。
    REQUIRE(collocation::CompareRemoteIdentity(installed, 398497836LL, empty) == collocation::UpdateVerdict::Unknown);
    REQUIRE(collocation::CompareRemoteIdentity(empty, 398497836LL, empty) == collocation::UpdateVerdict::Unknown);
}

TEST_CASE(collocation_remote_identity_tolerates_missing_and_malformed_files)
{
    ScopedCollocationEnvironment env;
    const std::filesystem::path directory = env.models_dir() / L"wanxiang-lts-zh-hans";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    REQUIRE(!ec);

    // 文件不存在：空结构，不抛异常。
    const collocation::RemoteIdentity missing = collocation::ReadRemoteIdentity(directory);
    REQUIRE(missing.etag.empty());
    REQUIRE(missing.last_modified.empty());
    REQUIRE_EQ(missing.length, static_cast<long long>(0));

    const collocation::RemoteIdentity identity{"\"0x8DF2D93621D626A\"", "Mon, 19 Oct 2026 03:45:13 GMT", 398497836LL};
    REQUIRE(collocation::WriteRemoteIdentity(directory, identity));
    const collocation::RemoteIdentity read = collocation::ReadRemoteIdentity(directory);
    REQUIRE_EQ(read.etag, identity.etag);
    REQUIRE_EQ(read.last_modified, identity.last_modified);
    REQUIRE_EQ(read.length, identity.length);

    // CRLF、空值、未知键、坏长度都容错：能读的读，坏的当未知。
    WriteTextFile(directory / L".remote", "etag=\r\nlast-modified=Tue, 20 Oct 2026 00:00:00 GMT\r\n"
                                          "length=not-a-number\r\nfuture-key=whatever\r\n\r\n");
    const collocation::RemoteIdentity tolerant = collocation::ReadRemoteIdentity(directory);
    REQUIRE(tolerant.etag.empty());
    REQUIRE_EQ(tolerant.last_modified, std::string("Tue, 20 Oct 2026 00:00:00 GMT"));
    REQUIRE_EQ(tolerant.length, static_cast<long long>(0));
}

TEST_CASE(collocation_apply_pending_updates_swaps_model_and_remote_identity)
{
    ScopedCollocationEnvironment env;
    const std::wstring id = L"wanxiang-lts-zh-hans";
    const std::filesystem::path directory = env.models_dir() / id;
    SeedModelFile(env.models_dir(), id);                                           // 旧模型，4 字节
    WriteTextFile(directory / L"wanxiang-lts-zh-hans.gram.new", "new-gram-model"); // 14 字节
    REQUIRE(collocation::WriteRemoteIdentity(directory, {"\"old\"", "old-time", 4}));
    WriteTextFile(directory / L".remote.new", "etag=\"new\"\nlast-modified=new-time\nlength=14\n");

    collocation::ApplyPendingUpdates();

    REQUIRE(!std::filesystem::exists(directory / L"wanxiang-lts-zh-hans.gram.new"));
    REQUIRE(!std::filesystem::exists(directory / L".remote.new"));
    REQUIRE_EQ(std::filesystem::file_size(directory / L"wanxiang-lts-zh-hans.gram"), static_cast<std::uintmax_t>(14));
    const collocation::RemoteIdentity applied = collocation::ReadRemoteIdentity(directory);
    REQUIRE_EQ(applied.etag, std::string("\"new\""));
    REQUIRE_EQ(applied.last_modified, std::string("new-time"));
    REQUIRE_EQ(applied.length, static_cast<long long>(14));

    // 只有 .gram.new、没有旧 .gram：直接落位；没有 .remote.new 时删掉旧记录，
    // 下一次探测按「旧安装」口径用长度补记录。
    std::filesystem::remove(directory / L"wanxiang-lts-zh-hans.gram");
    WriteTextFile(directory / L"wanxiang-lts-zh-hans.gram.new", "fresh");
    collocation::ApplyPendingUpdates();
    REQUIRE_EQ(std::filesystem::file_size(directory / L"wanxiang-lts-zh-hans.gram"), static_cast<std::uintmax_t>(5));
    REQUIRE(!std::filesystem::exists(directory / L".remote"));
}

TEST_CASE(collocation_status_exposes_pending_update_without_reporting_available)
{
    ScopedCollocationEnvironment env;
    const std::wstring id = L"wanxiang-lts-zh-hans";
    SeedModelFile(env.models_dir(), id);
    auto statuses = collocation::GetModelStatuses();
    REQUIRE_EQ(statuses["wanxiang-lts-zh-hans"].state, std::string("ready"));
    REQUIRE(!statuses["wanxiang-lts-zh-hans"].update_available);
    REQUIRE(!statuses["wanxiang-lts-zh-hans"].update_pending);

    // .gram.new 存在即「已下载、等重启」，不再同时提示可更新。
    WriteTextFile(env.models_dir() / id / L"wanxiang-lts-zh-hans.gram.new", "pending");
    statuses = collocation::GetModelStatuses();
    REQUIRE_EQ(statuses["wanxiang-lts-zh-hans"].state, std::string("ready"));
    REQUIRE(!statuses["wanxiang-lts-zh-hans"].update_available);
    REQUIRE(statuses["wanxiang-lts-zh-hans"].update_pending);
}

TEST_CASE(collocation_delete_removes_update_sidecars)
{
    ScopedCollocationEnvironment env;
    const std::wstring id = L"wanxiang-lts-zh-hans";
    const std::filesystem::path directory = env.models_dir() / id;
    SeedModelFile(env.models_dir(), id);
    WriteTextFile(directory / L"wanxiang-lts-zh-hans.gram.new", "pending");
    REQUIRE(collocation::WriteRemoteIdentity(directory, {"\"etag\"", "time", 4}));
    WriteTextFile(directory / L".remote.new", "etag=\"next\"\n");

    REQUIRE(collocation::DeleteModel(collocation::kRecommendedModelId));
    REQUIRE(!std::filesystem::exists(directory / L"wanxiang-lts-zh-hans.gram"));
    REQUIRE(!std::filesystem::exists(directory / L"wanxiang-lts-zh-hans.gram.new"));
    REQUIRE(!std::filesystem::exists(directory / L".remote"));
    REQUIRE(!std::filesystem::exists(directory / L".remote.new"));
    auto statuses = collocation::GetModelStatuses();
    REQUIRE_EQ(statuses["wanxiang-lts-zh-hans"].state, std::string("absent"));
    REQUIRE(!statuses["wanxiang-lts-zh-hans"].update_pending);
}

TEST_CASE(collocation_apply_pending_updates_keeps_both_files_while_old_model_is_mapped)
{
    ScopedCollocationEnvironment env;
    const std::wstring id = L"wanxiang-lts-zh-hans";
    const std::filesystem::path directory = env.models_dir() / id;
    const std::filesystem::path target = directory / L"wanxiang-lts-zh-hans.gram";
    const std::filesystem::path pending = directory / L"wanxiang-lts-zh-hans.gram.new";
    SeedModelFile(env.models_dir(), id);      // 旧模型，4 字节
    WriteTextFile(pending, "new-gram-model"); // 新版本，14 字节

    // 模拟输入法进程的映射：引擎只用 FILE_SHARE_READ 打开 .gram（没有 FILE_SHARE_DELETE），
    // 这正是「待重启」存在的理由。
    HANDLE mapped = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(mapped != INVALID_HANDLE_VALUE);
    collocation::ApplyPendingUpdates();
    // 先读结果再断言：断言抛异常时句柄必须已经关上，否则临时目录删不掉、后面的用例跟着挂。
    std::error_code size_error;
    const std::uintmax_t locked_bytes = std::filesystem::file_size(target, size_error);
    const bool locked_pending_exists = std::filesystem::exists(pending);
    CloseHandle(mapped);

    // 落不了位不是错误：旧模型一个字节都没动，新文件留着等下次启动。不允许「先删旧的再改名」
    // 那样中间留下没有模型的窗口。
    REQUIRE(!size_error);
    REQUIRE_EQ(locked_bytes, static_cast<std::uintmax_t>(4));
    REQUIRE(locked_pending_exists);

    // 映射释放后，下一次启动的 ApplyPendingUpdates 收敛到新版本。
    collocation::ApplyPendingUpdates();
    REQUIRE_EQ(std::filesystem::file_size(target), static_cast<std::uintmax_t>(14));
    REQUIRE(!std::filesystem::exists(pending));
    auto statuses = collocation::GetModelStatuses();
    REQUIRE_EQ(statuses["wanxiang-lts-zh-hans"].state, std::string("ready"));
    REQUIRE(!statuses["wanxiang-lts-zh-hans"].update_pending);
}

TEST_CASE(collocation_apply_pending_updates_skips_logically_deleted_model)
{
    ScopedCollocationEnvironment env;
    const std::wstring id = L"wanxiang-lts-zh-hans";
    const std::filesystem::path directory = env.models_dir() / id;
    SeedModelFile(env.models_dir(), id);
    WriteTextFile(directory / L"wanxiang-lts-zh-hans.gram.new", "new-gram-model");
    // .trash 在、旧 .gram 还没能物理删除（映射未释放）：删除意愿优先，待更新不得抢先落位。
    WriteTextFile(directory / L".trash", "");

    collocation::ApplyPendingUpdates();

    REQUIRE_EQ(std::filesystem::file_size(directory / L"wanxiang-lts-zh-hans.gram"), static_cast<std::uintmax_t>(4));
    REQUIRE(std::filesystem::exists(directory / L"wanxiang-lts-zh-hans.gram.new"));
}

TEST_CASE(collocation_import_does_not_fabricate_remote_identity)
{
    ScopedCollocationEnvironment env;
    const std::wstring id = L"wanxiang-lts-zh-hans";
    const std::filesystem::path directory = env.models_dir() / id;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    REQUIRE(!ec);
    // 目录里残留的旧身份（例如上次安装的记录）不能被当成导入文件的身份。
    REQUIRE(collocation::WriteRemoteIdentity(directory, {"\"stale\"", "time", 4}));

    const std::filesystem::path source = env.models_dir().parent_path() / L"imported.gram";
    WriteMinimalGramFile(source);
    REQUIRE(collocation::ImportModel(collocation::kRecommendedModelId, source));
    std::string state;
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        state = collocation::GetModelStatuses()["wanxiang-lts-zh-hans"].state;
        if (state != "importing")
            break;
        Sleep(25);
    }
    REQUIRE_EQ(state, std::string("ready"));
    // 导入没有响应头：落位后删掉旧记录，下一次探测按「旧安装」口径用长度补记录或提示更新。
    REQUIRE(!std::filesystem::exists(directory / L".remote"));
}
