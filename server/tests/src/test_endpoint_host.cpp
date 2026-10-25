// 端点判定：只有真正指向本机的地址才算免凭据与免特殊字段的对象。后缀伪装和私网地址必须判否，
// 否则一个手滑写下的域名就能把本机服务的信任范围放大到别人的服务器。
#include "src/utils/endpoint_host.h"
#include "tests/includes/test_framework.h"

TEST_CASE(endpoint_host_recognises_only_loopback_addresses)
{
    using NetworkProxy::IsLoopbackEndpoint;
    REQUIRE(IsLoopbackEndpoint("http://localhost:11434/v1/chat/completions"));
    REQUIRE(IsLoopbackEndpoint("http://127.0.0.1:11434/v1/chat/completions"));
    REQUIRE(IsLoopbackEndpoint("http://[::1]:11434/v1/chat/completions"));
    REQUIRE(IsLoopbackEndpoint("http://localhost/v1/models"));
    REQUIRE(IsLoopbackEndpoint("https://localhost"));
    REQUIRE(IsLoopbackEndpoint("http://localhost:11434"));
    REQUIRE(IsLoopbackEndpoint("http://localhost:11434?v=1"));
    REQUIRE(IsLoopbackEndpoint("http://localhost:11434#frag"));
    REQUIRE(IsLoopbackEndpoint("HTTP://LOCALHOST:11434/v1/chat/completions"));
    REQUIRE(IsLoopbackEndpoint("http://LoCaLhOsT:11434/v1/chat/completions"));
    REQUIRE(IsLoopbackEndpoint("http://user:pass@127.0.0.1:11434/v1/chat/completions"));
    REQUIRE(IsLoopbackEndpoint("http://user:p@ss@127.0.0.1:11434/v1/chat/completions"));
}

TEST_CASE(endpoint_host_rejects_everything_else)
{
    using NetworkProxy::IsLoopbackEndpoint;
    // 后缀伪装：host 必须以 localhost 整体结束，不能只是前缀相同。
    REQUIRE(!IsLoopbackEndpoint("http://localhost.evil.com/v1/chat/completions"));
    REQUIRE(!IsLoopbackEndpoint("http://127.0.0.1.evil.com/v1/chat/completions"));
    REQUIRE(!IsLoopbackEndpoint("http://notlocalhost:11434/v1/chat/completions"));
    REQUIRE(!IsLoopbackEndpoint("http://localhosts:11434/v1/chat/completions"));
    // 私网不在此列：局域网里那台谁都能访问的 Ollama 仍要凭据。
    REQUIRE(!IsLoopbackEndpoint("http://192.168.2.10:11434/v1/chat/completions"));
    REQUIRE(!IsLoopbackEndpoint("http://10.0.0.5:11434/v1/chat/completions"));
    REQUIRE(!IsLoopbackEndpoint("http://0.0.0.0:11434/v1/chat/completions"));
    // 没有 scheme 的地址不能被当成端点。
    REQUIRE(!IsLoopbackEndpoint("localhost:11434/v1/chat/completions"));
    REQUIRE(!IsLoopbackEndpoint("127.0.0.1:11434"));
    REQUIRE(!IsLoopbackEndpoint(""));
    REQUIRE(!IsLoopbackEndpoint("://"));
    // 方括号没闭合。
    REQUIRE(!IsLoopbackEndpoint("http://[::1/v1/chat/completions"));
}
