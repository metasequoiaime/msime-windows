#pragma once

#include "utils/endpoint_host.h"
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

// openai 这个槽可以填任意 OpenAI 兼容地址，但只有真 OpenAI 认 max_completion_tokens。
// 指向本机 Ollama 时该字段会被 Go 的 encoding/json 静默丢弃，配置的输出上限随之失效。
// 不能改用 VendorForRequest 判断：它只认 api.openai.com，会把 Azure 与中转服务降成
// max_tokens，而 Azure 只接受 max_completion_tokens，直接 400。
inline bool UsesOpenAiOutputField(std::string_view provider, std::string_view endpoint)
{
    return IsOpenAiProvider(provider) && !NetworkProxy::IsLoopbackEndpoint(endpoint);
}

inline void SetTokenLimit(nlohmann::json &body, bool use_openai_field, int limit)
{
    body[use_openai_field ? "max_completion_tokens" : "max_tokens"] = limit;
}
} // namespace ChatCompletions
