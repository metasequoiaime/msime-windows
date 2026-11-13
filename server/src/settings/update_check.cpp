#include "settings/update_check.h"

#include "config/ime_config.h"
#include "log/candidate_diag_log.h"
#include "msime_version.h"
#include "settings/native/native_settings_update_download.h"
#include "settings/update_channel.h"
#include "utils/common_utils.h"
#include "utils/network_proxy.h"

#include <Windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace update_check
{
namespace
{
constexpr char kManifestUrl[] = "https://msime.app/update.json";

std::mutex &Mutex()
{
    static std::mutex mutex;
    return mutex;
}

// 某个版本的安装包签名校验失败过（没签名或签名与内容不符）。再下一遍也一样，
// 记下来，这一版不再自动重试；换新版本或用户手动下载都不受影响。
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

void WriteProgressState(update_channel::DownloadProgress::State state, const std::string &version,
                        std::uint64_t received, std::uint64_t total, const std::string &reason)
{
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
    // 清单写明 signed=true 时还要过 Authenticode。缺省按未声明处理，
    // 不因为字段缺失就把没签名的包拦下来。
    candidate.signed_installer = manifest.value("signed", false);
    return true;
}

bool IsNewerThan(const std::string &version, const update_channel::Version &current)
{
    update_channel::Version parsed;
    return update_channel::Parse(version, parsed) && parsed.valid && parsed.kind == current.kind &&
           update_channel::Compare(parsed, current) > 0;
}

// 已经装上的版本不再需要安装包：updates/ 下所有不比本机新的版本目录都删掉，进度文件
// 记的版本不比本机新时回到 Idle，设置页不再显示「等待安装」。按目录扫而不是只看进度
// 文件：完整包安装时会清数据目录，progress.txt 没了，但正在运行的安装包删不掉，留下的
// 目录只能靠这里收。
void RetireInstalledVersions(const update_channel::Version &current)
{
    const update_channel::DownloadProgress progress = update_channel::ReadProgress();
    if (progress.state != update_channel::DownloadProgress::State::Idle && !IsNewerThan(progress.version, current))
        WriteProgressState(update_channel::DownloadProgress::State::Idle, {}, 0, 0, {});

    const std::wstring data = CommonUtils::get_ime_data_path_w();
    if (data.empty())
        return;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::path(data) / L"updates", ec))
    {
        std::error_code entry_error;
        if (!entry.is_directory(entry_error))
            continue;
        const std::string name = entry.path().filename().u8string();
        update_channel::Version parsed;
        // 认不出版本号的目录不是我们建的，不动。
        if (!update_channel::Parse(name, parsed) || !parsed.valid || IsNewerThan(name, current))
            continue;
        std::filesystem::remove_all(entry.path(), entry_error);
        DIAG_LOGF(L"[update] retired installed version={} removed={}", Widen(name), !entry_error);
    }
}

std::atomic<bool> g_started{false};

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

void CheckOnce()
{
    {
        std::lock_guard lock(Mutex());
        if (Running())
            return;
        Running() = true;
    }
    const auto done = [] {
        std::lock_guard lock(Mutex());
        Running() = false;
    };

    // 只有正式版参与。dev 与 nightly 的清单不由官网提供，版本比较也没有意义。
    update_channel::Version current;
    if (!update_channel::Parse(CurrentVersion(), current) || !current.valid ||
        !update_channel::SupportsAutoUpdate(current) || current.kind != update_channel::Kind::Release)
    {
        DIAG_LOGF(L"[update] poll skipped current={} reason=channel", Widen(CurrentVersion()));
        done();
        return;
    }
    // 不管开没开自动下载都先收尾：用户可能是从设置页手动装上的新版本。
    RetireInstalledVersions(current);
    if (!GetConfiguredAutoDownloadUpdate())
    {
        DIAG_LOGF(L"[update] poll skipped current={} reason=auto_download_off", Widen(CurrentVersion()));
        done();
        return;
    }

    // 缓存穿透：加一个时间戳，免得 CDN 一直返回旧清单。
    const long long stamp = std::chrono::system_clock::now().time_since_epoch().count();
    std::string body;
    if (!HttpGetText(std::string(kManifestUrl) + "?t=" + std::to_string(stamp), body))
    {
        DIAG_LOGF(L"[update] manifest request failed current={}", Widen(CurrentVersion()));
        done();
        return;
    }

    Candidate candidate;
    if (!ResolveCandidate(body, current, candidate))
    {
        DIAG_LOGF(L"[update] manifest rejected current={}", Widen(CurrentVersion()));
        done();
        return;
    }
    DIAG_LOGF(L"[update] manifest latest={} current={}", Widen(candidate.version), Widen(CurrentVersion()));
    if (!native_settings::update_download::IsInstallerName(candidate.installer) ||
        !native_settings::update_download::IsSha256Hex(candidate.sha256))
    {
        DIAG_LOGF(L"[update] manifest rejected latest={} reason=installer_fields", Widen(candidate.version));
        done();
        return;
    }
    if (ReadFailedVersion() == candidate.version && ReadFailedDigest() == candidate.sha256)
    {
        // 这一版的签名校验失败过。再下一遍同样会失败，保留 Failed 进度让设置页
        // 显示原因，等新版本或用户手动处理。同一版本重新上传了不同的包时摘要会变，
        // 这里会自动放行。
        DIAG_LOGF(L"[update] skipped failed version={}", Widen(candidate.version));
        done();
        return;
    }

    const std::filesystem::path directory = InstallerDirectory(candidate.version);
    if (directory.empty())
    {
        DIAG_LOGF(L"[update] installer directory unavailable latest={}", Widen(candidate.version));
        done();
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
        done();
        return;
    }
    if (candidate.signed_installer)
    {
        using native_settings::update_download::SignatureCheck;
        const SignatureCheck signature = native_settings::update_download::VerifyAuthenticode(existing);
        if (signature == SignatureCheck::Invalid)
        {
            // 文件本身没签名或签名与内容不符：删掉，记下这一版，别再反复下载。
            std::filesystem::remove(existing, ec);
            WriteFailedVersion(candidate.version, candidate.sha256);
            WriteProgressState(update_channel::DownloadProgress::State::Failed, candidate.version, 0, 0,
                               "安装包签名校验失败，请从发布页手动下载");
            DIAG_LOGF(L"[update] installer signature invalid latest={}", Widen(candidate.version));
            done();
            return;
        }
        // 本机暂时建不起信任链时照样标成可安装：设置页点「立即安装」前还会再验一次，
        // 那时多半已经能联网取到根证书。
        if (signature == SignatureCheck::Unverifiable)
            DIAG_LOGF(L"[update] installer signature unverifiable latest={}", Widen(candidate.version));
    }
    // 不主动弹窗：进度停在「等待安装」，由设置页的「立即安装」按钮触发安装。
    WriteProgressState(update_channel::DownloadProgress::State::Ready, candidate.version, 0, 0, {});
    DIAG_LOGF(L"[update] installer ready latest={}", Widen(candidate.version));
    done();
}

void StartPolling()
{
    if (g_started.exchange(true))
        return;
    // 轮询线程跟着进程活着，退出时直接 detach：没下完的 .part 下次会被覆盖。
    std::thread(PollLoop).detach();
}
} // namespace update_check
