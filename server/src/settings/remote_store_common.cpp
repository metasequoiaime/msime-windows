#include "settings/remote_store_internal.h"

#include "utils/network_proxy.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <mutex>

#pragma comment(lib, "bcrypt.lib")

namespace user_data_sync::detail
{
namespace
{
std::once_flag g_curl_init;

void InitCurlOnce()
{
    std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct UploadContext
{
    std::FILE *file = nullptr;
    std::uint64_t total = 0;
    std::uint64_t sent = 0;
    const ProgressCallback *progress = nullptr;
    std::string error;
};

struct DownloadContext
{
    std::FILE *file = nullptr;
    std::uint64_t total = 0;
    std::uint64_t received = 0;
    const ProgressCallback *progress = nullptr;
    std::string error;
    // 响应体也要收：S3 的错误 XML 就在这里，只有状态码看不出原因。
    std::string *capture = nullptr;
    std::size_t capture_limit = 0;
    std::string *headers = nullptr;
};

// curl_slist 必须活到 curl_easy_perform 返回之后，用 RAII 保证调用点不用记着释放。
struct HeaderList
{
    curl_slist *list = nullptr;
    ~HeaderList()
    {
        if (list)
            curl_slist_free_all(list);
    }
    HeaderList(const HeaderList &) = delete;
    HeaderList &operator=(const HeaderList &) = delete;
    HeaderList() = default;
};

std::size_t ReadCallback(char *buffer, std::size_t size, std::size_t count, void *user)
{
    auto *context = static_cast<UploadContext *>(user);
    const std::size_t wanted = size * count;
    if (wanted == 0)
        return 0;
    const std::size_t read = std::fread(buffer, 1, wanted, context->file);
    if (read > 0)
    {
        context->sent += read;
        if (context->progress && *context->progress)
            (*context->progress)({context->sent, context->total});
    }
    if (read < wanted && std::ferror(context->file))
        context->error = "读取待上传文件失败";
    return read;
}

std::size_t WriteCallback(char *data, std::size_t size, std::size_t count, void *user)
{
    auto *context = static_cast<DownloadContext *>(user);
    const std::size_t bytes = size * count;
    if (bytes == 0)
        return 0;
    if (context->file && std::fwrite(data, 1, bytes, context->file) != bytes)
    {
        context->error = "写入下载文件失败";
        return 0;
    }
    if (context->capture && context->capture->size() < context->capture_limit)
    {
        const std::size_t room = context->capture_limit - context->capture->size();
        context->capture->append(data, (std::min)(bytes, room));
    }
    context->received += bytes;
    if (context->progress && *context->progress)
        (*context->progress)({context->received, context->total});
    return bytes;
}

std::size_t HeaderCallback(char *data, std::size_t size, std::size_t count, void *user)
{
    auto *context = static_cast<DownloadContext *>(user);
    const std::size_t bytes = size * count;
    if (context->headers && bytes > 0)
        context->headers->append(data, bytes);
    return bytes;
}
} // namespace

bool IsSupportedUrl(const std::string &url)
{
    return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
}

bool SplitUrl(const std::string &url, std::string &scheme, std::string &host, std::string &port, std::string &path)
{
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        return false;
    scheme = url.substr(0, scheme_end);
    if (scheme != "https" && scheme != "http")
        return false;
    const std::size_t authority_begin = scheme_end + 3;
    const std::size_t authority_end = url.find('/', authority_begin);
    const std::string authority = url.substr(
        authority_begin, authority_end == std::string::npos ? std::string::npos : authority_end - authority_begin);
    path = authority_end == std::string::npos ? std::string("/") : url.substr(authority_end);
    if (authority.empty())
        return false;
    const std::size_t colon = authority.rfind(':');
    // 方括号 IPv6 里也有冒号，只在 ] 之后（或没有方括号）才把冒号当端口分隔。
    // authority.find(']') 在没有方括号时是 npos，npos < colon 恒为 false，所以必须先单独判掉。
    const std::size_t bracket = authority.find(']');
    const bool has_port = colon != std::string::npos && (bracket == std::string::npos || bracket < colon);
    if (has_port)
    {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    else
    {
        host = authority;
        port = scheme == "https" ? "443" : "80";
    }
    return !host.empty() && !port.empty();
}

std::string UriEncode(const std::string &value, bool encode_slash)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const unsigned char ch : value)
    {
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                                ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (unreserved || (!encode_slash && ch == '/'))
        {
            out.push_back(static_cast<char>(ch));
        }
        else
        {
            out.push_back('%');
            out.push_back(kHex[(ch >> 4) & 0x0F]);
            out.push_back(kHex[ch & 0x0F]);
        }
    }
    return out;
}

std::string JoinUrl(const std::string &base_url, const std::string &key)
{
    std::string base = base_url;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    if (key.empty())
        return base;
    return base + "/" + UriEncode(key, false);
}

std::string CanonicalQuery(const std::vector<std::pair<std::string, std::string>> &params)
{
    std::vector<std::pair<std::string, std::string>> sorted;
    sorted.reserve(params.size());
    for (const auto &item : params)
        sorted.emplace_back(UriEncode(item.first, true), UriEncode(item.second, true));
    std::sort(sorted.begin(), sorted.end());
    std::string out;
    for (const auto &item : sorted)
    {
        if (!out.empty())
            out.push_back('&');
        out += item.first;
        out.push_back('=');
        out += item.second;
    }
    return out;
}

std::string HexEncode(const unsigned char *data, std::size_t size)
{
    static const char kHex[] = "0123456789abcdef";
    std::string out(size * 2, '\0');
    for (std::size_t i = 0; i < size; ++i)
    {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return out;
}

bool Sha256(std::string_view data, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;
    const NTSTATUS created = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    const NTSTATUS hashed = created == 0
                                ? BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
                                                 static_cast<ULONG>(data.size()), 0)
                                : created;
    const NTSTATUS finished = hashed == 0 ? BCryptFinishHash(hash, out, 32, 0) : hashed;
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    return finished == 0;
}

bool HmacSha256(std::string_view key, std::string_view data, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return false;
    const NTSTATUS created =
        BCryptCreateHash(alg, &hash, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(key.data())),
                         static_cast<ULONG>(key.size()), 0);
    const NTSTATUS hashed = created == 0
                                ? BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
                                                 static_cast<ULONG>(data.size()), 0)
                                : created;
    const NTSTATUS finished = hashed == 0 ? BCryptFinishHash(hash, out, 32, 0) : hashed;
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    return finished == 0;
}

std::string HmacSha256Raw(std::string_view key, std::string_view data)
{
    unsigned char digest[32]{};
    if (!HmacSha256(key, data, digest))
        return {};
    return std::string(reinterpret_cast<const char *>(digest), 32);
}

bool IsSuccess(long status)
{
    return status >= 200 && status < 300;
}

bool IsNotFound(long status)
{
    return status == 404;
}

bool IsTransient(CURLcode code)
{
    switch (code)
    {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_SSL_CONNECT_ERROR:
        return true;
    default:
        return false;
    }
}

bool IsRetryableStatus(long status)
{
    // 5xx 是服务端临时故障；429 是限流，S3 在请求突增时会给它，退避后通常能过。
    return status >= 500 || status == 429;
}

bool Retry(const Attempt &attempt, std::string &message)
{
    constexpr int kAttempts = 3;
    for (int index = 0; index < kAttempts; ++index)
    {
        std::string attempt_message;
        const AttemptResult result = attempt(attempt_message);
        if (result == AttemptResult::Ok)
            return true;
        message = attempt_message;
        if (result == AttemptResult::Fatal)
            return false;
        // 最后一次失败后不要再等：退避只在还有下一次尝试时才有意义。
        if (index + 1 < kAttempts)
            Sleep(static_cast<DWORD>(200 * (index + 1) * (index + 1)));
    }
    return false;
}

std::string HeaderValue(const std::string &headers, const std::string &name)
{
    std::string lower_name = name;
    for (char &ch : lower_name)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    std::string found;
    std::size_t position = 0;
    while (position < headers.size())
    {
        const std::size_t end = headers.find("\r\n", position);
        const std::string line =
            headers.substr(position, end == std::string::npos ? std::string::npos : end - position);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos)
        {
            std::string key = line.substr(0, colon);
            for (char &ch : key)
            {
                if (ch >= 'A' && ch <= 'Z')
                    ch = static_cast<char>(ch - 'A' + 'a');
            }
            if (key == lower_name)
            {
                std::string value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    value.erase(value.begin());
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                    value.pop_back();
                found = value;
            }
        }
        if (end == std::string::npos)
            break;
        position = end + 2;
    }
    return found;
}

std::string DescribeHttpFailure(const HttpResponse &response)
{
    if (response.code != CURLE_OK)
    {
        if (!response.error.empty())
            return response.error;
        return std::string(curl_easy_strerror(response.code));
    }
    std::string detail = "HTTP " + std::to_string(response.status);
    if (!response.body.empty())
    {
        // S3 的错误是 XML，WebDAV 是 HTML；截一段就够定位，不要把它整块塞进提示。
        std::string snippet = response.body.substr(0, 400);
        for (char &ch : snippet)
        {
            if (ch == '\r' || ch == '\n' || ch == '\t')
                ch = ' ';
        }
        while (!snippet.empty() && snippet.back() == ' ')
            snippet.pop_back();
        if (!snippet.empty())
            detail += "：" + snippet;
    }
    return detail;
}

HttpResponse Perform(const HttpRequest &request, const ProgressCallback &progress)
{
    InitCurlOnce();
    HttpResponse response;
    if (!IsSupportedUrl(request.url))
    {
        response.error = "只支持 http 与 https 地址";
        return response;
    }
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        response.error = "无法初始化网络会话";
        return response;
    }
    char error_buffer[CURL_ERROR_SIZE] = {};
    HeaderList headers;
    for (const std::string &header : request.headers)
    {
        // curl_slist_append 失败会返回 null，直接赋回去就把已有链表丢了。
        curl_slist *appended = curl_slist_append(headers.list, header.c_str());
        if (!appended)
        {
            response.error = "无法构造请求头";
            curl_easy_cleanup(curl);
            return response;
        }
        headers.list = appended;
    }

    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, request.follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    // TLS 校验始终开启。自签证书的服务端应当把证书导入系统信任库。
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // 需要自定义方法的动词（PROPFIND / MKCOL / DELETE）都走 CUSTOMREQUEST；PUT 交给 UPLOAD
    // 模式，POST 交给 POSTFIELDS，HEAD 交给 NOBODY，三者不设 CUSTOMREQUEST。
    if (request.method == "HEAD")
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    else if (request.method != "GET" && request.method != "PUT" && request.method != "POST")
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    if (headers.list)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.list);

    UploadContext upload;
    DownloadContext download;
    std::FILE *upload_file = nullptr;
    std::FILE *download_file = nullptr;

    if (!request.upload_file.empty())
    {
        upload_file = _wfopen(request.upload_file.c_str(), L"rb");
        if (!upload_file)
        {
            response.error = "无法打开待上传文件";
            curl_easy_cleanup(curl);
            return response;
        }
        std::error_code ec;
        upload.total = std::filesystem::file_size(request.upload_file, ec);
        if (ec)
            upload.total = 0;
        upload.file = upload_file;
        upload.progress = &progress;
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadCallback);
        curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
        if (upload.total > 0)
            curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(upload.total));
    }
    else if (request.method == "POST" || !request.body.empty())
    {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }

    if (!request.download_file.empty())
    {
        download_file = _wfopen(request.download_file.c_str(), L"wb");
        if (!download_file)
        {
            response.error = "无法创建下载文件";
            if (upload_file)
                std::fclose(upload_file);
            curl_easy_cleanup(curl);
            return response;
        }
        download.file = download_file;
        download.progress = &progress;
        download.capture = &response.body;
        download.capture_limit = 64 * 1024;
    }
    else
    {
        download.capture = &response.body;
        download.capture_limit = 16 * 1024 * 1024;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
    if (request.capture_headers)
    {
        download.headers = &response.headers;
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &download);
    }
    response.code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    if (response.code != CURLE_OK)
        response.error = error_buffer[0] ? std::string(error_buffer) : std::string(curl_easy_strerror(response.code));
    if (!upload.error.empty())
        response.error = upload.error;
    if (!download.error.empty())
        response.error = download.error;

    curl_easy_cleanup(curl);
    if (upload_file)
        std::fclose(upload_file);
    if (download_file)
        std::fclose(download_file);
    return response;
}
} // namespace user_data_sync::detail
