#pragma once

// 匿名使用统计的本地状态与待发队列。协议与 msime 仓其他平台相同：
// POST https://api.msime.app/v1/telemetry/events，请求体 {id, kind, platform, version, install_id}，服务端按 id 去重。
//
// 本版本只发 active：每个安装每个 UTC 日最多一条，id 固定为 active-<install_id>-<yyyymmdd>。
// 所以同一天重复发送、失败后重试都只记一次。
// install_id 是第一次使用时用系统随机数生成的 32 位十六进制串，不由硬件、账号、用户名或任何输入数据派生。
//
// 这里只做文件读写和发送决策，不联网：HTTP 请求由调用方传入的 Sender 完成，测试因此不需要服务端。
// 发送结果的处理与其他平台一致：202 移出队列；400 类拒绝直接丢弃；429、503 按 Retry-After 延后。
// Retry-After 缺省 60 秒，最多 24 小时。其余失败和断网留在队列里，下次用同一个 id 重发。
// 队列最多 64 条，超出时丢最旧的。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace usage_reporting
{
inline constexpr char kPlatform[] = "windows";
inline constexpr char kActiveKind[] = "active";
inline constexpr char kQueueFileName[] = "telemetry.json";
inline constexpr char kStateFileName[] = "telemetry-state.json";
inline constexpr std::size_t kMaxQueuedEvents = 64;
inline constexpr std::chrono::seconds kDefaultRetryAfter{60};
inline constexpr std::chrono::seconds kMaxRetryAfter{24 * 60 * 60};

struct Event
{
    std::string id;
    std::string kind;
    std::string platform;
    std::string version;
    std::string install_id;

    bool operator==(const Event &other) const
    {
        return id == other.id && kind == other.kind && platform == other.platform && version == other.version &&
               install_id == other.install_id;
    }
};

enum class Delivery
{
    // 服务端收下了（202，也包括它早已有的 id）。
    Accepted,
    // 服务端拒绝事件本身（400 类），重发也一样会被拒，直接丢弃。
    Rejected,
    // 限流或服务不可用：保留事件，至少等 retry_after 再发。
    RetryAfter,
    // 没有应答或应答不认识：保留事件，下次再发。
    Failed,
};

struct SendResult
{
    Delivery delivery = Delivery::Failed;
    std::optional<std::chrono::seconds> retry_after;
};

// 发送一条已经序列化好的事件。生产实现走 WinHTTP，见 usage_reporting.cpp。
using Sender = std::function<SendResult(const std::string &body)>;

struct FlushReport
{
    std::size_t sent = 0;
    std::size_t dropped = 0;
    std::size_t remaining = 0;
    // 上一次的 Retry-After 还没到期，本次什么都没发。
    bool deferred = false;
};

// HTTP 状态码到发送结果的映射，与其他平台的共享实现一致。
SendResult ClassifyStatus(unsigned status, std::optional<std::chrono::seconds> retry_after);

// 解析 Retry-After 的秒数形式；HTTP 日期形式和其他写法返回空，由调用方取缺省值。
std::optional<std::chrono::seconds> ParseRetryAfter(std::string_view value);

// 服务端对 install_id 的规则：16 到 64 个 [A-Za-z0-9_-]。
bool IsValidInstallId(std::string_view value);

// 服务端会接受的事件：本版本只认 active，字段都是单行可打印 ASCII 且长度在服务端限制内。
bool IsValidEvent(const Event &event);

// UTC 日期 yyyymmdd。
std::string UtcDay(std::chrono::system_clock::time_point now);

std::string SerializeEvent(const Event &event);

// 一个目录里的统计文件：
// - telemetry-state.json：install_id、最近一次排入 active 的日期、Retry-After 截止时间；
// - telemetry.json：待发队列。
// 读改写都在目录里 telemetry.lock 的独占锁内完成，同一用户的多个 Server 进程不会互相覆盖；发送请求时不持锁。
class Store
{
  public:
    explicit Store(std::filesystem::path directory);

    const std::filesystem::path &directory() const
    {
        return directory_;
    }

    // 匿名安装 id，第一次调用时生成并保存。目录写不进去或系统随机数不可用时返回空。
    std::optional<std::string> InstallId();

    // 排入今天的 active，今天已经排过就什么都不做。只读写本地文件，不联网。
    // 本地状态写不进去时返回 false。
    bool RecordActive(const std::string &version, std::chrono::system_clock::time_point now);

    // 按先后顺序发送队列，遇到服务端现在收不下的事件就停。
    // still_enabled 在每次发送前检查，用户中途关掉统计后不再发出任何请求。
    // 会阻塞在网络上，只能在后台线程调用。
    FlushReport Flush(const Sender &send, std::chrono::system_clock::time_point now,
                      const std::function<bool()> &still_enabled);

    // 用户关掉统计：删掉队列，清掉当天记录和 Retry-After。
    // install_id 保留，重新打开时同一个安装不会被算成两个。目录不存在时什么都不创建。
    bool Clear();

    // 队列里的事件，按先后顺序。
    std::vector<Event> Queued();

  private:
    std::filesystem::path directory_;
};
} // namespace usage_reporting
