#include "settings/native/native_settings_update_download.h"

#include "utils/network_proxy.h"

#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <regex>
#include <vector>

namespace native_settings::update_download
{
namespace
{
// 首段测速的窗口与门槛。窗口长度取得比「一眼看上去」长：真实的家庭宽带在头一两秒
// 还在慢启动，3 秒的样本会把一条正常的连接误判成不顶用。
constexpr std::uint64_t kProbeSeconds = 10;
constexpr std::uint64_t kProbeMinBytes = 1024 * 1024;
constexpr std::uint64_t kProbeFloorBytesPerSecond = 50 * 1024;
// 通过测速之后，连续这么久收不到任何字节就算这次尝试失败。
constexpr std::uint64_t kStallSeconds = 20;
// 安装包上限。当前安装包约 100 MB，留十倍余量；超过就当作清单或重定向出了问题。
constexpr std::uint64_t kMaxInstallerBytes = 1024ull * 1024 * 1024;
constexpr int kAttemptsPerChannel = 3;
constexpr char kMirrorPrefix[] = "https://dl.msime.app/gh/";
constexpr char kPartSuffix[] = ".part";

using Clock = std::chrono::steady_clock;

std::uint64_t ElapsedSeconds(Clock::time_point from)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - from).count());
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

// URL 拆成 host + path。只接受 HTTPS：安装包不能走明文通道。
bool CrackHttps(const std::string &url, std::wstring &host, std::wstring &path, INTERNET_PORT &port)
{
    if (url.rfind("https://", 0) != 0)
        return false;
    const std::wstring wide = Widen(url);
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host_buffer[256]{};
    wchar_t path_buffer[4096]{};
    parts.lpszHostName = host_buffer;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host_buffer));
    parts.lpszUrlPath = path_buffer;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path_buffer));
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return false;
    host.assign(host_buffer, parts.dwHostNameLength);
    path.assign(path_buffer, parts.dwUrlPathLength);
    port = parts.nPort;
    return !host.empty() && !path.empty();
}

std::string HexDigest(const unsigned char raw[32])
{
    static const char kHex[] = "0123456789abcdef";
    std::string digest;
    digest.reserve(64);
    for (size_t i = 0; i < 32; ++i)
    {
        digest.push_back(kHex[raw[i] >> 4]);
        digest.push_back(kHex[raw[i] & 0x0F]);
    }
    return digest;
}

// 一次尝试。测速阶段失败会把 outcome 标成 Failed 并带上原因；测速通过后
// 就一直下到流结束。
Attempt FetchOnce(const std::string &url, const std::filesystem::path &target, Clock::time_point deadline,
                  const std::function<void(std::uint64_t, std::uint64_t)> &progress)
{
    Attempt attempt;
    attempt.url = url;
    const Clock::time_point started = Clock::now();

    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    if (!CrackHttps(url, host, path, port))
    {
        attempt.message = "下载地址无效";
        return attempt;
    }

    // 与其它联网路径一致地走用户配置的代理，不在调用点各自 WinHttpOpen。
    HINTERNET session = NetworkProxy::OpenWinHttpSession(L"MetasequoiaImeSettings/1.0");
    if (!session)
    {
        attempt.message = "无法建立网络会话";
        return attempt;
    }
    // 连接与发送保持短超时；接收留给停滞判定，不用统一超时把慢速连接一刀切掉。
    WinHttpSetTimeouts(session, 15000, 15000, 30000, static_cast<int>(kStallSeconds * 1000));
    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                   : nullptr;
    if (request)
    {
        // GitHub 的 release 资产会 302 到 CDN，镜像也会跳转，显式放开重定向。
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    }

    const auto close_all = [&] {
        if (request)
            WinHttpCloseHandle(request);
        if (connection)
            WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
    };

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    const bool responded =
        request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (!responded)
    {
        attempt.message = "连不上下载服务器";
        attempt.seconds = ElapsedSeconds(started);
        close_all();
        return attempt;
    }
    // 404 单独区分：镜像是按需回源的，新版本刚发布时它可能还没有，换下一条就好，
    // 重试同一条不会有不同结果。
    if (status == HTTP_STATUS_NOT_FOUND)
    {
        attempt.outcome = Outcome::NotFound;
        attempt.message = "服务器没有这个安装包";
        attempt.seconds = ElapsedSeconds(started);
        close_all();
        return attempt;
    }
    if (status != HTTP_STATUS_OK)
    {
        attempt.message = "下载服务器返回了 " + std::to_string(status);
        attempt.seconds = ElapsedSeconds(started);
        close_all();
        return attempt;
    }

    std::uint64_t total = 0;
    DWORD length = 0;
    DWORD length_size = sizeof(length);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size, WINHTTP_NO_HEADER_INDEX))
    {
        total = length;
    }
    if (total > kMaxInstallerBytes)
    {
        attempt.message = "安装包超过允许的大小";
        attempt.seconds = ElapsedSeconds(started);
        close_all();
        return attempt;
    }

    std::error_code ec;
    std::filesystem::remove(target, ec);
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    const bool hash_ready = file && BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
                            BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    if (!hash_ready)
    {
        attempt.message = "无法写入下载文件";
        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        close_all();
        std::filesystem::remove(target, ec);
        return attempt;
    }

    std::vector<char> buffer(64 * 1024);
    unsigned char digest[32]{};
    const Clock::time_point probe_started = Clock::now();
    bool probing = true;
    bool ok = true;

    for (;;)
    {
        if (Clock::now() > deadline)
        {
            attempt.message = "下载超时";
            ok = false;
            break;
        }
        DWORD available = 0;
        // 停滞判定放在等数据之前：WinHttpQueryDataAvailable 会一直阻塞到有数据或
        // 接收超时（30 秒），靠循环之间的时间差看不出空转。
        const Clock::time_point wait_started = Clock::now();
        const BOOL available_ok = WinHttpQueryDataAvailable(request, &available);
        if (!available_ok)
        {
            attempt.message = "下载中断";
            ok = false;
            break;
        }
        if (available == 0)
        {
            break;
        }
        // 一段数据等得太久，说明这条连接已经在原地打转。门槛取「停滞 20 秒」减去
        // 接收超时那一档，避免正常的长等待被误判。
        if (ElapsedSeconds(wait_started) >= kStallSeconds)
        {
            attempt.message = "下载停滞";
            ok = false;
            break;
        }
        const DWORD want = (std::min)(available, static_cast<DWORD>(buffer.size()));
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), want, &read) || read == 0)
        {
            attempt.message = "下载中断";
            ok = false;
            break;
        }
        if (attempt.bytes + read > kMaxInstallerBytes)
        {
            attempt.message = "安装包超过允许的大小";
            ok = false;
            break;
        }
        file.write(buffer.data(), static_cast<std::streamsize>(read));
        if (!file)
        {
            attempt.message = "写入下载文件失败";
            ok = false;
            break;
        }
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), read, 0) != 0)
        {
            attempt.message = "无法计算校验值";
            ok = false;
            break;
        }
        attempt.bytes += read;
        if (progress)
            progress(attempt.bytes, total);

        // 首段测速。收到足够样本或窗口结束就下判决，测速期间的数据不丢，继续使用。
        // 用毫秒算，不用整数秒：按秒取整会让门槛随样本量漂移，一条恰好卡在 50 KB/s
        // 附近的连接可能因为取整被判成慢。
        if (probing)
        {
            const auto probe_ms =
                (std::max)(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - probe_started).count(),
                           std::int64_t{500});
            const bool enough_sample = attempt.bytes >= kProbeMinBytes;
            if (enough_sample || probe_ms >= static_cast<std::int64_t>(kProbeSeconds * 1000))
            {
                const std::uint64_t bytes = attempt.bytes;
                if (bytes * 1000 < kProbeFloorBytesPerSecond * static_cast<std::uint64_t>(probe_ms))
                {
                    attempt.message = "下载速度低于 50 KB/s";
                    ok = false;
                    break;
                }
                probing = false;
            }
        }
    }
    file.close();
    close_all();

    if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0)
    {
        attempt.message = "无法计算校验值";
        ok = false;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    if (ok && total != 0 && attempt.bytes != total)
    {
        // 服务端声明了长度却没给全，不能当完整文件用。
        attempt.message = "下载不完整";
        ok = false;
    }
    attempt.seconds = ElapsedSeconds(started);
    if (!ok)
    {
        std::filesystem::remove(target, ec);
        attempt.bytes = 0;
        return attempt;
    }
    attempt.outcome = Outcome::Ok;
    attempt.digest = HexDigest(digest);
    return attempt;
}
} // namespace

bool IsSha256Hex(const std::string &text)
{
    if (text.size() != 64)
        return false;
    for (const unsigned char ch : text)
        if (!std::isdigit(ch) && !(ch >= 'a' && ch <= 'f'))
            return false;
    return true;
}

bool IsInstallerName(const std::string &name)
{
    static const std::regex pattern(R"(^MetasequoiaIME_Setup_v[\w.-]+\.exe$)", std::regex::icase);
    return name.size() <= 120 && std::regex_match(name, pattern);
}

bool IsReleaseVersion(const std::string &version)
{
    // 三段数字，不许有预发布标签与构建元数据。`0.11.1-dev.20261010.d1adf7df7682` 和
    // `0.11.1-nightly.20261010.d1adf7df7682` 都在这里被挡掉；`v0.11.1` 放行，因为发布页的
    // tag 就带 v，清单里的 version 字段两种写法都出现过。
    static const std::regex pattern(R"(^\s*[vV]?\d+\.\d+\.\d+\s*$)");
    return std::regex_match(version, pattern);
}

std::vector<std::string> ChannelUrls(const std::string &release_url, const std::string &installer_name)
{
    // 发布页地址换成资产地址：releases/tag/<tag> → releases/download/<tag>/<name>。
    static const std::regex tag(R"(^(https://github\.com/metasequoiaime/[\w.-]+)/releases/tag/([\w.+-]+)$)");
    std::smatch match;
    if (!IsInstallerName(installer_name) || !std::regex_match(release_url, match, tag))
        return {};
    const std::string github = match[1].str() + "/releases/download/" + match[2].str() + "/" + installer_name;
    // 镜像按前缀拼接原始资产地址，和关于页里「国内镜像下载」按钮用的是同一条形状。
    return {github, std::string(kMirrorPrefix) + github};
}

std::filesystem::path DefaultDirectory()
{
    // 放在用户临时目录下，不是数据目录：数据目录是中等完整性、按安装器设置可写的，
    // 从这里拉起的 exe 在「校验完成」到「用户点 UAC」之间可能被同机其他进程替换。
    // 临时目录再套一层进程唯一的子目录，让替换没有稳定的落脚点。
    //
    // 每次调用都算一次：设置窗口关掉只是隐藏，进程还活着，要求后端一次下载走的是
    // 同一个目录；用户可能又点了一次，那一次也该落在同一个目录里，靠临时的文件名
    // 区分，而不是靠目录。
    std::wstring base(MAX_PATH, L'\0');
    const DWORD length = GetTempPathW(static_cast<DWORD>(base.size()), base.data());
    base.resize(length == 0 || length >= base.size() ? 0 : length);
    if (base.empty())
        return {};
    const std::wstring name = L"MetasequoiaImeUpdate-" + std::to_wstring(GetCurrentProcessId());
    return std::filesystem::path(base) / name;
}

void PruneOldInstallers(const std::filesystem::path &directory, const std::string &keep_name)
{
    if (directory.empty())
        return;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(directory, ec))
    {
        if (!entry.is_regular_file(ec))
            continue;
        const std::string name = entry.path().filename().u8string();
        if (name == keep_name)
            continue;
        // 只删自己产生的文件：安装包、别的实例留下的临时文件，以及旧版本留下的
        // 「.exe.part」这类名字。被正在写入的文件占着就删不掉，删不掉就跳过。
        if (IsInstallerName(name) || name.find("MetasequoiaIME_Setup_") == 0)
            std::filesystem::remove(entry.path(), ec);
    }
}

std::string PartFileName(const std::string &installer_name)
{
    // 计数只增不减，配合进程号，同一台机器上并发的两个下载不会撞名。
    static std::atomic<unsigned long long> counter{0};
    const unsigned long long ticket = counter.fetch_add(1) + 1;
    return installer_name + kPartSuffix + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(ticket);
}

bool FileMatchesDigest(const std::filesystem::path &path, const std::string &expected_hex)
{
    if (!IsSha256Hex(expected_hex))
        return false;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0)
    {
        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::vector<char> buffer(64 * 1024);
    bool ok = true;
    while (file)
    {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = file.gcount();
        if (got <= 0)
            break;
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(got), 0) != 0)
        {
            ok = false;
            break;
        }
    }
    unsigned char digest[32]{};
    if (ok)
        ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok && HexDigest(digest) == expected_hex;
}

Result Download(const Request &request)
{
    Result result;
    // 下载只在正式版上开放。nightly 与 dev 构建的清单不由官网提供，版本比较也没有
    // 意义，所以在这里一刀切，而不是让它们下到一半再失败。
    if (!IsReleaseVersion(request.current_version))
    {
        result.message = "当前构建不是正式版，不自动下载安装包";
        return result;
    }
    if (!IsInstallerName(request.installer_name))
    {
        result.message = "更新清单里的安装包名无效";
        return result;
    }
    // 没有摘要就不下载：清单不给校验值等于放弃完整性判断。
    if (!IsSha256Hex(request.installer_sha256))
    {
        result.message = "更新清单没有提供有效的 SHA256";
        return result;
    }
    std::vector<std::string> channels = request.override_channel_urls;
    if (channels.empty())
        channels = ChannelUrls(request.release_url, request.installer_name);
    if (channels.empty())
    {
        result.message = "无法从发布页地址推出下载地址";
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(request.directory, ec);
    if (ec)
    {
        result.message = "无法创建下载目录";
        return result;
    }

    const std::filesystem::path final_path = request.directory / request.installer_name;
    // 每个下载任务用独占的临时名：两个实例同时下同一个版本时各写各的文件，
    // 谁先完成谁改名成正式名字，后完成的那份要么在校验后同样通过、要么自行放弃，
    // 不会把对方写了一半的数据当成自己的继续写。
    std::filesystem::path part_path = request.directory / PartFileName(request.installer_name);
    const Clock::time_point deadline =
        Clock::now() + std::chrono::seconds((std::max)(request.deadline_seconds, std::uint64_t{60}));

    bool saw_not_found = false;
    for (const std::string &channel : channels)
    {
        for (int attempt_index = 0; attempt_index < kAttemptsPerChannel; ++attempt_index)
        {
            if (Clock::now() > deadline)
            {
                result.outcome = Outcome::Failed;
                result.message = "下载超时";
                return result;
            }
            Attempt attempt = FetchOnce(channel, part_path, deadline, request.on_progress);
            attempt.url = channel;
            result.attempts.push_back(attempt);
            if (attempt.outcome == Outcome::Ok)
            {
                // 摘要对不上是明确的失败：字节已经收全但内容不对，换通道重试没意义，
                // 换来的只会是同一份被篡改或被截断的包。摘要由 FetchOnce 边下边算，
                // 这里不重读一遍文件：103 MB 的包重读一次要几秒，且会多占一份内存。
                if (attempt.digest == request.installer_sha256)
                {
                    // 正式名字可能已经被另一个先完成的实例写好，且内容相同。这时不再
                    // 覆盖：MoveFileEx 会把旧文件顶掉，而用户可能正读到它。
                    if (FileMatchesDigest(final_path, request.installer_sha256))
                    {
                        std::filesystem::remove(part_path, ec);
                        result.outcome = Outcome::Ok;
                        result.message.clear();
                        result.path = final_path;
                        return result;
                    }
                    std::filesystem::remove(final_path, ec);
                    if (MoveFileExW(part_path.c_str(), final_path.c_str(),
                                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    {
                        result.outcome = Outcome::Ok;
                        result.message.clear();
                        result.path = final_path;
                        return result;
                    }
                    std::filesystem::remove(part_path, ec);
                    result.outcome = Outcome::Failed;
                    result.message = "无法保存安装包";
                    return result;
                }
                std::filesystem::remove(part_path, ec);
                result.outcome = Outcome::Checksum;
                result.message = "安装包校验失败";
                result.attempts.back().message = "SHA256 与清单不符";
                return result;
            }
            if (attempt.outcome == Outcome::NotFound)
            {
                saw_not_found = true;
                // 这一条没有就没有，重试三次也一样。换下一条通道。
                break;
            }
            // 其它失败：同一条通道还有重试机会。静默重试，不打扰用户。
        }
    }
    std::filesystem::remove(part_path, ec);
    result.outcome = Outcome::Failed;
    result.message = saw_not_found ? "下载通道上都没有这个安装包" : "所有下载通道都失败了";
    return result;
}
} // namespace native_settings::update_download
