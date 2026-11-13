#include "settings/remote_store_internal.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace user_data_sync
{
namespace
{
using detail::Attempt;
using detail::AttemptResult;
using detail::CanonicalQuery;
using detail::DescribeHttpFailure;
using detail::HeaderValue;
using detail::HexEncode;
using detail::HmacSha256;
using detail::HmacSha256Raw;
using detail::HttpRequest;
using detail::HttpResponse;
using detail::IsNotFound;
using detail::IsRetryableStatus;
using detail::IsSuccess;
using detail::IsTransient;
using detail::Perform;
using detail::Retry;
using detail::Sha256;
using detail::SplitUrl;
using detail::UriEncode;

constexpr char kAlgorithm[] = "AWS4-HMAC-SHA256";

std::string Sha256Hex(std::string_view data)
{
    unsigned char digest[32]{};
    return Sha256(data, digest) ? HexEncode(digest, 32) : std::string{};
}

std::string UtcStamp(std::int64_t unix_time)
{
    const std::time_t stamp = static_cast<std::time_t>(unix_time);
    std::tm utc{};
    gmtime_s(&utc, &stamp);
    char buffer[24]{};
    std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc);
    return buffer;
}

std::string UtcDate(std::int64_t unix_time)
{
    const std::time_t stamp = static_cast<std::time_t>(unix_time);
    std::tm utc{};
    gmtime_s(&utc, &stamp);
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d", &utc);
    return buffer;
}

// canonical_headers 里每个值的首尾空白要去掉，内部连续空白压成一个空格。
std::string TrimHeaderValue(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    bool pending_space = false;
    bool started = false;
    for (const char ch : value)
    {
        if (ch == ' ' || ch == '\t')
        {
            pending_space = started;
            continue;
        }
        if (pending_space)
            out.push_back(' ');
        pending_space = false;
        started = true;
        out.push_back(ch);
    }
    return out;
}

bool IsDefaultPort(const std::string &scheme, const std::string &port)
{
    return (scheme == "https" && port == "443") || (scheme == "http" && port == "80");
}

// 服务端的 host 与请求实际发往的 host 必须一致，否则签名校验失败。
std::string RequestHost(const S3Options &options, const std::string &host, const std::string &port,
                        const std::string &scheme)
{
    std::string authority = options.path_style ? host : options.bucket + "." + host;
    if (!IsDefaultPort(scheme, port))
        authority += ":" + port;
    return authority;
}

// canonical_uri：路径风格是 /bucket/key，虚拟主机风格是 /key。每一段都要再编码一次。
std::string CanonicalUri(const S3Options &options, const std::string &key)
{
    std::string uri;
    if (options.path_style)
        uri = "/" + options.bucket;
    if (!key.empty())
        uri += "/" + UriEncode(key, false);
    else if (uri.empty())
        uri = "/";
    return uri;
}

struct SignedRequest
{
    std::string url;
    std::vector<std::string> headers;
    bool ok = false;
    std::string message;
};

// 组装一条已签名的请求。payload_hash 由调用方给：PUT 用文件摘要，其余用空串摘要。
// 签名本身走 S3Sign——那是被 AWS 官方测试向量验证过的那一份，这里只负责拼 URL 与头。
SignedRequest BuildSignedRequest(const S3Options &options, const std::string &method, const std::string &key,
                                 const std::vector<std::pair<std::string, std::string>> &query,
                                 const std::string &payload_hash, std::int64_t now)
{
    SignedRequest result;
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    if (!SplitUrl(options.endpoint, scheme, host, port, path))
    {
        result.message = "S3 服务地址无效";
        return result;
    }
    if (options.bucket.empty())
    {
        result.message = "未填写存储桶名称";
        return result;
    }
    if (options.access_key.empty() || options.secret_key.empty())
    {
        result.message = "未填写 Access Key 与 Secret Key";
        return result;
    }

    const std::string canonical_uri = CanonicalUri(options, key);
    const std::string canonical_query = CanonicalQuery(query);
    const std::string authority = RequestHost(options, host, port, scheme);

    const std::vector<std::pair<std::string, std::string>> signed_headers = {
        {"host", authority},
        {"x-amz-content-sha256", payload_hash},
        {"x-amz-date", UtcStamp(now)},
    };
    const S3Signature signature =
        S3Sign(options, method, canonical_uri, canonical_query, signed_headers, payload_hash, now);
    if (signature.signature.empty())
    {
        result.message = "无法计算请求签名";
        return result;
    }

    std::string url = scheme + "://";
    url += options.path_style ? host : options.bucket + "." + host;
    if (!IsDefaultPort(scheme, port))
        url += ":" + port;
    url += canonical_uri;
    if (!canonical_query.empty())
        url += "?" + canonical_query;

    result.url = std::move(url);
    result.headers.push_back("x-amz-date: " + signature.amz_date);
    result.headers.push_back("x-amz-content-sha256: " + payload_hash);
    result.headers.push_back("Authorization: " + signature.authorization);
    result.ok = true;
    return result;
}

std::string NormalizedPrefix(const S3Options &options)
{
    std::string prefix = options.prefix;
    while (!prefix.empty() && prefix.front() == '/')
        prefix.erase(prefix.begin());
    while (!prefix.empty() && prefix.back() == '/')
        prefix.pop_back();
    return prefix;
}

std::string PrefixedKey(const S3Options &options, const std::string &key)
{
    const std::string prefix = NormalizedPrefix(options);
    if (prefix.empty())
        return key;
    return key.empty() ? prefix : prefix + "/" + key;
}

std::string StripPrefix(const S3Options &options, const std::string &key)
{
    const std::string prefix = NormalizedPrefix(options);
    if (prefix.empty())
        return key;
    if (key == prefix)
        return {};
    if (key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0 && key[prefix.size()] == '/')
        return key.substr(prefix.size() + 1);
    return key;
}

// 极简 XML 取值。S3 的 ListObjectsV2 与错误响应结构固定，不需要通用解析器；
// 这里只按标签切，遇到同名嵌套也不会误取（S3 的响应里没有这种形状）。
std::vector<std::string> Blocks(const std::string &xml, const std::string &tag)
{
    std::vector<std::string> values;
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    std::size_t position = 0;
    while ((position = xml.find(open, position)) != std::string::npos)
    {
        const std::size_t begin = position + open.size();
        const std::size_t end = xml.find(close, begin);
        if (end == std::string::npos)
            break;
        values.push_back(xml.substr(begin, end - begin));
        position = end + close.size();
    }
    return values;
}

std::string FirstElement(const std::string &xml, const std::string &tag)
{
    const std::vector<std::string> values = Blocks(xml, tag);
    return values.empty() ? std::string() : values.front();
}

std::string XmlUnescape(const std::string &value)
{
    return detail::XmlDecodeEntities(value);
}

// S3 请求不跟随重定向（见 HttpRequest::follow_redirects），3xx 原样落到这里。最常见的原因是
// 区域填错：AWS 会在 x-amz-bucket-region 里给出正确的区域，path-style 的 301 连 Location 都没有。
std::string DescribeS3Failure(const HttpResponse &response)
{
    if (response.status >= 300 && response.status < 400)
    {
        std::string message = "存储桶不在所填的区域，或服务地址不对（HTTP " + std::to_string(response.status) + "）";
        const std::string region = HeaderValue(response.headers, "x-amz-bucket-region");
        if (!region.empty())
            message += "，服务端提示区域应为 " + region;
        return message;
    }
    return DescribeHttpFailure(response);
}

std::int64_t ParseIso8601(const std::string &text)
{
    // 2024-05-24T00:00:00.000Z
    std::tm utc{};
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
        return 0;
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    return static_cast<std::int64_t>(_mkgmtime(&utc));
}

std::int64_t ParseHttpDate(const std::string &text)
{
    // Sun, 06 Nov 1994 08:49:37 GMT
    std::tm utc{};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    char month[4]{};
    if (std::sscanf(text.c_str(), "%*3s, %d %3s %d %d:%d:%d", &day, month, &year, &hour, &minute, &second) != 6)
        return 0;
    static const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int month_index = -1;
    for (int i = 0; i < 12; ++i)
    {
        if (_stricmp(month, kMonths[i]) == 0)
        {
            month_index = i;
            break;
        }
    }
    if (month_index < 0)
        return 0;
    utc.tm_year = year - 1900;
    utc.tm_mon = month_index;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    return static_cast<std::int64_t>(_mkgmtime(&utc));
}

class S3Store final : public RemoteStore
{
  public:
    explicit S3Store(S3Options options) : options_(std::move(options))
    {
    }

    bool Put(const std::string &key, const std::filesystem::path &file, const ProgressCallback &progress,
             std::string &message) override
    {
        const std::string digest = Sha256FileHex(file);
        if (digest.empty())
        {
            message = "无法读取待上传文件：" + file.u8string();
            return false;
        }
        const std::string object_key = PrefixedKey(options_, key);
        return Retry(
            [&](std::string &attempt_message) {
                const SignedRequest signed_request =
                    BuildSignedRequest(options_, "PUT", object_key, {}, digest, std::time(nullptr));
                if (!signed_request.ok)
                {
                    attempt_message = signed_request.message;
                    return AttemptResult::Fatal;
                }
                HttpRequest request;
                request.method = "PUT";
                request.url = signed_request.url;
                request.headers = signed_request.headers;
                request.follow_redirects = false;
                request.capture_headers = true;
                request.headers.push_back("Content-Type: application/octet-stream");
                request.upload_file = file;
                request.timeout_seconds = 0;
                const HttpResponse response = Perform(request, progress);
                if (IsSuccess(response.status))
                    return AttemptResult::Ok;
                attempt_message = DescribeS3Failure(response);
                return IsTransient(response.code) || IsRetryableStatus(response.status) ? AttemptResult::Retry
                                                                                        : AttemptResult::Fatal;
            },
            message);
    }

    bool Get(const std::string &key, const std::filesystem::path &file, const ProgressCallback &progress,
             std::string &message) override
    {
        const std::string object_key = PrefixedKey(options_, key);
        return Retry(
            [&](std::string &attempt_message) {
                const SignedRequest signed_request =
                    BuildSignedRequest(options_, "GET", object_key, {}, EmptyPayloadSha256(), std::time(nullptr));
                if (!signed_request.ok)
                {
                    attempt_message = signed_request.message;
                    return AttemptResult::Fatal;
                }
                HttpRequest request;
                request.method = "GET";
                request.url = signed_request.url;
                request.headers = signed_request.headers;
                request.follow_redirects = false;
                request.capture_headers = true;
                request.download_file = file;
                request.timeout_seconds = 0;
                const HttpResponse response = Perform(request, progress);
                if (IsSuccess(response.status))
                    return AttemptResult::Ok;
                attempt_message = IsNotFound(response.status) ? "云端没有这个文件" : DescribeS3Failure(response);
                return IsTransient(response.code) || IsRetryableStatus(response.status) ? AttemptResult::Retry
                                                                                        : AttemptResult::Fatal;
            },
            message);
    }

    bool Stat(const std::string &key, RemoteObject &object, std::string &message) override
    {
        const std::string object_key = PrefixedKey(options_, key);
        bool found = false;
        const bool ok = Retry(
            [&](std::string &attempt_message) {
                const SignedRequest signed_request =
                    BuildSignedRequest(options_, "HEAD", object_key, {}, EmptyPayloadSha256(), std::time(nullptr));
                if (!signed_request.ok)
                {
                    attempt_message = signed_request.message;
                    return AttemptResult::Fatal;
                }
                HttpRequest request;
                request.method = "HEAD";
                request.url = signed_request.url;
                request.headers = signed_request.headers;
                request.follow_redirects = false;
                request.capture_headers = true;
                request.timeout_seconds = 60;
                const HttpResponse response = Perform(request);
                if (IsNotFound(response.status))
                {
                    // 不存在不是错误：调用方据此决定"要不要上传"，message 必须留空。
                    found = false;
                    attempt_message.clear();
                    return AttemptResult::Ok;
                }
                if (IsSuccess(response.status))
                {
                    found = true;
                    object.key = key;
                    const std::string length = HeaderValue(response.headers, "content-length");
                    object.size =
                        length.empty() ? 0 : static_cast<std::uint64_t>(std::strtoull(length.c_str(), nullptr, 10));
                    object.etag = HeaderValue(response.headers, "etag");
                    object.modified_unix = ParseHttpDate(HeaderValue(response.headers, "last-modified"));
                    return AttemptResult::Ok;
                }
                attempt_message = DescribeS3Failure(response);
                return IsTransient(response.code) || IsRetryableStatus(response.status) ? AttemptResult::Retry
                                                                                        : AttemptResult::Fatal;
            },
            message);
        return ok && found;
    }

    // 语义与 WebDAV 的 PROPFIND Depth: 1 对齐：只列 prefix 这一"层"的直接子对象。
    // 前缀补上尾斜杠，否则 devices 会连 devices-old/… 一起匹配；带 delimiter=/，否则
    // devices 会把每台设备 devices/<id>/ 下的快照也递归列出来。
    bool List(const std::string &prefix, std::vector<RemoteObject> &objects, std::string &message) override
    {
        objects.clear();
        std::string full_prefix = PrefixedKey(options_, prefix);
        if (!full_prefix.empty() && full_prefix.back() != '/')
            full_prefix.push_back('/');
        std::string continuation;
        for (int page = 0; page < 1000; ++page)
        {
            std::string next;
            bool truncated = false;
            if (!ListPage(full_prefix, continuation, 1000, objects, next, truncated, message))
                return false;
            if (!truncated || next.empty())
                return true;
            continuation = next;
        }
        message = "云端文件过多，已停止列举";
        return false;
    }

    bool Delete(const std::string &key, std::string &message) override
    {
        const std::string object_key = PrefixedKey(options_, key);
        return Retry(
            [&](std::string &attempt_message) {
                const SignedRequest signed_request =
                    BuildSignedRequest(options_, "DELETE", object_key, {}, EmptyPayloadSha256(), std::time(nullptr));
                if (!signed_request.ok)
                {
                    attempt_message = signed_request.message;
                    return AttemptResult::Fatal;
                }
                HttpRequest request;
                request.method = "DELETE";
                request.url = signed_request.url;
                request.headers = signed_request.headers;
                request.follow_redirects = false;
                request.capture_headers = true;
                request.timeout_seconds = 60;
                const HttpResponse response = Perform(request);
                // S3 的 DELETE 是幂等的：不存在也回 204。
                if (IsSuccess(response.status) || IsNotFound(response.status))
                    return AttemptResult::Ok;
                attempt_message = DescribeS3Failure(response);
                return IsTransient(response.code) || IsRetryableStatus(response.status) ? AttemptResult::Retry
                                                                                        : AttemptResult::Fatal;
            },
            message);
    }

    // 测试连接只要一页、一个对象：桶里文件多时完整 List 要翻成百上千页。
    bool Probe(std::string &message) override
    {
        std::vector<RemoteObject> ignored;
        std::string next;
        bool truncated = false;
        std::string full_prefix = PrefixedKey(options_, std::string());
        if (!full_prefix.empty())
            full_prefix.push_back('/');
        return ListPage(full_prefix, std::string(), 1, ignored, next, truncated, message);
    }

    // S3 没有目录：对象 key 里的斜杠只是名字的一部分。
    bool EnsureDirectory(const std::string &, std::string &) override
    {
        return true;
    }

  private:
    bool ListPage(const std::string &full_prefix, const std::string &continuation, int max_keys,
                  std::vector<RemoteObject> &objects, std::string &next, bool &truncated, std::string &message)
    {
        std::string page_message;
        const bool listed = Retry(
            [&](std::string &attempt_message) {
                std::vector<std::pair<std::string, std::string>> query = {
                    {"list-type", "2"}, {"max-keys", std::to_string(max_keys)}, {"delimiter", "/"}};
                if (!full_prefix.empty())
                    query.emplace_back("prefix", full_prefix);
                if (!continuation.empty())
                    query.emplace_back("continuation-token", continuation);
                const SignedRequest signed_request =
                    BuildSignedRequest(options_, "GET", {}, query, EmptyPayloadSha256(), std::time(nullptr));
                if (!signed_request.ok)
                {
                    attempt_message = signed_request.message;
                    return AttemptResult::Fatal;
                }
                HttpRequest request;
                request.method = "GET";
                request.url = signed_request.url;
                request.headers = signed_request.headers;
                request.follow_redirects = false;
                request.capture_headers = true;
                request.timeout_seconds = 120;
                const HttpResponse response = Perform(request);
                if (!IsSuccess(response.status))
                {
                    attempt_message = DescribeS3Failure(response);
                    return IsTransient(response.code) || IsRetryableStatus(response.status) ? AttemptResult::Retry
                                                                                            : AttemptResult::Fatal;
                }
                // 重试的那一次不能把上一次已经收进来的对象再收一遍：先收到局部，成功再并入。
                std::vector<RemoteObject> page_objects;
                // <Contents> 里各有 Key / Size / ETag / LastModified，按块切再逐块取。
                // CommonPrefixes 是"子目录"，与 WebDAV 跳过集合一样不算对象。
                for (const std::string &block : Blocks(response.body, "Contents"))
                {
                    RemoteObject object;
                    object.key = StripPrefix(options_, XmlUnescape(FirstElement(block, "Key")));
                    const std::string size = FirstElement(block, "Size");
                    object.size =
                        size.empty() ? 0 : static_cast<std::uint64_t>(std::strtoull(size.c_str(), nullptr, 10));
                    object.etag = XmlUnescape(FirstElement(block, "ETag"));
                    object.modified_unix = ParseIso8601(FirstElement(block, "LastModified"));
                    if (!object.key.empty())
                        page_objects.push_back(std::move(object));
                }
                objects.insert(objects.end(), std::make_move_iterator(page_objects.begin()),
                               std::make_move_iterator(page_objects.end()));
                next = XmlUnescape(FirstElement(response.body, "NextContinuationToken"));
                truncated = FirstElement(response.body, "IsTruncated") == "true";
                return AttemptResult::Ok;
            },
            page_message);
        if (!listed)
            message = page_message.empty() ? "列举云端文件失败" : page_message;
        return listed;
    }

    S3Options options_;
};

} // namespace

S3Signature S3Sign(const S3Options &options, const std::string &method, const std::string &canonical_uri,
                   const std::string &canonical_query,
                   const std::vector<std::pair<std::string, std::string>> &signed_headers,
                   const std::string &payload_sha256_hex, std::int64_t unix_time)
{
    S3Signature signature;
    const std::string amz_date = UtcStamp(unix_time);
    const std::string date = UtcDate(unix_time);

    std::vector<std::pair<std::string, std::string>> headers = signed_headers;
    std::sort(headers.begin(), headers.end());

    std::string canonical_headers;
    std::string signed_names;
    for (const auto &header : headers)
    {
        canonical_headers += header.first + ":" + TrimHeaderValue(header.second) + "\n";
        if (!signed_names.empty())
            signed_names.push_back(';');
        signed_names += header.first;
    }

    signature.amz_date = amz_date;
    signature.payload_hash = payload_sha256_hex;
    signature.canonical_request = method + "\n" + canonical_uri + "\n" + canonical_query + "\n" + canonical_headers +
                                  "\n" + signed_names + "\n" + payload_sha256_hex;

    const std::string scope = date + "/" + options.region + "/s3/aws4_request";
    signature.string_to_sign =
        std::string(kAlgorithm) + "\n" + amz_date + "\n" + scope + "\n" + Sha256Hex(signature.canonical_request);

    const std::string secret_date = HmacSha256Raw("AWS4" + options.secret_key, date);
    const std::string secret_region = HmacSha256Raw(secret_date, options.region);
    const std::string secret_service = HmacSha256Raw(secret_region, "s3");
    const std::string secret_signing = HmacSha256Raw(secret_service, "aws4_request");
    unsigned char digest[32]{};
    if (secret_signing.empty() || !HmacSha256(secret_signing, signature.string_to_sign, digest))
        return signature;
    signature.signature = HexEncode(digest, 32);
    signature.authorization = std::string(kAlgorithm) + " Credential=" + options.access_key + "/" + scope +
                              ", SignedHeaders=" + signed_names + ", Signature=" + signature.signature;
    return signature;
}

std::string Sha256FileHex(const std::filesystem::path &file)
{
    // 归档动辄几百 MB，读整个文件进内存不现实，所以流式喂给 BCrypt。
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    std::FILE *input = _wfopen(file.c_str(), L"rb");
    if (!input)
    {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    std::vector<unsigned char> buffer(256 * 1024);
    bool ok = true;
    for (;;)
    {
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), input);
        if (read > 0 && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(read), 0) != 0)
        {
            ok = false;
            break;
        }
        if (read < buffer.size())
        {
            if (std::ferror(input))
                ok = false;
            break;
        }
    }
    std::fclose(input);
    unsigned char digest[32]{};
    if (ok && BCryptFinishHash(hash, digest, 32, 0) != 0)
        ok = false;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok ? HexEncode(digest, 32) : std::string();
}

std::string EmptyPayloadSha256()
{
    return Sha256Hex(std::string_view());
}

std::unique_ptr<RemoteStore> MakeS3Store(S3Options options)
{
    return std::make_unique<S3Store>(std::move(options));
}
} // namespace user_data_sync
