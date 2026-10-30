#pragma once

#include <optional>
#include <string>

// AI 联想与 AI 候选词翻译共用的 OpenAI 兼容 Chat Completions 传输层。
namespace ChatCompletions
{
constexpr long kConnectTimeoutMs = 2500;
// 非思考模式的整体超时。推理类模型（默认开思考的 Qwen3 等）几秒内答不完，5 秒会让请求整批落空。
constexpr long kDefaultTimeoutMs = 8000;

// 一次请求失败的原因。原来所有失败都折成 nullopt，用户只能看到「服务未返回有效译文」，
// 分不清是连不上本机服务、地址写错、被服务端拒绝还是答得太慢。
enum class Failure
{
    None,
    Request,      // 本地就失败了：curl 初始化、请求体构造
    Network,      // 连接被拒、DNS、代理、TLS 等，响应体为空
    Status,       // 服务端返回了非 2xx
    Timeout,      // 超过整体超时或连接超时
    TooLarge,     // 响应超过 1 MiB，已中止读取
    InvalidJson,  // 响应不是合法 JSON
    NoContent,    // JSON 合法但没有 choices[0].message.content
    EmptyContent, // 有 content 但内容为空或全是思考
};

struct Result
{
    std::optional<std::string> content;
    Failure failure = Failure::None;
    // curl 的详细错误、HTTP 状态码或服务端返回的 error.message，已脱敏。可直接展示给用户。
    std::string detail;
};

// POST 一次请求并从 choices[0].message.content 取回复。失败时 Result::failure 说明原因，
// detail 带上足够定位的信息。token 为空时不带 Authorization 头（Ollama 这类不鉴权的本机服务）。
Result FetchContent(const std::string &endpoint, const std::string &token, const std::string &payload, long timeout_ms);

// 把失败原因格式化成一句中文。detail 非空时附在后面。
std::string DescribeFailure(Failure failure, const std::string &detail);
} // namespace ChatCompletions
