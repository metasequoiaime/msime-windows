// 匿名使用统计：本地状态、active 的去重规则、发送结果的处理，以及 usage_reporting 开关的读取和升级保留。
// 发送用假的 Sender，不联网。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "config/ime_config.h"
#include "telemetry/usage_reporting_store.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using usage_reporting::Delivery;
using usage_reporting::Event;
using usage_reporting::SendResult;
using usage_reporting::Store;
using Clock = std::chrono::system_clock;

// 2026-10-24 12:00:00 UTC。
const Clock::time_point kNoon = Clock::from_time_t(1792843200);

class TempDirectory
{
  public:
    explicit TempDirectory(const wchar_t *name)
        : path_(test::TempRoot() / (std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId())))
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ~TempDirectory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDirectory(const TempDirectory &) = delete;
    TempDirectory &operator=(const TempDirectory &) = delete;

    const std::filesystem::path &path() const
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

struct RecordingSender
{
    std::vector<std::string> bodies;
    std::vector<SendResult> results;

    usage_reporting::Sender sender()
    {
        return [this](const std::string &body) {
            const std::size_t index = bodies.size();
            bodies.push_back(body);
            return index < results.size() ? results[index] : SendResult{Delivery::Accepted, std::nullopt};
        };
    }
};

bool AlwaysEnabled()
{
    return true;
}

void WriteText(const std::filesystem::path &path, const std::string &text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(output));
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string ReadText(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

class ScopedEnv
{
  public:
    ScopedEnv(const wchar_t *name, const std::wstring &value) : name_(name)
    {
        wchar_t buffer[32768];
        const DWORD length = GetEnvironmentVariableW(name, buffer, 32768);
        had_previous_ = length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        previous_.assign(buffer, length);
        SetEnvironmentVariableW(name, value.c_str());
    }
    ~ScopedEnv()
    {
        SetEnvironmentVariableW(name_.c_str(), had_previous_ ? previous_.c_str() : nullptr);
    }
    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;

  private:
    std::wstring name_;
    std::wstring previous_;
    bool had_previous_ = false;
};
} // namespace

TEST_CASE(usage_reporting_install_id_is_random_persistent_and_private)
{
    TempDirectory first(L"msime-usage-a");
    TempDirectory second(L"msime-usage-b");
    Store store(first.path());
    const auto id = store.InstallId();
    REQUIRE(id.has_value());
    REQUIRE_EQ(id->size(), std::size_t{32});
    REQUIRE(usage_reporting::IsValidInstallId(*id));
    // 只是系统随机数的十六进制，不含任何用户、账号或硬件信息。
    REQUIRE(id->find_first_not_of("0123456789abcdef") == std::string::npos);
    // 同一目录里保持不变，另一个安装得到另一个随机值。
    REQUIRE_EQ(Store(first.path()).InstallId(), id);
    const auto other = Store(second.path()).InstallId();
    REQUIRE(other.has_value());
    REQUIRE(*other != *id);
}

TEST_CASE(usage_reporting_queues_one_active_per_utc_day)
{
    TempDirectory directory(L"msime-usage-day");
    Store store(directory.path());
    REQUIRE(store.RecordActive("0.11.1", kNoon));
    const auto id = store.InstallId();
    REQUIRE(id.has_value());
    auto queued = store.Queued();
    REQUIRE_EQ(queued.size(), std::size_t{1});
    REQUIRE_EQ(queued[0].id, "active-" + *id + "-20261024");
    REQUIRE_EQ(queued[0].kind, std::string("active"));
    REQUIRE_EQ(queued[0].platform, std::string("windows"));
    REQUIRE_EQ(queued[0].version, std::string("0.11.1"));
    REQUIRE_EQ(queued[0].install_id, *id);

    // 同一个 UTC 日内再记（包括重启后的新 Store）不重复排队。
    REQUIRE(store.RecordActive("0.11.1", kNoon + 11h + 59min));
    REQUIRE(Store(directory.path()).RecordActive("0.11.1", kNoon + 1h));
    REQUIRE_EQ(store.Queued().size(), std::size_t{1});

    // 跨过 UTC 零点就是新的一天。
    REQUIRE(store.RecordActive("0.11.2", kNoon + 12h));
    queued = store.Queued();
    REQUIRE_EQ(queued.size(), std::size_t{2});
    REQUIRE_EQ(queued[1].id, "active-" + *id + "-20261025");
    REQUIRE_EQ(queued[1].version, std::string("0.11.2"));
}

TEST_CASE(usage_reporting_sends_exactly_the_protocol_fields)
{
    TempDirectory directory(L"msime-usage-body");
    Store store(directory.path());
    REQUIRE(store.RecordActive("0.11.1", kNoon));
    RecordingSender sender;
    const auto report = store.Flush(sender.sender(), kNoon, AlwaysEnabled);
    REQUIRE_EQ(report.sent, std::size_t{1});
    REQUIRE_EQ(report.remaining, std::size_t{0});
    REQUIRE(store.Queued().empty());
    REQUIRE_EQ(sender.bodies.size(), std::size_t{1});

    const auto body = nlohmann::json::parse(sender.bodies[0]);
    REQUIRE(body.is_object());
    REQUIRE_EQ(body.size(), std::size_t{5});
    const auto id = store.InstallId();
    REQUIRE(id.has_value());
    REQUIRE_EQ(body.at("id").get<std::string>(), "active-" + *id + "-20261024");
    REQUIRE_EQ(body.at("kind").get<std::string>(), std::string("active"));
    REQUIRE_EQ(body.at("platform").get<std::string>(), std::string("windows"));
    REQUIRE_EQ(body.at("version").get<std::string>(), std::string("0.11.1"));
    REQUIRE_EQ(body.at("install_id").get<std::string>(), *id);

    // 已发过的当天不再发。
    REQUIRE(store.RecordActive("0.11.1", kNoon + 1h));
    REQUIRE_EQ(store.Flush(sender.sender(), kNoon + 1h, AlwaysEnabled).sent, std::size_t{0});
    REQUIRE_EQ(sender.bodies.size(), std::size_t{1});
}

TEST_CASE(usage_reporting_keeps_failed_events_and_drops_rejected_ones)
{
    TempDirectory directory(L"msime-usage-outcomes");
    Store store(directory.path());
    REQUIRE(store.RecordActive("0.11.1", kNoon));
    REQUIRE(store.RecordActive("0.11.1", kNoon + 24h));

    // 网络失败：留在队列里，后面的也不再尝试。
    RecordingSender offline;
    offline.results = {{Delivery::Failed, std::nullopt}};
    auto report = store.Flush(offline.sender(), kNoon + 24h, AlwaysEnabled);
    REQUIRE_EQ(offline.bodies.size(), std::size_t{1});
    REQUIRE_EQ(report.sent, std::size_t{0});
    REQUIRE_EQ(report.remaining, std::size_t{2});

    // 400 类拒绝丢掉，接着发下一条，同一个 id 重发。
    RecordingSender server;
    server.results = {{Delivery::Rejected, std::nullopt}, {Delivery::Accepted, std::nullopt}};
    report = store.Flush(server.sender(), kNoon + 24h, AlwaysEnabled);
    REQUIRE_EQ(report.dropped, std::size_t{1});
    REQUIRE_EQ(report.sent, std::size_t{1});
    REQUIRE(store.Queued().empty());
    REQUIRE_EQ(server.bodies[0], offline.bodies[0]);
}

TEST_CASE(usage_reporting_honours_retry_after)
{
    TempDirectory directory(L"msime-usage-retry");
    Store store(directory.path());
    REQUIRE(store.RecordActive("0.11.1", kNoon));
    RecordingSender limited;
    limited.results = {{Delivery::RetryAfter, std::chrono::seconds(120)}};
    REQUIRE_EQ(store.Flush(limited.sender(), kNoon, AlwaysEnabled).remaining, std::size_t{1});

    // 截止时间之前一条都不发，即使换一个 Store（下一次启动）也一样。
    RecordingSender early;
    const auto deferred = Store(directory.path()).Flush(early.sender(), kNoon + 119s, AlwaysEnabled);
    REQUIRE(deferred.deferred);
    REQUIRE(early.bodies.empty());

    RecordingSender later;
    REQUIRE_EQ(store.Flush(later.sender(), kNoon + 121s, AlwaysEnabled).sent, std::size_t{1});

    // 没有可用的 Retry-After 时退避 60 秒。
    REQUIRE(store.RecordActive("0.11.1", kNoon + 24h));
    RecordingSender no_header;
    no_header.results = {{Delivery::RetryAfter, std::nullopt}};
    store.Flush(no_header.sender(), kNoon + 24h, AlwaysEnabled);
    RecordingSender after_default;
    REQUIRE(store.Flush(after_default.sender(), kNoon + 24h + 59s, AlwaysEnabled).deferred);
    REQUIRE_EQ(store.Flush(after_default.sender(), kNoon + 24h + 61s, AlwaysEnabled).sent, std::size_t{1});

    // Retry-After 最多按 24 小时算。
    REQUIRE(store.RecordActive("0.11.1", kNoon + 48h));
    RecordingSender huge;
    huge.results = {{Delivery::RetryAfter, std::chrono::seconds(10 * 24 * 60 * 60)}};
    store.Flush(huge.sender(), kNoon + 48h, AlwaysEnabled);
    RecordingSender capped;
    REQUIRE(store.Flush(capped.sender(), kNoon + 72h - 1s, AlwaysEnabled).deferred);
    REQUIRE(capped.bodies.empty());
    // 时钟被调回之后，离现在超过 24 小时的截止时间只可能是旧时钟留下的，不再挡住发送。
    RecordingSender rewound;
    REQUIRE_EQ(store.Flush(rewound.sender(), kNoon + 47h, AlwaysEnabled).sent, std::size_t{1});
}

TEST_CASE(usage_reporting_sends_nothing_once_switched_off)
{
    TempDirectory directory(L"msime-usage-off");
    Store store(directory.path());
    REQUIRE(store.RecordActive("0.11.1", kNoon));
    RecordingSender sender;
    const auto report = store.Flush(sender.sender(), kNoon, [] { return false; });
    REQUIRE(sender.bodies.empty());
    REQUIRE_EQ(report.remaining, std::size_t{1});

    // 关掉：队列清空，install_id 保留；重新打开后当天的 active 用同一个 id 再排一次，由服务端去重。
    const auto id = store.InstallId();
    REQUIRE(store.Clear());
    REQUIRE(store.Queued().empty());
    REQUIRE(!std::filesystem::exists(directory.path() / usage_reporting::kQueueFileName));
    REQUIRE_EQ(store.InstallId(), id);
    REQUIRE(store.RecordActive("0.11.1", kNoon + 1h));
    const auto queued = store.Queued();
    REQUIRE_EQ(queued.size(), std::size_t{1});
    REQUIRE_EQ(queued[0].id, "active-" + *id + "-20261024");

    // 从没打开过的用户：清理不创建目录，也不生成 install_id。
    TempDirectory never(L"msime-usage-never");
    REQUIRE(Store(never.path()).Clear());
    REQUIRE(!std::filesystem::exists(never.path()));
}

TEST_CASE(usage_reporting_recovers_from_damaged_files_and_bounds_the_queue)
{
    TempDirectory directory(L"msime-usage-damaged");
    WriteText(directory.path() / usage_reporting::kStateFileName, "{\"install_id\":\"bad id\"");
    WriteText(directory.path() / usage_reporting::kQueueFileName,
              "[{\"id\":\"x\",\"kind\":\"active\"},{\"kind\":\"crash\"},42,"
              "{\"id\":\"active-0123456789abcdef0123456789abcdef-20261023\",\"kind\":\"active\","
              "\"platform\":\"windows\",\"version\":\"0.11.0\",\"install_id\":\"0123456789abcdef0123456789abcdef\"}]");
    Store store(directory.path());
    // 坏状态文件换一个新的合法 id；队列里只留下服务端会接受的那条。
    const auto id = store.InstallId();
    REQUIRE(id.has_value());
    REQUIRE(usage_reporting::IsValidInstallId(*id));
    auto queued = store.Queued();
    REQUIRE_EQ(queued.size(), std::size_t{1});
    REQUIRE_EQ(queued[0].version, std::string("0.11.0"));

    // 离线很多天：最多留 64 条，丢最旧的。
    for (int day = 0; day < 70; ++day)
        REQUIRE(store.RecordActive("0.11.1", kNoon + std::chrono::hours(24 * day)));
    queued = store.Queued();
    REQUIRE_EQ(queued.size(), usage_reporting::kMaxQueuedEvents);
    REQUIRE_EQ(queued.back().id, "active-" + *id + "-" + usage_reporting::UtcDay(kNoon + std::chrono::hours(24 * 69)));
    REQUIRE_EQ(queued.front().id, "active-" + *id + "-" + usage_reporting::UtcDay(kNoon + std::chrono::hours(24 * 6)));
}

TEST_CASE(usage_reporting_maps_http_status_like_the_other_clients)
{
    using usage_reporting::ClassifyStatus;
    using usage_reporting::ParseRetryAfter;
    REQUIRE(ClassifyStatus(202, std::nullopt).delivery == Delivery::Accepted);
    for (const unsigned status : {400u, 413u, 415u, 422u})
        REQUIRE(ClassifyStatus(status, std::nullopt).delivery == Delivery::Rejected);
    REQUIRE(ClassifyStatus(429, std::chrono::seconds(30)).delivery == Delivery::RetryAfter);
    REQUIRE_EQ(ClassifyStatus(429, std::chrono::seconds(30)).retry_after, std::optional(std::chrono::seconds(30)));
    REQUIRE(ClassifyStatus(503, std::nullopt).delivery == Delivery::RetryAfter);
    for (const unsigned status : {301u, 401u, 404u, 500u, 502u})
        REQUIRE(ClassifyStatus(status, std::nullopt).delivery == Delivery::Failed);

    REQUIRE_EQ(ParseRetryAfter("3600"), std::optional(std::chrono::seconds(3600)));
    REQUIRE_EQ(ParseRetryAfter(" 60 "), std::optional(std::chrono::seconds(60)));
    REQUIRE(!ParseRetryAfter("").has_value());
    REQUIRE(!ParseRetryAfter("-5").has_value());
    REQUIRE(!ParseRetryAfter("Wed, 21 Oct 2026 07:28:00 GMT").has_value());

    REQUIRE(usage_reporting::IsValidInstallId("abcdefABCDEF0123_-"));
    REQUIRE(!usage_reporting::IsValidInstallId("short"));
    REQUIRE(!usage_reporting::IsValidInstallId("0123456789abcdef!"));
    REQUIRE(!usage_reporting::IsValidInstallId(std::string(65, 'a')));
}

TEST_CASE(usage_reporting_switch_ships_undecided_and_upgrades_keep_the_decision)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    const auto working_path =
        std::filesystem::path(MSIME_DEFAULT_CONFIG_PATH).parent_path().parent_path().parent_path() /
        "server/assets/config/config.toml";
    const std::string working = ReadText(working_path);
    // 出厂模板不替用户做决定：写的是 "ask"，不是布尔值。安装程序据此判断要不要显示询问页。
    REQUIRE_EQ(toml::parse(installed)["general"]["usage_reporting"].value_or(std::string()), std::string("ask"));
    REQUIRE_EQ(toml::parse(working)["general"]["usage_reporting"].value_or(std::string()), std::string("ask"));

    // 升级以模板为骨架重建配置：模板里必须有这个键，否则用户的选择会在升级时丢掉。
    const std::string old_template = "[general]\ncloud_candidates = true\n";
    for (const bool decision : {false, true})
    {
        const std::string decided = old_template + "usage_reporting = " + (decision ? "true\n" : "false\n");
        for (const auto &baseline : {std::string(), old_template, installed})
        {
            const auto merged = toml::parse(MergeConfigIntoTemplate(installed, decided, baseline));
            REQUIRE_EQ(merged["general"]["usage_reporting"].value_exact<bool>(), std::optional<bool>(decision));
        }
    }
    // 从没选过的旧配置升级后仍是「还没选」，不会因为合并变成同意。
    for (const auto &undecided : {old_template, old_template + "usage_reporting = \"ask\"\n"})
    {
        for (const auto &baseline : {std::string(), old_template, installed})
        {
            const auto merged = toml::parse(MergeConfigIntoTemplate(installed, undecided, baseline));
            REQUIRE(!merged["general"]["usage_reporting"].value_exact<bool>().has_value());
        }
    }
}

TEST_CASE(usage_reporting_switch_sends_only_on_an_explicit_true)
{
    TempDirectory root(L"msime-usage-config");
    const std::filesystem::path config_dir = root.path() / L"metasequoiaime";
    std::filesystem::create_directories(config_dir);
    ScopedEnv local_app_data(L"LOCALAPPDATA", root.path().wstring());
    ScopedEnv config(L"METASEQUOIA_IME_CONFIG_DIR", config_dir.wstring());
    ScopedEnv data(L"METASEQUOIA_IME_DATA_DIR", config_dir.wstring());

    struct Case
    {
        const char *general;
        bool expected;
    };
    // 同意必须是记录下来的：缺键和 "ask" 是还没选，不是布尔值的写法读不懂，都不发送。
    const Case cases[] = {
        {"cloud_candidates = true\n", false},    {"usage_reporting = \"ask\"\n", false},
        {"usage_reporting = true\n", true},      {"usage_reporting = false\n", false},
        {"usage_reporting = \"true\"\n", false}, {"usage_reporting = 1\n", false},
    };
    for (const auto &item : cases)
    {
        WriteText(config_dir / L"config.toml", std::string("[general]\n") + item.general);
        InitImeConfig();
        REQUIRE_EQ(GetConfiguredUsageReportingEnabled(), item.expected);
    }

    // 设置页里打开或关闭都会把选择写进 [general]，立即生效。
    WriteText(config_dir / L"config.toml", "[general]\nusage_reporting = \"ask\"\n");
    InitImeConfig();
    REQUIRE(!GetConfiguredUsageReportingEnabled());
    REQUIRE(SetConfiguredUsageReportingEnabled(true));
    REQUIRE(GetConfiguredUsageReportingEnabled());
    InitImeConfig();
    REQUIRE(GetConfiguredUsageReportingEnabled());
    REQUIRE(SetConfiguredUsageReportingEnabled(false));
    REQUIRE(!GetConfiguredUsageReportingEnabled());
    InitImeConfig();
    REQUIRE(!GetConfiguredUsageReportingEnabled());
    REQUIRE_EQ(toml::parse(ReadText(config_dir / L"config.toml"))["general"]["usage_reporting"].value_exact<bool>(),
               std::optional<bool>(false));
}
