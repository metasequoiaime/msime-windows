#pragma once

// 用户数据远程备份的传输层。只认"对象"这一件事：按 key 读写、列举、删除，不解释 key 的
// 语义，也不碰归档格式。WebDAV 与 S3 兼容（MinIO / R2 / B2 / OSS）各实现一份。
//
// 为什么自己写 SigV4 而不引入 aws-sdk-cpp：canonical request 只有两百行，而 SDK 会带来
// 一整套凭证链、CMake 集成和平台依赖；仓库的 CONTRIBUTING 明确要求功能性 PR 不轻易加
// 依赖。哈希与 HMAC 用 BCrypt，和 cloud/tencent_tmt.cpp 的做法一致。
//
// 所有实现都走 utils/network_proxy.cpp 的代理配置，和云输入、模型下载共用同一套开关。
// TLS 校验始终开启，不提供"跳过校验"开关：自签证书的服务端应当把证书导入系统信任库。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace user_data_sync
{
struct RemoteObject
{
    std::string key;
    std::uint64_t size = 0;
    std::string etag;
    // 服务端给出的修改时间，Unix 秒。拿不到时为 0。
    std::int64_t modified_unix = 0;
};

struct TransferProgress
{
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
};

using ProgressCallback = std::function<void(const TransferProgress &)>;

class RemoteStore
{
  public:
    virtual ~RemoteStore() = default;

    // 上传一个本地文件。失败时 message 是给人看的中文说明。
    virtual bool Put(const std::string &key, const std::filesystem::path &file, const ProgressCallback &progress,
                     std::string &message) = 0;
    // 下载到本地文件。目标文件由调用方负责清理；本函数失败时不保证目标文件可用。
    virtual bool Get(const std::string &key, const std::filesystem::path &file, const ProgressCallback &progress,
                     std::string &message) = 0;
    // 不存在时返回 false，并把 message 留空：调用方用它区分"没有"和"出错"。
    virtual bool Stat(const std::string &key, RemoteObject &object, std::string &message) = 0;
    virtual bool List(const std::string &prefix, std::vector<RemoteObject> &objects, std::string &message) = 0;
    virtual bool Delete(const std::string &key, std::string &message) = 0;
    // 确保 key 所在的目录存在。WebDAV 必须显式建集合，否则对不存在集合的 PUT 返回 409；
    // S3 没有目录概念，实现为空。
    virtual bool EnsureDirectory(const std::string &key, std::string &message) = 0;
    // 验证地址与凭据能不能用。设置页的「测试连接」调它。
    virtual bool Probe(std::string &message) = 0;
};

struct WebDavOptions
{
    // 备份根目录，例如 https://dav.example.com/remote.php/dav/files/me/msime。
    // 末尾斜杠会被去掉；key 直接拼在后面。
    std::string base_url;
    std::string username;
    std::string password;
};

struct S3Options
{
    // 服务地址，例如 https://s3.us-east-1.amazonaws.com 或 http://127.0.0.1:9000。
    std::string endpoint;
    std::string region = "us-east-1";
    std::string bucket;
    std::string access_key;
    std::string secret_key;
    // MinIO、R2 与多数自建网关用路径风格（/bucket/key）；AWS 默认虚拟主机风格。
    bool path_style = true;
    // 备份根前缀，可为空。
    std::string prefix;
};

std::unique_ptr<RemoteStore> MakeWebDavStore(WebDavOptions options);
std::unique_ptr<RemoteStore> MakeS3Store(S3Options options);

// SigV4 的签名结果。拆成结构体是为了让测试能逐段核对官方测试向量，而不是只看
// Authorization 头拼得对不对。
struct S3Signature
{
    std::string authorization;
    std::string amz_date; // 20150830T123600Z
    std::string canonical_request;
    std::string string_to_sign;
    std::string signature;    // 十六进制
    std::string payload_hash; // 十六进制
};

// 计算一次请求的签名。signed_headers 里的名字必须已小写，值原样参与规范化；
// host 由调用方给，因为虚拟主机风格与路径风格的 host 不同。unix_time 由调用方给，
// 免得签名逻辑自己读时钟、测试无法复现。
S3Signature S3Sign(const S3Options &options, const std::string &method, const std::string &canonical_uri,
                   const std::string &canonical_query,
                   const std::vector<std::pair<std::string, std::string>> &signed_headers,
                   const std::string &payload_sha256_hex, std::int64_t unix_time);

// 文件的 SHA-256，小写十六进制。读失败返回空串。
std::string Sha256FileHex(const std::filesystem::path &file);
// 空串的 SHA-256，S3 对没有 body 的请求要用它。
std::string EmptyPayloadSha256();
} // namespace user_data_sync
