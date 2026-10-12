#pragma once

#include <optional>
#include <string>

// AI 联想与 AI 候选词翻译共用的 OpenAI 兼容 Chat Completions 传输层。
namespace ChatCompletions
{
constexpr long kConnectTimeoutMs = 2500;
// 非思考模式的整体超时。推理类模型（默认开思考的 Qwen3 等）几秒内答不完，5 秒会让请求整批落空。
constexpr long kDefaultTimeoutMs = 8000;

// POST 一次请求并返回 choices[0].message.content。网络错误、非 2xx、响应超过 1 MiB
// 或响应结构不对都返回 nullopt。
std::optional<std::string> FetchContent(const std::string &endpoint, const std::string &token,
                                        const std::string &payload, long timeout_ms);
} // namespace ChatCompletions
