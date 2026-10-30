#pragma once

#include "voice-input/voice_providers.h"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace ChatCompletions
{
inline bool IsOpenAiProvider(std::string_view provider)
{
    return VoiceInput::NormalizeProviderId(provider) == "openai";
}

inline bool IsOpenAiReasoningModel(std::string_view model)
{
    std::string normalized(model);
    for (char &ch : normalized)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    model = normalized;
    const auto has_family = [model](std::string_view family) {
        return model == family || (model.size() > family.size() && model.substr(0, family.size()) == family &&
                                   (model[family.size()] == '-' || model[family.size()] == '.'));
    };
    if (has_family("gpt-5") || has_family("gpt-6"))
        return true;
    return model.size() >= 2 && model.front() == 'o' && model[1] >= '1' && model[1] <= '9' &&
           (model.size() == 2 || model[2] == '-');
}

// Reasoning models reject sampling parameters and spend output tokens on hidden reasoning.
inline bool IsOpenAiReasoningRequest(std::string_view provider, std::string_view model)
{
    return IsOpenAiProvider(provider) && IsOpenAiReasoningModel(model);
}

// The default effort is medium, which regularly outlasts an IME request. "low" is the lowest
// level every o-series and GPT-5 family model accepts.
inline constexpr const char *kOpenAiReasoningEffort = "low";
inline constexpr long kReasoningTimeoutMs = 30000;

inline void SetTokenLimit(nlohmann::json &body, std::string_view provider, int limit)
{
    body[IsOpenAiProvider(provider) ? "max_completion_tokens" : "max_tokens"] = limit;
}

// response_format 是 OpenAI 的扩展字段，不在兼容层的公共子集里。原生实现（OpenAI、DeepSeek
// 等）收到它会约束输出，本地推理服务的表现却分成三种：LM Studio 只认 json_schema，遇到
// json_object 直接拒绝；vLLM 走 xgrammar 引导解码，schema 特性不支持时返回 400；Ollama 的
// /v1 把它收下但不映射到原生 format，静默不生效。所以这个字段发不发由调用方按用户的策略决定，
// 不能被当成通用默认值。
enum class JsonMode
{
    Auto,  // 按接口地址判断：本机与未知服务不带，已知原生实现带上
    Force, // 一定带 {"type":"json_object"}
    Off    // 一定不带，靠 prompt 约束与解析兜底
};

inline bool IsValidJsonMode(std::string_view mode)
{
    return mode == "auto" || mode == "force" || mode == "off";
}

inline std::string NormalizeJsonMode(std::string_view mode)
{
    return IsValidJsonMode(mode) ? std::string(mode) : std::string("auto");
}

// 本机回环与私有网段。用户在本机跑 vLLM、LM Studio、Ollama
// 时地址常常是 127.0.0.1 或局域网另一台机器，这些地址上几乎没人实现 response_format。
inline bool IsLocalChatEndpoint(std::string_view endpoint)
{
    std::string host;
    const size_t scheme = endpoint.find("://");
    if (scheme == std::string_view::npos)
        return false;
    std::string_view rest = endpoint.substr(scheme + 3);
    const size_t path = rest.find('/');
    if (path != std::string_view::npos)
        rest = rest.substr(0, path);
    const size_t at = rest.rfind('@');
    if (at != std::string_view::npos)
        rest = rest.substr(at + 1);
    if (!rest.empty() && rest.front() == '[') // [::1]:11434
    {
        const size_t close = rest.find(']');
        host.assign(rest.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
    }
    else
    {
        const size_t colon = rest.find(':');
        host.assign(rest.substr(0, colon));
    }
    std::string lowered(host);
    for (char &ch : lowered)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (lowered == "localhost" || lowered == "::1" || lowered == "0.0.0.0")
        return true;
    // 只认完整的点分四段地址：数值前缀匹配会把 172.16.example.com 这类公网域名也当成本机。
    int octets[4] = {0, 0, 0, 0};
    size_t position = 0;
    for (int i = 0; i < 4; ++i)
    {
        const size_t start = position;
        int value = 0;
        while (position < lowered.size() && lowered[position] >= '0' && lowered[position] <= '9')
        {
            value = value * 10 + (lowered[position] - '0');
            if (value > 255)
                return false;
            ++position;
        }
        if (position == start) // 空段或非数字字符
            return false;
        octets[i] = value;
        if (i < 3)
        {
            if (position >= lowered.size() || lowered[position] != '.')
                return false;
            ++position;
        }
    }
    if (position != lowered.size()) // 多于四段
        return false;
    if (octets[0] == 127 || octets[0] == 10)
        return true;
    if (octets[0] == 192 && octets[1] == 168)
        return true;
    return octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31;
}

// 这个地址上要不要带 response_format。只有明确知道对方是原生实现才在 Auto 下带上：
// 本机一律不带，未知服务也不带（拒收的代价比少一层约束大得多）。
inline bool UsesJsonResponseFormat(std::string_view mode, std::string_view endpoint)
{
    const std::string normalized = NormalizeJsonMode(mode);
    if (normalized == "force")
        return true;
    if (normalized == "off" || IsLocalChatEndpoint(endpoint))
        return false;
    const VoiceInput::ChatVendor vendor = VoiceInput::VendorFromChatEndpoint(endpoint);
    return vendor == VoiceInput::ChatVendor::OpenAI || vendor == VoiceInput::ChatVendor::DeepSeek;
}
} // namespace ChatCompletions
