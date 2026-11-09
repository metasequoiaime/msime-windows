#pragma once

// remote_store 各实现共享的东西：HTTP 传输、哈希、URI 编码、重试。不对外暴露。

#include "settings/remote_store.h"

#include <curl/curl.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace user_data_sync::detail
{
// 一条 HTTP 请求。body 与 upload_file 二选一：给了 upload_file 就用它作为请求体并流式发送。
struct HttpRequest
{
    std::string method;
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    std::filesystem::path upload_file;
    std::filesystem::path download_file;
    // 单条请求的挂钟上限。上传大归档要放宽，探活要收紧。
    long timeout_seconds = 120;
    // 是否跟随重定向。WebDAV 的目录地址常 301 到带斜杠的写法，S3 会 301 到正确的 region。
    bool follow_redirects = true;
    // 需要读响应头的调用方（HEAD 取 size 与修改时间）把它设为非空。
    bool capture_headers = false;
};

struct HttpResponse
{
    CURLcode code = CURLE_FAILED_INIT;
    long status = 0;
    std::string body;
    std::string error;   // curl 的错误文本
    std::string headers; // 原始响应头，仅 capture_headers 为真时填充
};

// 发一条请求。全局初始化只做一次，代理按 utils/network_proxy 的配置注入。
HttpResponse Perform(const HttpRequest &request, const ProgressCallback &progress = {});

// 只接受 http / https 的绝对地址，其余一律拒绝（防止 file:// 之类的协议被当成远端）。
bool IsSupportedUrl(const std::string &url);
// 从 https://host:port/path 里取出 scheme、host（不含端口）、port 与 path。失败返回 false。
bool SplitUrl(const std::string &url, std::string &scheme, std::string &host, std::string &port, std::string &path);

// S3 的 URI 编码：除 A-Z a-z 0-9 - _ . ~ 之外全部 %XX，空格编成 %20。
std::string UriEncode(const std::string &value, bool encode_slash);
// 把 key 拼到 base_url 后面，逐段编码。base 末尾斜杠不重复。
std::string JoinUrl(const std::string &base_url, const std::string &key);
// 规范化查询串：按编码后的参数名排序。
std::string CanonicalQuery(const std::vector<std::pair<std::string, std::string>> &params);

std::string HexEncode(const unsigned char *data, std::size_t size);
bool Sha256(std::string_view data, unsigned char out[32]);
bool HmacSha256(std::string_view key, std::string_view data, unsigned char out[32]);
std::string HmacSha256Raw(std::string_view key, std::string_view data);

bool IsSuccess(long status);
bool IsNotFound(long status);
// 网络层瞬时错误（连不上、超时、连接被重置）值得重试；4xx 重试没有意义。
bool IsTransient(CURLcode code);
// 值得重试的状态码：5xx 与 429（S3 限流）。
bool IsRetryableStatus(long status);
// 把失败转成给人看的中文，附带服务端返回的前若干字节（S3 的 XML 错误里有 Code/Message）。
std::string DescribeHttpFailure(const HttpResponse &response);

// 从原始响应头里取一个字段（大小写不敏感），取最后一个出现的。
std::string HeaderValue(const std::string &headers, const std::string &name);

enum class AttemptResult
{
    Ok,
    Retry,
    Fatal,
};

using Attempt = std::function<AttemptResult(std::string &)>;

// 指数退避重试，最多三次。4xx 不重试：凭据错、桶名错重试一万次也还是错。
bool Retry(const Attempt &attempt, std::string &message);
} // namespace user_data_sync::detail
