#include "telemetry/usage_reporting_store.h"

#include <Windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>

namespace usage_reporting
{
namespace
{
constexpr wchar_t kLockFileName[] = L"telemetry.lock";
constexpr std::uintmax_t kMaxQueueBytes = 1 << 20;
constexpr std::uintmax_t kMaxStateBytes = 16 * 1024;

struct State
{
    std::string install_id;
    // 最近一次排入 active 的 UTC 日期，yyyymmdd。
    std::string active_day;
    std::uint64_t retry_after_unix_ms = 0;
};

std::uint64_t UnixMs(std::chrono::system_clock::time_point now)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return ms > 0 ? static_cast<std::uint64_t>(ms) : 0;
}

// 单行可打印 ASCII，长度在 [minimum, maximum] 之内。
// 服务端按 Unicode 标量计长度，这里只放行 ASCII，两种算法结果一致。
bool IsSingleLineAscii(std::string_view value, std::size_t minimum, std::size_t maximum)
{
    if (value.size() < minimum || value.size() > maximum)
        return false;
    if (std::all_of(value.begin(), value.end(), [](char ch) { return ch == ' '; }))
        return false;
    return std::all_of(value.begin(), value.end(), [](char ch) { return ch >= 0x20 && ch <= 0x7e; });
}

// 独占 telemetry.lock：同一用户可能同时有几个会话各跑一个 Server，它们共用这个目录。
// 持锁期间只做本地文件读写。
class DirectoryLock
{
  public:
    explicit DirectoryLock(const std::filesystem::path &directory)
    {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        handle_ = CreateFileW((directory / kLockFileName).c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            return;
        OVERLAPPED overlapped{};
        locked_ = LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped) != FALSE;
    }

    ~DirectoryLock()
    {
        if (locked_)
        {
            OVERLAPPED overlapped{};
            UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
        }
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
    }

    DirectoryLock(const DirectoryLock &) = delete;
    DirectoryLock &operator=(const DirectoryLock &) = delete;

    explicit operator bool() const
    {
        return locked_;
    }

  private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool locked_ = false;
};

std::optional<std::string> ReadBounded(const std::filesystem::path &path, std::uintmax_t limit)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return std::nullopt;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > limit)
        return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

// 先写临时文件再整体替换，读的一方不会看到半截内容。
bool WriteAtomically(const std::filesystem::path &path, const std::string &text)
{
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output)
            return false;
    }
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    return false;
}

bool RemoveIfPresent(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
}

std::string StringField(const nlohmann::json &object, const char *name)
{
    const auto found = object.find(name);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// 读不出来、格式不对都按空状态处理：install_id 随之重新生成，不会因为一份坏文件让统计永远停住。
State ReadState(const std::filesystem::path &directory)
{
    State state;
    const auto text = ReadBounded(directory / kStateFileName, kMaxStateBytes);
    if (!text)
        return state;
    const nlohmann::json document = nlohmann::json::parse(*text, nullptr, false);
    if (!document.is_object())
        return state;
    state.install_id = StringField(document, "install_id");
    state.active_day = StringField(document, "active_day");
    const auto retry = document.find("retry_after_unix_ms");
    if (retry != document.end() && retry->is_number_unsigned())
        state.retry_after_unix_ms = retry->get<std::uint64_t>();
    return state;
}

bool WriteState(const std::filesystem::path &directory, const State &state)
{
    const nlohmann::json document = {{"install_id", state.install_id},
                                     {"active_day", state.active_day},
                                     {"retry_after_unix_ms", state.retry_after_unix_ms}};
    return WriteAtomically(directory / kStateFileName, document.dump());
}

nlohmann::json EventJson(const Event &event)
{
    return {{"id", event.id},
            {"kind", event.kind},
            {"platform", event.platform},
            {"version", event.version},
            {"install_id", event.install_id}};
}

void Push(std::vector<Event> &queue, Event event)
{
    queue.erase(std::remove_if(queue.begin(), queue.end(), [&](const Event &queued) { return queued.id == event.id; }),
                queue.end());
    queue.push_back(std::move(event));
    if (queue.size() > kMaxQueuedEvents)
        queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(queue.size() - kMaxQueuedEvents));
}

// 只留下服务端会接受的事件：解析不了或校验不过的直接丢掉，免得一条坏数据永远堵在队首。
std::vector<Event> ReadQueue(const std::filesystem::path &directory)
{
    std::vector<Event> queue;
    const auto text = ReadBounded(directory / kQueueFileName, kMaxQueueBytes);
    if (!text)
        return queue;
    const nlohmann::json document = nlohmann::json::parse(*text, nullptr, false);
    if (!document.is_array())
        return queue;
    for (const auto &item : document)
    {
        if (!item.is_object())
            continue;
        Event event{StringField(item, "id"), StringField(item, "kind"), StringField(item, "platform"),
                    StringField(item, "version"), StringField(item, "install_id")};
        if (IsValidEvent(event))
            Push(queue, std::move(event));
    }
    return queue;
}

bool WriteQueue(const std::filesystem::path &directory, const std::vector<Event> &queue)
{
    if (queue.empty())
        return RemoveIfPresent(directory / kQueueFileName);
    nlohmann::json document = nlohmann::json::array();
    for (const auto &event : queue)
        document.push_back(EventJson(event));
    return WriteAtomically(directory / kQueueFileName, document.dump());
}

std::optional<std::string> NewInstallId()
{
    unsigned char bytes[16]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return std::nullopt;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string id;
    id.reserve(sizeof(bytes) * 2);
    for (const unsigned char byte : bytes)
    {
        id.push_back(kHex[byte >> 4]);
        id.push_back(kHex[byte & 0x0f]);
    }
    return id;
}

// 已有合法的 install_id 就沿用。没有或被改坏了就重新生成，并清掉当天记录，让新 id 当天也能记上一条。
std::optional<std::string> EnsureInstallId(const std::filesystem::path &directory, State &state)
{
    if (IsValidInstallId(state.install_id))
        return state.install_id;
    auto id = NewInstallId();
    if (!id)
        return std::nullopt;
    state.install_id = *id;
    state.active_day.clear();
    if (!WriteState(directory, state))
        return std::nullopt;
    return id;
}
} // namespace

SendResult ClassifyStatus(unsigned status, std::optional<std::chrono::seconds> retry_after)
{
    if (status >= 200 && status <= 299)
        return {Delivery::Accepted, std::nullopt};
    if (status == 400 || status == 413 || status == 415 || status == 422)
        return {Delivery::Rejected, std::nullopt};
    if (status == 429 || status == 503)
        return {Delivery::RetryAfter, retry_after};
    return {Delivery::Failed, std::nullopt};
}

std::optional<std::chrono::seconds> ParseRetryAfter(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    if (value.empty() || value.size() > 10)
        return std::nullopt;
    std::uint64_t seconds = 0;
    for (const char ch : value)
    {
        if (ch < '0' || ch > '9')
            return std::nullopt;
        seconds = seconds * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    return std::chrono::seconds(static_cast<std::chrono::seconds::rep>(seconds));
}

bool IsValidInstallId(std::string_view value)
{
    return value.size() >= 16 && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                      ch == '-';
           });
}

bool IsValidEvent(const Event &event)
{
    return IsSingleLineAscii(event.id, 16, 128) && event.kind == kActiveKind && event.platform == kPlatform &&
           IsSingleLineAscii(event.version, 1, 64) && IsValidInstallId(event.install_id);
}

std::string UtcDay(std::chrono::system_clock::time_point now)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    if (gmtime_s(&utc, &seconds) != 0)
        return {};
    char buffer[16]{};
    const std::size_t length = std::strftime(buffer, sizeof(buffer), "%Y%m%d", &utc);
    return std::string(buffer, length);
}

std::string SerializeEvent(const Event &event)
{
    return EventJson(event).dump();
}

Store::Store(std::filesystem::path directory) : directory_(std::move(directory))
{
}

std::optional<std::string> Store::InstallId()
{
    DirectoryLock lock(directory_);
    if (!lock)
        return std::nullopt;
    State state = ReadState(directory_);
    return EnsureInstallId(directory_, state);
}

bool Store::RecordActive(const std::string &version, std::chrono::system_clock::time_point now)
{
    DirectoryLock lock(directory_);
    if (!lock)
        return false;
    State state = ReadState(directory_);
    const auto install_id = EnsureInstallId(directory_, state);
    if (!install_id)
        return false;
    const std::string day = UtcDay(now);
    if (day.empty())
        return false;
    if (state.active_day == day)
        return true;
    Event event{"active-" + *install_id + "-" + day, kActiveKind, kPlatform, version, *install_id};
    if (!IsValidEvent(event))
        return false;
    std::vector<Event> queue = ReadQueue(directory_);
    Push(queue, std::move(event));
    if (!WriteQueue(directory_, queue))
        return false;
    state.active_day = day;
    return WriteState(directory_, state);
}

FlushReport Store::Flush(const Sender &send, std::chrono::system_clock::time_point now,
                         const std::function<bool()> &still_enabled)
{
    FlushReport report;
    const std::uint64_t now_ms = UnixMs(now);
    const std::uint64_t max_retry_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(kMaxRetryAfter).count());
    std::vector<Event> pending;
    std::uint64_t observed_deadline = 0;
    {
        DirectoryLock lock(directory_);
        if (!lock)
            return report;
        const State state = ReadState(directory_);
        observed_deadline = state.retry_after_unix_ms;
        pending = ReadQueue(directory_);
        // 远于最大退避窗口的截止时间只可能来自被调回的时钟或坏状态，不能让它一直挡住发送。
        if (state.retry_after_unix_ms > now_ms && state.retry_after_unix_ms - now_ms <= max_retry_ms)
        {
            report.deferred = true;
            report.remaining = pending.size();
            return report;
        }
    }

    std::vector<std::string> done;
    std::optional<std::chrono::seconds> retry_after;
    for (const auto &event : pending)
    {
        if (still_enabled && !still_enabled())
            break;
        const SendResult result = send(SerializeEvent(event));
        if (result.delivery == Delivery::Accepted)
        {
            ++report.sent;
            done.push_back(event.id);
        }
        else if (result.delivery == Delivery::Rejected)
        {
            ++report.dropped;
            done.push_back(event.id);
        }
        else
        {
            if (result.delivery == Delivery::RetryAfter)
                retry_after = (std::min)(result.retry_after.value_or(kDefaultRetryAfter), kMaxRetryAfter);
            break;
        }
    }

    DirectoryLock lock(directory_);
    if (!lock)
        return report;
    // 请求在途时别的进程可能排入了新事件，重新读一遍再删掉这次处理完的。
    std::vector<Event> queue = ReadQueue(directory_);
    if (!done.empty())
    {
        queue.erase(std::remove_if(queue.begin(), queue.end(),
                                   [&](const Event &event) {
                                       return std::find(done.begin(), done.end(), event.id) != done.end();
                                   }),
                    queue.end());
        WriteQueue(directory_, queue);
    }
    report.remaining = queue.size();
    State state = ReadState(directory_);
    // 请求在途时另一个进程记下的更晚截止时间要保留，不能被这次较早的结果覆盖。
    std::uint64_t deadline = state.retry_after_unix_ms != observed_deadline && state.retry_after_unix_ms > now_ms
                                 ? state.retry_after_unix_ms
                                 : std::uint64_t{0};
    if (retry_after)
        deadline = (std::max)(deadline, now_ms + static_cast<std::uint64_t>(retry_after->count()) * 1000);
    if (state.retry_after_unix_ms != deadline)
    {
        state.retry_after_unix_ms = deadline;
        WriteState(directory_, state);
    }
    return report;
}

bool Store::Clear()
{
    std::error_code ec;
    if (!std::filesystem::is_directory(directory_, ec))
        return true;
    DirectoryLock lock(directory_);
    if (!lock)
        return false;
    bool ok = RemoveIfPresent(directory_ / kQueueFileName);
    State state = ReadState(directory_);
    if (!state.active_day.empty() || state.retry_after_unix_ms != 0)
    {
        state.active_day.clear();
        state.retry_after_unix_ms = 0;
        ok = WriteState(directory_, state) && ok;
    }
    return ok;
}

std::vector<Event> Store::Queued()
{
    DirectoryLock lock(directory_);
    if (!lock)
        return {};
    return ReadQueue(directory_);
}
} // namespace usage_reporting
