#include "settings/remote_store.h"
#include "settings/remote_store_internal.h"
#include "tests/includes/test_framework.h"

#include <string>
#include <utility>
#include <vector>

// 传输层的离线验证：SigV4 签名、URI 编码、URL 拆分与响应头解析。
// 真实服务器的端到端验证不在这里，它需要网络，放在手工回归里。

// AWS 官方文档里的两个 SigV4 测试向量。签名逻辑错一位，真实 S3 会直接 403，
// 而本地没有 S3 可连，所以这两个向量是唯一能离线验证正确性的手段。
TEST_CASE(sigv4_matches_aws_documented_get_object_vector)
{
    user_data_sync::S3Options options;
    options.region = "us-east-1";
    options.access_key = "AKIAIOSFODNN7EXAMPLE";
    options.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    options.path_style = false;
    options.bucket = "examplebucket";

    const std::vector<std::pair<std::string, std::string>> headers = {
        {"host", "examplebucket.s3.amazonaws.com"},
        {"range", "bytes=0-9"},
        {"x-amz-content-sha256", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"x-amz-date", "20130524T000000Z"},
    };
    const user_data_sync::S3Signature signature =
        user_data_sync::S3Sign(options, "GET", "/test.txt", "", headers,
                               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 1369353600);
    REQUIRE_EQ(signature.signature, std::string("f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"));
    REQUIRE_EQ(signature.amz_date, std::string("20130524T000000Z"));
}

TEST_CASE(sigv4_matches_aws_documented_put_object_vector)
{
    user_data_sync::S3Options options;
    options.region = "us-east-1";
    options.access_key = "AKIAIOSFODNN7EXAMPLE";
    options.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    options.path_style = false;
    options.bucket = "examplebucket";

    const std::string payload = "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072";
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"date", "Fri, 24 May 2013 00:00:00 GMT"},
        {"host", "examplebucket.s3.amazonaws.com"},
        {"x-amz-content-sha256", payload},
        {"x-amz-date", "20130524T000000Z"},
    };
    const user_data_sync::S3Signature signature =
        user_data_sync::S3Sign(options, "PUT", "/test%24file.text", "", headers, payload, 1369353600);
    REQUIRE_EQ(signature.signature, std::string("1f0f03a2f9e50fde0c0a81574e56504b7c8a90148de788439fd654821bcab381"));
}

TEST_CASE(empty_payload_hash_is_sha256_of_nothing)
{
    REQUIRE_EQ(user_data_sync::EmptyPayloadSha256(),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

TEST_CASE(uri_encode_matches_sigv4_rules)
{
    // SigV4 要求空格编成 %20 而不是 +，斜杠在对象 key 里保留。
    REQUIRE_EQ(user_data_sync::detail::UriEncode("a b", true), std::string("a%20b"));
    REQUIRE_EQ(user_data_sync::detail::UriEncode("a/b", false), std::string("a/b"));
    REQUIRE_EQ(user_data_sync::detail::UriEncode("a/b", true), std::string("a%2Fb"));
    REQUIRE_EQ(user_data_sync::detail::UriEncode("A-z_0.9~", true), std::string("A-z_0.9~"));
    REQUIRE_EQ(user_data_sync::detail::UriEncode("水", true), std::string("%E6%B0%B4"));
}

TEST_CASE(canonical_query_sorts_by_encoded_name)
{
    const std::vector<std::pair<std::string, std::string>> params = {{"prefix", "a b"}, {"list-type", "2"}};
    REQUIRE_EQ(user_data_sync::detail::CanonicalQuery(params), std::string("list-type=2&prefix=a%20b"));
}

TEST_CASE(split_url_handles_ports_and_ipv6)
{
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    REQUIRE(user_data_sync::detail::SplitUrl("https://s3.example.com/bucket/key", scheme, host, port, path));
    REQUIRE_EQ(scheme, std::string("https"));
    REQUIRE_EQ(host, std::string("s3.example.com"));
    REQUIRE_EQ(port, std::string("443"));
    REQUIRE_EQ(path, std::string("/bucket/key"));

    REQUIRE(user_data_sync::detail::SplitUrl("http://127.0.0.1:9000/b", scheme, host, port, path));
    REQUIRE_EQ(host, std::string("127.0.0.1"));
    REQUIRE_EQ(port, std::string("9000"));

    // IPv6 字面量里的冒号不能被当成端口分隔。
    REQUIRE(user_data_sync::detail::SplitUrl("https://[::1]:8443/b", scheme, host, port, path));
    REQUIRE_EQ(host, std::string("[::1]"));
    REQUIRE_EQ(port, std::string("8443"));

    REQUIRE(user_data_sync::detail::SplitUrl("https://[::1]/b", scheme, host, port, path));
    REQUIRE_EQ(host, std::string("[::1]"));
    REQUIRE_EQ(port, std::string("443"));

    // 域名里的端口必须从 host 里剥掉，否则 S3 的 canonical host 会错、签名对不上。
    REQUIRE(user_data_sync::detail::SplitUrl("https://minio.local:9000", scheme, host, port, path));
    REQUIRE_EQ(host, std::string("minio.local"));
    REQUIRE_EQ(port, std::string("9000"));
    REQUIRE_EQ(path, std::string("/"));
}

TEST_CASE(split_url_rejects_non_http_schemes)
{
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    REQUIRE(!user_data_sync::detail::SplitUrl("file:///C:/x", scheme, host, port, path));
    REQUIRE(!user_data_sync::detail::IsSupportedUrl("file:///C:/x"));
    REQUIRE(user_data_sync::detail::IsSupportedUrl("https://a"));
}

TEST_CASE(join_url_does_not_double_slash)
{
    REQUIRE_EQ(user_data_sync::detail::JoinUrl("https://dav.example.com/msime/", "devices/a.json"),
               std::string("https://dav.example.com/msime/devices/a.json"));
    REQUIRE_EQ(user_data_sync::detail::JoinUrl("https://dav.example.com/msime", "devices/a.json"),
               std::string("https://dav.example.com/msime/devices/a.json"));
}

TEST_CASE(url_root_keeps_only_scheme_and_authority)
{
    REQUIRE_EQ(user_data_sync::detail::UrlRoot("https://dav.example.com/dav/msime"),
               std::string("https://dav.example.com"));
    REQUIRE_EQ(user_data_sync::detail::UrlRoot("http://127.0.0.1:9000/a/b"), std::string("http://127.0.0.1:9000"));
    REQUIRE_EQ(user_data_sync::detail::UrlRoot("https://dav.example.com"), std::string("https://dav.example.com"));
}

// 带认证的请求会先收到 401 挑战再收到 200。挑战块里的 Content-Length: 0 不是最终响应
// 的事实，取头必须只看最后一个响应块——真实坚果云上 HEAD 就踩过这个坑。
TEST_CASE(header_value_ignores_the_auth_challenge_block)
{
    const std::string headers = "HTTP/1.1 401 Unauthorized\r\n"
                                "Content-Length: 0\r\n"
                                "WWW-Authenticate: Basic realm=\"nutstore\"\r\n"
                                "\r\n"
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Length: 4321\r\n"
                                "Last-Modified: Mon, 09 Nov 2026 14:58:58 GMT\r\n"
                                "\r\n";
    REQUIRE_EQ(user_data_sync::detail::HeaderValue(headers, "content-length"), std::string("4321"));
    REQUIRE_EQ(user_data_sync::detail::HeaderValue(headers, "last-modified"),
               std::string("Mon, 09 Nov 2026 14:58:58 GMT"));
    // 只有挑战块时仍然能取到（用于 401 的错误描述）。
    const std::string only_challenge = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n";
    REQUIRE_EQ(user_data_sync::detail::HeaderValue(only_challenge, "content-length"), std::string("0"));
}
