#include "settings/native/native_settings_update_download.h"

#include "utils/network_proxy.h"

#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <wintrust.h>
#include <softpub.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <limits>
#include <regex>
#include <vector>

namespace native_settings::update_download
{
namespace
{
// 首段测速的窗口与门槛。窗口取 30 秒、门槛取 8 KB/s：真实的家庭宽带和代理在头
// 十几秒还在慢启动，10 秒 50 KB/s 会把「先卡半分钟再飙」的线路判死，而它其实能下。
// 真正死掉的连接由 kStallSeconds 的停滞判定兜住。
constexpr std::uint64_t kProbeSeconds = 30;
constexpr std::uint64_t kProbeMinBytes = 2 * 1024 * 1024;
constexpr std::uint64_t kProbeFloorBytesPerSecond = 8 * 1024;
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

std::uint64_t FileSizeOrZero(const std::filesystem::path &path)
{
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

// 打开文件并在句柄上算 SHA-256。share_mode 由调用方决定：周期性核对用宽松共享，
// 避免杀毒软件之类的临时写句柄让校验假失败；启动安装程序前用只共享读，锁住文件。
bool OpenAndHash(const std::filesystem::path &path, const std::string &expected_hex, DWORD share_mode, HANDLE &file)
{
    file = INVALID_HANDLE_VALUE;
    if (!IsSha256Hex(expected_hex))
        return false;
    HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ, share_mode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
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
        CloseHandle(handle);
        return false;
    }
    std::vector<char> buffer(64 * 1024);
    bool ok = true;
    for (;;)
    {
        DWORD read = 0;
        if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
        {
            ok = false;
            break;
        }
        if (read == 0)
            break;
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), read, 0) != 0)
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
    if (!ok || HexDigest(digest) != expected_hex)
    {
        CloseHandle(handle);
        return false;
    }
    file = handle;
    return true;
}

// Content-Range 里的十进制段。溢出直接判失败，不让一条坏头把长度算歪。
bool ReadUnsigned(const std::wstring &text, size_t &pos, std::uint64_t &value)
{
    const size_t begin = pos;
    value = 0;
    while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9')
    {
        if (value > (std::numeric_limits<std::uint64_t>::max() - 9) / 10)
            return false;
        value = value * 10 + static_cast<std::uint64_t>(text[pos] - L'0');
        ++pos;
    }
    return pos > begin;
}

// 一次尝试。临时文件里已有的字节当作续传前缀，用 Range 从那里接着要；服务器不认
// Range 返回 200 时清空重下。完整性由调用方对整份文件统一核对，这里只管把字节
// 写到磁盘上。
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

    // 断点续传的前缀。只认普通的临时文件；比上限还大说明文件已经不可信，直接丢弃。
    // 已经下完的文件也走这里：Range 会得到 416，按失败处理，调用方核对摘要后要么
    // 直接采用，要么删掉重下。
    std::error_code ec;
    std::uint64_t resume_from = 0;
    if (std::filesystem::is_regular_file(target, ec))
        resume_from = FileSizeOrZero(target);
    if (resume_from > kMaxInstallerBytes)
    {
        std::filesystem::remove(target, ec);
        resume_from = 0;
    }
    attempt.bytes = resume_from;

    if (request)
    {
        // GitHub 的 release 资产会 302 到 CDN，镜像也会跳转，显式放开重定向。
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        if (resume_from > 0)
        {
            const std::wstring range = L"Range: bytes=" + std::to_wstring(resume_from) + L"-";
            WinHttpAddRequestHeaders(request, range.c_str(), static_cast<DWORD>(-1),
                                     WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
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
    // 416 说明本地前缀已经不小于服务器的整包。上次可能已经把全部字节写完，只是尾部
    // 查询报错；这里不急着删，交给调用方对整文件算摘要，匹配就直接用，不匹配再重下。
    if (status == 416)
    {
        attempt.outcome = Outcome::Ok;
        attempt.bytes = resume_from;
        attempt.total = 0;
        attempt.seconds = ElapsedSeconds(started);
        close_all();
        return attempt;
    }
    if (status != HTTP_STATUS_OK && status != HTTP_STATUS_PARTIAL_CONTENT)
    {
        attempt.message = "下载服务器返回了 " + std::to_string(status);
        attempt.seconds = ElapsedSeconds(started);
        close_all();
        return attempt;
    }

    std::uint64_t total = 0;
    bool total_unknown = false;
    if (status == HTTP_STATUS_PARTIAL_CONTENT)
    {
        // 206 必须给出 Content-Range，而且起点要正好接上本地文件；对不上就说明
        // 服务器给的是另一段内容，继续写只会把文件拼坏。WinHTTP 取自定义头要先
        // 问一次长度，再按长度分配缓冲区。
        DWORD range_bytes = 0;
        std::wstring content_range;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Content-Range", WINHTTP_NO_OUTPUT_BUFFER, &range_bytes,
                                WINHTTP_NO_HEADER_INDEX) ||
            GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            if (range_bytes > 0)
            {
                content_range.assign(range_bytes / sizeof(wchar_t), L'\0');
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Content-Range", content_range.data(),
                                        &range_bytes, WINHTTP_NO_HEADER_INDEX))
                    content_range.resize(range_bytes / sizeof(wchar_t));
                else
                    content_range.clear();
            }
        }
        while (!content_range.empty() && content_range.back() == L'\0')
            content_range.pop_back();
        std::uint64_t range_start = 0;
        if (content_range.empty() || !ParseContentRange(content_range, range_start, total) ||
            range_start != resume_from)
        {
            std::filesystem::remove(target, ec);
            attempt.message = "续传响应与本地文件对不上";
            attempt.seconds = ElapsedSeconds(started);
            close_all();
            return attempt;
        }
        // `Content-Range: bytes a-b/*` 表示总长未知。这时 Content-Length 只是剩余
        // 字节数，不能拿它加前缀当整包长度，否则会把一个正常的前缀判成超长。
        total_unknown = total == 0;
    }
    else
    {
        // 服务器没认 Range，返回的是整包：丢掉前缀从头写。
        resume_from = 0;
    }

    DWORD length = 0;
    DWORD length_size = sizeof(length);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size, WINHTTP_NO_HEADER_INDEX))
    {
        if (total == 0 && !total_unknown)
            total = resume_from + length;
    }
    if (total > kMaxInstallerBytes)
    {
        std::filesystem::remove(target, ec);
        attempt.message = "安装包超过允许的大小";
        attempt.seconds = ElapsedSeconds(started);
        close_all();
        return attempt;
    }
    if (total != 0 && resume_from > total)
    {
        // 服务器这次报的整包比本地前缀还短，说明上游换了文件或响应有问题，
        // 不能拿旧前缀继续拼。
        std::filesystem::remove(target, ec);
        attempt.message = "续传文件与服务器长度对不上";
        attempt.seconds = ElapsedSeconds(started);
        close_all();
        return attempt;
    }

    std::ofstream file(target, std::ios::binary | (resume_from > 0 ? std::ios::app : std::ios::trunc));
    if (!file)
    {
        attempt.message = "无法写入下载文件";
        attempt.seconds = ElapsedSeconds(started);
        close_all();
        return attempt;
    }

    std::vector<char> buffer(64 * 1024);
    const Clock::time_point probe_started = Clock::now();
    std::uint64_t received = 0;
    // 只剩一个尾巴时不再测速：不到 2 MB 的剩余量永远凑不够样本，30 秒后按
    // 8 KB/s 判会把一段正常但偏慢的收尾误杀。
    const bool tail_only = total != 0 && resume_from < total && total - resume_from < kProbeMinBytes;
    bool probing = !tail_only;
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
        // 接收超时，靠循环之间的时间差看不出空转。
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
        if (resume_from + received + read > kMaxInstallerBytes)
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
        received += read;
        if (progress)
            progress(resume_from + received, total);

        // 首段测速只统计这次尝试收到的字节，不把续传前缀算进去，否则一个很大的
        // 前缀会让测速还没开始就“样本足够”。
        if (probing)
        {
            const auto probe_ms =
                (std::max)(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - probe_started).count(),
                           std::int64_t{500});
            const bool enough_sample = received >= kProbeMinBytes;
            if (enough_sample || probe_ms >= static_cast<std::int64_t>(kProbeSeconds * 1000))
            {
                if (received * 1000 < kProbeFloorBytesPerSecond * static_cast<std::uint64_t>(probe_ms))
                {
                    attempt.message = "下载速度低于 8 KB/s";
                    ok = false;
                    break;
                }
                probing = false;
            }
        }
    }
    file.close();
    close_all();

    attempt.bytes = resume_from + received;
    attempt.total = total;
    attempt.seconds = ElapsedSeconds(started);
    if (!ok)
    {
        // 网络断了但字节是真的，留在原地给下一次续传；文件超过上限或写盘失败时
        // 内容已经不可信，删掉重来。
        if (attempt.message == "安装包超过允许的大小" || attempt.message == "写入下载文件失败")
            std::filesystem::remove(target, ec);
        return attempt;
    }
    if (total != 0 && attempt.bytes > total)
    {
        std::filesystem::remove(target, ec);
        attempt.message = "下载内容超过声明长度";
        attempt.bytes = 0;
        return attempt;
    }
    if (total != 0 && attempt.bytes < total)
    {
        // 连接正常结束但没给全。留着前缀，下一次 Range 接着要。
        attempt.message = "下载不完整";
        return attempt;
    }
    attempt.outcome = Outcome::Ok;
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

bool ParseContentRange(const std::wstring &value, std::uint64_t &start, std::uint64_t &total)
{
    start = 0;
    total = 0;
    size_t pos = 0;
    const auto skip_space = [&] {
        while (pos < value.size() && (value[pos] == L' ' || value[pos] == L'\t'))
            ++pos;
    };
    const auto literal = [&](const wchar_t *word) {
        for (const wchar_t *ch = word; *ch != L'\0'; ++ch)
        {
            if (pos >= value.size() || std::towlower(value[pos]) != *ch)
                return false;
            ++pos;
        }
        return true;
    };
    skip_space();
    if (!literal(L"bytes"))
        return false;
    skip_space();
    std::uint64_t end = 0;
    if (!ReadUnsigned(value, pos, start) || pos >= value.size() || value[pos] != L'-')
        return false;
    ++pos;
    if (!ReadUnsigned(value, pos, end) || pos >= value.size() || value[pos] != L'/')
        return false;
    ++pos;
    skip_space();
    if (pos < value.size() && value[pos] == L'*')
        ++pos;
    else if (!ReadUnsigned(value, pos, total))
        return false;
    skip_space();
    // 起点不能大于终点；`*` 表示总长未知，total 保持 0。
    return pos == value.size() && end >= start;
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

bool OpenVerifiedInstaller(const std::filesystem::path &path, const std::string &expected_hex, HANDLE &file)
{
    // 只共享读：从校验完成到安装程序真正打开文件之间，其他进程拿不到写权限，
    // 改不了内容也删不掉。句柄要由调用方在启动之后再关闭。
    return OpenAndHash(path, expected_hex, FILE_SHARE_READ, file);
}

bool FileMatchesDigest(const std::filesystem::path &path, const std::string &expected_hex)
{
    // 这一份只做判断，不拿去启动程序，共享模式放宽，别让一个临时的写句柄
    // 或者杀毒扫描把正常文件判成坏的。
    HANDLE file = INVALID_HANDLE_VALUE;
    const bool ok = OpenAndHash(path, expected_hex, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, file);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    return ok;
}

bool VerifyAuthenticode(const std::filesystem::path &path)
{
    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    // 离线也能用：不查吊销，只验证签名和本机信任链。
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file_info;
    // 不查吊销，也不让信任链构建去联网取证书。离线机器上不能因为一次更新检查
    // 卡在网络超时上；本机没有的中间证书会直接判失败，宁可让用户重新下载。
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return status == ERROR_SUCCESS;
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

    for (const std::string &channel : channels)
    {
        for (int attempt_index = 0; attempt_index < kAttemptsPerChannel; ++attempt_index)
        {
            if (Clock::now() > deadline)
            {
                std::filesystem::remove(part_path, ec);
                result.outcome = Outcome::Failed;
                result.message = "下载超时";
                return result;
            }
            Attempt attempt = FetchOnce(channel, part_path, deadline, request.on_progress);
            attempt.url = channel;
            result.attempts.push_back(attempt);
            if (attempt.outcome == Outcome::Ok)
            {
                // 完整性只认整份文件的 SHA256。续传把多次连接的字节拼在同一个临时
                // 文件里，边下边算的哈希跨不过重连，所以这里统一读一遍再决定。
                if (FileMatchesDigest(part_path, request.installer_sha256))
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
                // 摘要不对：可能是续传的前缀来自一条坏连接，也可能是上游内容变了。
                // 丢掉临时文件，下一次从零重下；最后一次尝试仍是这个结论才判校验失败。
                std::filesystem::remove(part_path, ec);
                result.attempts.back().message = "SHA256 与清单不符";
                continue;
            }
            if (attempt.outcome == Outcome::NotFound)
            {
                // 这一条没有就没有，重试三次也一样。换下一条通道。
                break;
            }
            // 其它失败：同一条通道还有重试机会。临时文件留着，下一次用 Range 续传。
        }
    }
    std::filesystem::remove(part_path, ec);
    // 只按最后一次尝试的结局下结论。之前出现过一次摘要不符、后面只是断网时，
    // 不能把网络问题报成「安装包校验失败」这种安全结论。
    const bool last_checksum = !result.attempts.empty() && result.attempts.back().message == "SHA256 与清单不符";
    const bool all_not_found = !result.attempts.empty() &&
                               std::all_of(result.attempts.begin(), result.attempts.end(),
                                           [](const Attempt &attempt) { return attempt.outcome == Outcome::NotFound; });
    result.outcome = last_checksum ? Outcome::Checksum : Outcome::Failed;
    if (last_checksum)
        result.message = "安装包校验失败";
    else if (all_not_found)
        result.message = "下载通道上都没有这个安装包";
    else
    {
        result.message = "所有下载通道都失败了";
        // 把最后一次尝试的原因带上：用户看到的就是「速度低」「连不上」这类具体
        // 结论，而不是一句没有信息量的总失败。
        if (!result.attempts.empty() && !result.attempts.back().message.empty())
            result.message += "：" + result.attempts.back().message;
    }
    return result;
}
} // namespace native_settings::update_download
