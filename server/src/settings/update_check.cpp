#include "settings/update_check.h"

#include "config/ime_config.h"
#include "ipc/ipc.h"
#include "log/candidate_diag_log.h"
#include "msime_version.h"
#include "settings/native/native_settings_update_download.h"
#include "settings/settings_launcher.h"
#include "settings/update_channel.h"
#include "utils/network_proxy.h"

#include <Windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace update_check
{
namespace
{
constexpr char kManifestUrl[] = "https://msime.app/update.json";

using Clock = std::chrono::steady_clock;

std::mutex &Mutex()
{
    static std::mutex mutex;
    return mutex;
}

// 上一轮探测结果：比本机新的版本号，没有时为空。
std::string &PendingVersionSlot()
{
    static std::string version;
    return version;
}

// 与 PendingVersionSlot 同一次探测的安装包摘要。启动安装程序前要再核对一次：
// 校验发生在下载结束，到用户点「是」之间隔着一个 UAC，数据目录又是中等完整性、
// 同机可写的，中间被换掉或截断时必须发现。
std::string &PendingSha256Slot()
{
    static std::string digest;
    return digest;
}

// 已经弹过安装提示的版本。轮询每 6 小时一轮，包一直躺在磁盘上，不去重的话
// 用户点过「否」之后还会被反复问；同一进程内同一个版本只问一次。
std::string &NotifiedVersionSlot()
{
    static std::string version;
    return version;
}

bool ClaimNotification(const std::string &version)
{
    std::lock_guard lock(Mutex());
    if (NotifiedVersionSlot() == version)
        return false;
    NotifiedVersionSlot() = version;
    return true;
}

// 正在进行的探测，避免轮询间隔内叠两轮。
bool &Running()
{
    static bool running = false;
    return running;
}

std::wstring Widen(const std::string &text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string Narrow(const std::wstring &wide)
{
    if (wide.empty())
        return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string text(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), length, nullptr, nullptr);
    return text;
}

// GET 一个文本地址。成功返回 true。与其它联网路径一致地走用户配置的代理。
bool HttpGetText(const std::string &url, std::string &body)
{
    const std::wstring wide = Widen(url);
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts))
        return false;

    HINTERNET session = NetworkProxy::OpenWinHttpSession(L"MetasequoiaImeServer/1.0");
    if (!session)
        return false;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request =
        connection
            ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
            : nullptr;
    if (request)
    {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    const bool ok = request &&
                    WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                    WinHttpReceiveResponse(request, nullptr) &&
                    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) &&
                    status == HTTP_STATUS_OK;
    if (ok)
    {
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0 && body.size() < 1024 * 1024)
        {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
                break;
            body.append(chunk.data(), read);
        }
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

// 通知所有已连接的 TSF 客户端。用 worker 管道，因为那里本来就不受焦点客户端限制。
void BroadcastInstallerReady(const std::string &version, const std::filesystem::path &path,
                             const std::string &installer)
{
    // 三段：版本号、安装包绝对路径、安装包文件名。
    std::wstring payload = Widen(version);
    payload += L'\t';
    payload += path.wstring();
    payload += L'\t';
    payload += Widen(installer);
    BroadcastToTsfWorkerThreadViaNamedpipe(Global::DataFromServerMsgTypeToTsfWorkerThread::UpdateInstallerReady,
                                           payload);
}

struct Candidate
{
    std::string version; // 规范化后的版本号，用来命名目录
    std::string release_url;
    std::string installer;
    std::string sha256;
};

// 解析清单并与当前版本比对。任何一处不合法都判为「没有更新」，不让一条坏数据变成下载。
bool ResolveCandidate(const std::string &body, const update_channel::Version &current, Candidate &candidate)
{
    const nlohmann::json manifest = nlohmann::json::parse(body, nullptr, false);
    if (!manifest.is_object() || !manifest.contains("version") || !manifest["version"].is_string() ||
        !manifest.contains("releaseUrl") || !manifest["releaseUrl"].is_string())
    {
        return false;
    }
    const std::string raw_version = manifest["version"].get<std::string>();
    const std::string release_url = manifest["releaseUrl"].get<std::string>();

    update_channel::Version latest;
    if (!update_channel::Parse(raw_version, latest) || !latest.valid)
        return false;
    // 通道必须一致：正式版只跟正式版比。dev 与 nightly 在调用前就已被挡掉。
    if (latest.kind != update_channel::Kind::Release || current.kind != update_channel::Kind::Release)
        return false;
    if (update_channel::Compare(latest, current) <= 0)
        return false;

    // 发布页地址必须指向本仓库，且 tag 与版本号对得上，避免清单里塞一个任意地址。
    const std::string tag = update_channel::TagFromReleaseUrl(release_url);
    if (tag.empty())
        return false;
    update_channel::Version from_tag;
    if (!update_channel::Parse(tag, from_tag) || update_channel::Compare(from_tag, latest) != 0)
        return false;

    candidate.version = latest.text;
    candidate.release_url = release_url;
    candidate.installer = manifest.value("installerName", std::string());
    candidate.sha256 = manifest.value("installerSha256", std::string());
    return true;
}

std::atomic<bool> g_started{false};

// 同一时刻只允许拉起一个安装程序。弹窗是广播给所有 TSF 宿主的，几个窗口各点一次
// 「是」时不能同时起两个 Inno 实例，否则后一个会自己退出，看起来像装到一半没了。
std::atomic<bool> g_launching{false};

void PollLoop()
{
    std::this_thread::sleep_for(std::chrono::minutes(kStartupDelayMinutes));
    for (;;)
    {
        CheckOnce();
        std::this_thread::sleep_for(std::chrono::minutes(kPollIntervalMinutes));
    }
}
} // namespace

std::string CurrentVersion()
{
    return MSIME_PRODUCT_VERSION;
}

std::filesystem::path InstallerDirectory(const std::string &version)
{
    return update_channel::InstallerDirectory(version);
}

std::string PendingVersion()
{
    std::lock_guard lock(Mutex());
    return PendingVersionSlot();
}

bool LaunchPendingInstaller(const std::wstring &requested_installer)
{
    std::string version;
    std::string digest;
    {
        std::lock_guard lock(Mutex());
        version = PendingVersionSlot();
        digest = PendingSha256Slot();
    }
    if (version.empty() || digest.empty())
        return false;
    const std::filesystem::path directory = InstallerDirectory(version);
    if (directory.empty())
        return false;
    const std::filesystem::path path = directory / std::filesystem::path(requested_installer);
    // 只认自己写下的那一份：名字合法、就在这个版本目录里、文件真的存在。
    // 名字的校验函数吃窄字符，这里转一次；文件名一直是 ASCII。
    if (!native_settings::update_download::IsInstallerName(Narrow(requested_installer)))
        return false;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return false;
    // 最后一道完整性检查。下载结束时已经算过一次，但那次与现在之间隔着用户读弹窗、
    // 点 UAC 的时间，文件落在中等完整性的数据目录里，任何人、任何截断都能改写它。
    // 摘要对不上就不启动，宁可让用户重新检查一次。
    if (!native_settings::update_download::FileMatchesDigest(path, digest))
    {
        DIAG_LOGF(L"[update] installer digest mismatch before launch version={} file={}", Widen(version),
                  requested_installer);
        return false;
    }
    // 安装器会替换本进程和几个面板 exe，先让它们退出，避免 Inno 逼用户重启。
    CloseEmojiPanelApplication();
    CloseKeyboardPanelApplication();
    CloseHandwritingPanelApplication();
    // 多个宿主同时点「是」时只放行第一个；后到的直接放弃，不再起第二个 Inno 实例。
    bool expected = false;
    if (!g_launching.compare_exchange_strong(expected, true))
        return false;
    // 安装包需要提权，交给 ShellExecute 走 UAC；不放参数，让用户在 Inno Setup 里自己确认。
    const HINSTANCE result =
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, path.parent_path().c_str(), SW_SHOWNORMAL);
    const bool launched = reinterpret_cast<INT_PTR>(result) > 32;
    if (!launched)
        g_launching = false;
    DIAG_LOGF(L"[update] launch installer version={} launched={}", Widen(version), launched);
    return launched;
}

void CheckOnce()
{
    {
        std::lock_guard lock(Mutex());
        if (Running())
            return;
        Running() = true;
    }
    const auto done = [](const std::string &version, const std::string &digest) {
        std::lock_guard lock(Mutex());
        // 一次探测失败不该把已经下好、校验过的包从待安装状态里抹掉：用户可能正开着
        // 提示框，下一次轮询刚好断网就点不动了。只有拿到新的可用结果才覆盖。
        if (!version.empty())
        {
            PendingVersionSlot() = version;
            PendingSha256Slot() = digest;
        }
        Running() = false;
    };

    // 只有正式版参与。dev 与 nightly 的清单不由官网提供，版本比较也没有意义。
    update_channel::Version current;
    if (!update_channel::Parse(CurrentVersion(), current) || !current.valid ||
        !update_channel::SupportsAutoUpdate(current) || current.kind != update_channel::Kind::Release)
    {
        DIAG_LOGF(L"[update] poll skipped current={} reason=channel", Widen(CurrentVersion()));
        done({}, {});
        return;
    }
    if (!GetConfiguredAutoDownloadUpdate())
    {
        DIAG_LOGF(L"[update] poll skipped current={} reason=auto_download_off", Widen(CurrentVersion()));
        done({}, {});
        return;
    }

    // 缓存穿透：加一个时间戳，免得 CDN 一直返回旧清单。
    const long long stamp = std::chrono::system_clock::now().time_since_epoch().count();
    std::string body;
    if (!HttpGetText(std::string(kManifestUrl) + "?t=" + std::to_string(stamp), body))
    {
        DIAG_LOGF(L"[update] manifest request failed current={}", Widen(CurrentVersion()));
        done({}, {});
        return;
    }

    Candidate candidate;
    if (!ResolveCandidate(body, current, candidate))
    {
        DIAG_LOGF(L"[update] manifest rejected current={}", Widen(CurrentVersion()));
        done({}, {});
        return;
    }
    DIAG_LOGF(L"[update] manifest latest={} current={}", Widen(candidate.version), Widen(CurrentVersion()));
    if (!native_settings::update_download::IsInstallerName(candidate.installer) ||
        !native_settings::update_download::IsSha256Hex(candidate.sha256))
    {
        DIAG_LOGF(L"[update] manifest rejected latest={} reason=installer_fields", Widen(candidate.version));
        done({}, {});
        return;
    }

    const std::filesystem::path directory = InstallerDirectory(candidate.version);
    if (directory.empty())
    {
        DIAG_LOGF(L"[update] installer directory unavailable latest={}", Widen(candidate.version));
        done({}, {});
        return;
    }
    const std::filesystem::path existing = directory / std::filesystem::path(candidate.installer);
    const auto now_seconds = [] {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
    };
    const auto write_progress = [&](update_channel::DownloadProgress::State state, std::uint64_t received,
                                    std::uint64_t total, const std::string &reason) {
        update_channel::DownloadProgress progress;
        progress.state = state;
        progress.version = candidate.version;
        progress.received = received;
        progress.total = total;
        progress.reason = reason;
        progress.updated_at = now_seconds();
        update_channel::WriteProgress(progress);
    };
    const auto download = [&]() {
        native_settings::update_download::Request request;
        request.current_version = CurrentVersion();
        request.release_url = candidate.release_url;
        request.installer_name = candidate.installer;
        request.installer_sha256 = candidate.sha256;
        request.directory = directory;
        // 每收一块就写一次进度，设置页靠它显示百分比。写的是临时文件加改名，
        // 读的一侧不会看到半截内容。
        request.on_progress = [&write_progress](std::uint64_t received, std::uint64_t total) {
            write_progress(update_channel::DownloadProgress::State::Downloading, received, total, {});
        };
        const native_settings::update_download::Result result = native_settings::update_download::Download(request);
        if (result.outcome == native_settings::update_download::Outcome::Ok)
            return true;
        for (const native_settings::update_download::Attempt &attempt : result.attempts)
        {
            DIAG_LOGF(L"[update] attempt url={} outcome={} bytes={} seconds={} message={}", Widen(attempt.url),
                      static_cast<int>(attempt.outcome), attempt.bytes, attempt.seconds, Widen(attempt.message));
        }
        write_progress(update_channel::DownloadProgress::State::Failed, 0, 0,
                       result.message.empty() ? std::string("下载失败") : result.message);
        return false;
    };

    std::error_code ec;
    bool ready = false;
    if (std::filesystem::is_regular_file(existing, ec))
    {
        // 目录里已经有同名文件：摘要对得上就直接用，对不上说明上次没写完整或被换掉了，
        // 删掉重下。不能只看文件存在就当成可用。
        ready = native_settings::update_download::FileMatchesDigest(existing, candidate.sha256);
        DIAG_LOGF(L"[update] existing installer latest={} verified={}", Widen(candidate.version), ready);
        if (!ready)
            std::filesystem::remove(existing, ec);
    }
    if (!ready)
    {
        // 上一次进程崩溃留下的 .part 不能续（临时名带进程号），留着只会占几百 MB。
        native_settings::update_download::PruneOldInstallers(directory, candidate.installer);
        ready = download();
    }
    if (!ready)
    {
        DIAG_LOGF(L"[update] download failed latest={}", Widen(candidate.version));
        done({}, {});
        return;
    }
    // 已经下过并校验过了，只差提示。进度停在「等待安装」，设置页据此显示提示。
    write_progress(update_channel::DownloadProgress::State::Ready, 0, 0, {});
    done(candidate.version, candidate.sha256);
    DIAG_LOGF(L"[update] installer ready latest={}", Widen(candidate.version));
    // 没有人连着就什么也没弹出去，这时不能记成「已经问过」，否则这个版本在本次
    // 进程里再也不会提示。等到某一轮轮询时有宿主连着，再问一次。
    if (ConnectedTsfWorkerClientCount() > 0 && ClaimNotification(candidate.version))
        BroadcastInstallerReady(candidate.version, existing, candidate.installer);
}

void StartPolling()
{
    if (g_started.exchange(true))
        return;
    // 轮询线程跟着进程活着，退出时直接 detach：没下完的 .part 下次会被覆盖。
    std::thread(PollLoop).detach();
}
} // namespace update_check
