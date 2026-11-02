#include "settings/update_check.h"

#include "config/ime_config.h"
#include "ipc/ipc.h"
#include "log/candidate_diag_log.h"
#include "msime_version.h"
#include "settings/native/native_settings_update_download.h"
#include "settings/settings_launcher.h"
#include "settings/update_channel.h"
#include "utils/common_utils.h"
#include "utils/network_proxy.h"

#include <Windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
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

std::string &PendingInstallerSlot()
{
    static std::string installer;
    return installer;
}

bool &PendingSignedSlot()
{
    static bool signed_installer = false;
    return signed_installer;
}

// 只清自己那一版。轮询线程可能在提示或启动期间已经换上了更新的版本，
// 不能把新的候选连带抹掉。
void ClearPendingSlotsIfVersion(const std::string &version)
{
    std::lock_guard lock(Mutex());
    if (PendingVersionSlot() != version)
        return;
    PendingVersionSlot().clear();
    PendingSha256Slot().clear();
    PendingInstallerSlot().clear();
    PendingSignedSlot() = false;
}

// 已经提示过的版本，落在 updates/notified.txt。只在进程内记的话，Server 一重启
// 就会对同一个已经下载好的版本再问一遍；安装器运行期间重启还会再弹一次。
std::filesystem::path NotifiedPath()
{
    const std::wstring data = CommonUtils::get_ime_data_path_w();
    if (data.empty())
        return {};
    return std::filesystem::path(data) / L"updates" / L"notified.txt";
}

// 某个版本已经在启动前验签失败。环境问题不会因为再下一遍 64 MB 就好，记下来，
// 这一版不再自动重试；换新版本或用户手动下载都不受影响。
std::filesystem::path FailedPath()
{
    const std::wstring data = CommonUtils::get_ime_data_path_w();
    if (data.empty())
        return {};
    return std::filesystem::path(data) / L"updates" / L"failed.txt";
}

std::string ReadStateLine(const std::filesystem::path &path)
{
    if (path.empty())
        return {};
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::string line;
    std::getline(file, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    return line;
}

void WriteStateLine(const std::filesystem::path &path, const std::string &line)
{
    if (path.empty() || line.empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
            return;
        file << line;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        std::filesystem::remove(temporary, ec);
}

std::string ReadNotifiedVersion()
{
    return ReadStateLine(NotifiedPath());
}

void WriteNotifiedVersion(const std::string &version)
{
    WriteStateLine(NotifiedPath(), version);
}

void ClearNotifiedVersion()
{
    const std::filesystem::path path = NotifiedPath();
    if (path.empty())
        return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void ClearNotifiedVersionIfVersion(const std::string &version)
{
    if (ReadNotifiedVersion() == version)
        ClearNotifiedVersion();
}

std::string ReadFailedVersion()
{
    const std::string line = ReadStateLine(FailedPath());
    const size_t tab = line.find('\t');
    return tab == std::string::npos ? line : line.substr(0, tab);
}

std::string ReadFailedDigest()
{
    const std::string line = ReadStateLine(FailedPath());
    const size_t tab = line.find('\t');
    return tab == std::string::npos ? std::string() : line.substr(tab + 1);
}

void WriteFailedVersion(const std::string &version, const std::string &digest)
{
    // 连摘要一起记：同一版本重新上传了不同的包，摘要变了就不该再跳过。
    WriteStateLine(FailedPath(), version + "\t" + digest);
}

// 轮询线程和安装失败路径都会写 progress.txt，底层临时文件名是固定的，必须串行。
std::mutex &ProgressMutex()
{
    static std::mutex mutex;
    return mutex;
}

void WriteProgressState(update_channel::DownloadProgress::State state, const std::string &version,
                        std::uint64_t received, std::uint64_t total, const std::string &reason)
{
    std::lock_guard lock(ProgressMutex());
    update_channel::DownloadProgress progress;
    progress.state = state;
    progress.version = version;
    progress.received = received;
    progress.total = total;
    progress.reason = reason;
    progress.updated_at = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    update_channel::WriteProgress(progress);
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

// 提示路径只允许一个在跑。轮询线程和客户端激活线程都会进来，两个同时读到
// 「还没提示过」就会弹两次。
std::mutex &PromptMutex()
{
    static std::mutex mutex;
    return mutex;
}

// 只发给当前活动的 TSF 宿主：广播会让每个已连接的宿主各弹一个同样的模态框。
bool SendInstallerReady(uint64_t client_id, const std::string &version, const std::filesystem::path &path,
                        const std::string &installer)
{
    // 三段：版本号、安装包路径、安装包文件名。TSF 只用到版本号和文件名，路径仅作
    // 展示；数据目录很深时路径本身就可能超过管道载荷上限，这时留空，别让整条提示
    // 因为一个展示字段发不出去。
    const std::wstring wide_version = Widen(version);
    const std::wstring wide_installer = Widen(installer);
    std::wstring payload = wide_version;
    payload += L'\t';
    const std::wstring wide_path = path.wstring();
    if (wide_version.size() + wide_path.size() + wide_installer.size() + 2 < FanyImePipeLimits::CandidateTextCapacity)
    {
        payload += wide_path;
    }
    payload += L'\t';
    payload += wide_installer;
    return SendToTsfWorkerThreadClientViaNamedpipe(
        client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::UpdateInstallerReady, payload);
}

// 有待安装包就尝试提示一次。没有活动宿主、已经提示过都不算提示成功；只有真正
// 写进管道才记 notified.txt，客户端下次激活还能补上。文件完整性和签名留给启动
// 路径核对，这里不做 64 MB 的重复读盘。
void BroadcastPendingIfAny()
{
    std::lock_guard prompt_lock(PromptMutex());
    std::string version;
    std::string installer;
    {
        std::lock_guard lock(Mutex());
        version = PendingVersionSlot();
        installer = PendingInstallerSlot();
    }
    if (version.empty() || installer.empty())
        return;
    if (ReadNotifiedVersion() == version)
        return;
    const PipeClientActivation active = GetActivePipeClient();
    if (active.client_id == 0)
        return;
    const std::filesystem::path path = InstallerDirectory(version) / std::filesystem::path(installer);
    if (SendInstallerReady(active.client_id, version, path, installer))
    {
        WriteNotifiedVersion(version);
        DIAG_LOGF(L"[update] installer prompt sent version={} client={}", Widen(version), active.client_id);
    }
}

struct Candidate
{
    std::string version; // 规范化后的版本号，用来命名目录
    std::string release_url;
    std::string installer;
    std::string sha256;
    bool signed_installer = false;
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
    // value() 在键存在但类型不对时会抛 type_error，轮询线程没有捕获就会
    // std::terminate。先把可选字段的类型挡掉，坏清单当成「没有更新」。
    if ((manifest.contains("installerName") && !manifest["installerName"].is_string()) ||
        (manifest.contains("installerSha256") && !manifest["installerSha256"].is_string()) ||
        (manifest.contains("signed") && !manifest["signed"].is_boolean()))
    {
        return false;
    }
    candidate.installer = manifest.value("installerName", std::string());
    candidate.sha256 = manifest.value("installerSha256", std::string());
    // 清单写明 signed=true 时，启动前还要过 Authenticode。缺省按未声明处理，
    // 不因为字段缺失就把没签名的包拦下来。
    candidate.signed_installer = manifest.value("signed", false);
    return true;
}

std::atomic<bool> g_started{false};

// 同一时刻只允许拉起一个安装程序。提示只发给当前活动的宿主，但用户在多个窗口
// 之间快速切换时仍可能点到两次；后一次直接放弃，不再起第二个 Inno 实例。
std::atomic<bool> g_launching{false};

void PollLoop()
{
    std::this_thread::sleep_for(std::chrono::minutes(kStartupDelayMinutes));
    for (;;)
    {
        try
        {
            CheckOnce();
        }
        catch (...)
        {
            // 清单或文件系统里出现没预料到的异常时，别让轮询线程整个死掉，
            // 也别把 Running 永远留在 true。
            std::lock_guard lock(Mutex());
            Running() = false;
        }
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

bool LaunchPendingInstaller(const std::wstring &requested_installer, std::wstring &error)
{
    std::lock_guard prompt_lock(PromptMutex());
    error.clear();
    if (g_launching.load())
    {
        // 重复点击不是错误，调用方不要弹红色提示框。
        return false;
    }
    std::string version;
    std::string digest;
    std::string installer;
    bool signed_installer = false;
    {
        std::lock_guard lock(Mutex());
        version = PendingVersionSlot();
        digest = PendingSha256Slot();
        installer = PendingInstallerSlot();
        signed_installer = PendingSignedSlot();
    }
    if (version.empty() || digest.empty() || installer.empty())
    {
        // 弹窗是旧的，或者上一轮已经装完了；静默收敛，不打扰用户。
        return false;
    }
    // 只认自己写下的那一份：名字合法、和待安装版本一致。
    if (!native_settings::update_download::IsInstallerName(Narrow(requested_installer)) ||
        Narrow(requested_installer) != installer)
    {
        error = L"安装包与当前待安装版本不一致，请重新检查。";
        ClearPendingSlotsIfVersion(version);
        ClearNotifiedVersionIfVersion(version);
        return false;
    }
    const std::filesystem::path path = InstallerDirectory(version) / std::filesystem::path(installer);
    // 打开一个只共享读的句柄并核对摘要。句柄会一直拿到安装程序启动之后再关，
    // 这段时间里其他进程拿不到写权限，改不了也删不掉。
    HANDLE file = INVALID_HANDLE_VALUE;
    if (!native_settings::update_download::OpenVerifiedInstaller(path, digest, file))
    {
        DIAG_LOGF(L"[update] installer digest mismatch before launch version={} file={}", Widen(version),
                  requested_installer);
        ClearPendingSlotsIfVersion(version);
        ClearNotifiedVersionIfVersion(version);
        WriteProgressState(update_channel::DownloadProgress::State::Failed, version, 0, 0,
                           "安装包已损坏，请重新检查更新");
        error = L"安装包已损坏，请重新检查更新。";
        return false;
    }
    if (signed_installer && !native_settings::update_download::VerifyAuthenticode(path))
    {
        CloseHandle(file);
        DIAG_LOGF(L"[update] installer signature invalid version={} file={}", Widen(version), requested_installer);
        ClearPendingSlotsIfVersion(version);
        // 文件留着，不反复重下 64 MB；notified 也留着，同一个版本不再一遍遍弹。
        // failed.txt 让下一轮轮询直接跳过这一版，保留 Failed 进度给设置页看。
        WriteFailedVersion(version, digest);
        WriteProgressState(update_channel::DownloadProgress::State::Failed, version, 0, 0,
                           "安装包签名校验失败，请从发布页手动下载");
        error = L"安装包签名校验失败。请从发布页手动下载安装包。";
        return false;
    }
    // 安装器会替换本进程和几个面板 exe，先让它们退出，避免 Inno 逼用户重启。
    CloseEmojiPanelApplication();
    CloseKeyboardPanelApplication();
    CloseHandwritingPanelApplication();
    // 多个宿主同时点「是」时只放行第一个；后到的直接放弃，不再起第二个 Inno 实例。
    bool expected = false;
    if (!g_launching.compare_exchange_strong(expected, true))
    {
        CloseHandle(file);
        return false;
    }
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = path.c_str();
    const std::filesystem::path working_directory = path.parent_path();
    info.lpDirectory = working_directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    const bool launched = ShellExecuteExW(&info) != FALSE;
    if (launched)
    {
        // 等安装器起来并打开安装包，再放开只读句柄。最多等 5 秒，失败也不影响启动。
        if (info.hProcess != nullptr)
        {
            WaitForInputIdle(info.hProcess, 5000);
            CloseHandle(info.hProcess);
        }
        // 成功拉起后清掉待安装状态，但保留 notified：安装器运行期间 Server 重启时
        // 不会再对同一个版本问一次。
        ClearPendingSlotsIfVersion(version);
        DIAG_LOGF(L"[update] launch installer version={} launched=true", Widen(version));
    }
    else
    {
        g_launching = false;
        ClearPendingSlotsIfVersion(version);
        ClearNotifiedVersionIfVersion(version);
        WriteProgressState(update_channel::DownloadProgress::State::Failed, version, 0, 0, "无法启动安装程序");
        error = L"安装包已下载并通过校验，但无法启动安装程序。请稍后重试。";
        DIAG_LOGF(L"[update] launch installer version={} launched=false", Widen(version));
    }
    CloseHandle(file);
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
    const auto done = [](const std::string &version, const std::string &digest, const std::string &installer,
                         const bool signed_installer) {
        std::lock_guard lock(Mutex());
        // 一次探测失败不该把已经下好、校验过的包从待安装状态里抹掉：用户可能正开着
        // 提示框，下一次轮询刚好断网就点不动了。只有拿到新的可用结果才覆盖。
        if (!version.empty())
        {
            PendingVersionSlot() = version;
            PendingSha256Slot() = digest;
            PendingInstallerSlot() = installer;
            PendingSignedSlot() = signed_installer;
        }
        Running() = false;
    };

    // 只有正式版参与。dev 与 nightly 的清单不由官网提供，版本比较也没有意义。
    update_channel::Version current;
    if (!update_channel::Parse(CurrentVersion(), current) || !current.valid ||
        !update_channel::SupportsAutoUpdate(current) || current.kind != update_channel::Kind::Release)
    {
        DIAG_LOGF(L"[update] poll skipped current={} reason=channel", Widen(CurrentVersion()));
        done({}, {}, {}, false);
        return;
    }
    if (!GetConfiguredAutoDownloadUpdate())
    {
        DIAG_LOGF(L"[update] poll skipped current={} reason=auto_download_off", Widen(CurrentVersion()));
        done({}, {}, {}, false);
        return;
    }

    // 缓存穿透：加一个时间戳，免得 CDN 一直返回旧清单。
    const long long stamp = std::chrono::system_clock::now().time_since_epoch().count();
    std::string body;
    if (!HttpGetText(std::string(kManifestUrl) + "?t=" + std::to_string(stamp), body))
    {
        DIAG_LOGF(L"[update] manifest request failed current={}", Widen(CurrentVersion()));
        done({}, {}, {}, false);
        return;
    }

    Candidate candidate;
    if (!ResolveCandidate(body, current, candidate))
    {
        DIAG_LOGF(L"[update] manifest rejected current={}", Widen(CurrentVersion()));
        done({}, {}, {}, false);
        return;
    }
    DIAG_LOGF(L"[update] manifest latest={} current={}", Widen(candidate.version), Widen(CurrentVersion()));
    if (!native_settings::update_download::IsInstallerName(candidate.installer) ||
        !native_settings::update_download::IsSha256Hex(candidate.sha256))
    {
        DIAG_LOGF(L"[update] manifest rejected latest={} reason=installer_fields", Widen(candidate.version));
        done({}, {}, {}, false);
        return;
    }
    if (ReadFailedVersion() == candidate.version && ReadFailedDigest() == candidate.sha256)
    {
        // 这一版在启动前验签失败过。再下一遍同样会失败，保留 Failed 进度让设置页
        // 显示原因，等新版本或用户手动处理。同一版本重新上传了不同的包时摘要会变，
        // 这里会自动放行。
        DIAG_LOGF(L"[update] skipped failed version={}", Widen(candidate.version));
        done({}, {}, {}, false);
        return;
    }

    const std::filesystem::path directory = InstallerDirectory(candidate.version);
    if (directory.empty())
    {
        DIAG_LOGF(L"[update] installer directory unavailable latest={}", Widen(candidate.version));
        done({}, {}, {}, false);
        return;
    }
    const std::filesystem::path existing = directory / std::filesystem::path(candidate.installer);
    const auto download = [&]() {
        native_settings::update_download::Request request;
        request.current_version = CurrentVersion();
        request.release_url = candidate.release_url;
        request.installer_name = candidate.installer;
        request.installer_sha256 = candidate.sha256;
        request.directory = directory;
        // 每收一块就写一次进度，设置页靠它显示百分比。写的是临时文件加改名，
        // 读的一侧不会看到半截内容。
        request.on_progress = [&candidate](std::uint64_t received, std::uint64_t total) {
            WriteProgressState(update_channel::DownloadProgress::State::Downloading, candidate.version, received, total,
                               {});
        };
        const native_settings::update_download::Result result = native_settings::update_download::Download(request);
        if (result.outcome == native_settings::update_download::Outcome::Ok)
            return true;
        for (const native_settings::update_download::Attempt &attempt : result.attempts)
        {
            DIAG_LOGF(L"[update] attempt url={} outcome={} bytes={} total={} seconds={} message={}", Widen(attempt.url),
                      static_cast<int>(attempt.outcome), attempt.bytes, attempt.total, attempt.seconds,
                      Widen(attempt.message));
        }
        WriteProgressState(update_channel::DownloadProgress::State::Failed, candidate.version, 0, 0,
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
        done({}, {}, {}, false);
        return;
    }
    // 已经下过并校验过了，只差提示。进度停在「等待安装」，设置页据此显示提示。
    WriteProgressState(update_channel::DownloadProgress::State::Ready, candidate.version, 0, 0, {});
    done(candidate.version, candidate.sha256, candidate.installer, candidate.signed_installer);
    DIAG_LOGF(L"[update] installer ready latest={}", Widen(candidate.version));
    // 没有活动宿主时不会写 notified.txt，客户端激活时会再补一次提示。
    BroadcastPendingIfAny();
}

void NotifyClientActivated()
{
    // 一次只跑一个补提示任务。客户端激活很频繁，不能每次都去读一遍安装包。
    static std::atomic<bool> notifying{false};
    if (notifying.exchange(true))
        return;
    try
    {
        std::thread([] {
            try
            {
                BroadcastPendingIfAny();
            }
            catch (...)
            {
            }
            notifying = false;
        }).detach();
    }
    catch (...)
    {
        notifying = false;
    }
}

void StartPolling()
{
    if (g_started.exchange(true))
        return;
    // 轮询线程跟着进程活着，退出时直接 detach：没下完的 .part 下次会被覆盖。
    std::thread(PollLoop).detach();
}
} // namespace update_check
