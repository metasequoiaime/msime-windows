#pragma once

#include "utils/network_proxy.h"
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

// 不能按 VendorForRequest 判断：它只认 api.openai.com，会把 Azure 和 OpenRouter 中转降成 max_tokens。
inline bool UsesOpenAiOutputField(std::string_view provider, std::string_view endpoint)
{
    return IsOpenAiProvider(provider) && !NetworkProxy::IsLoopbackEndpoint(endpoint);
}

inline void SetTokenLimit(nlohmann::json &body, bool use_openai_field, int limit)
{
    body[use_openai_field ? "max_completion_tokens" : "max_tokens"] = limit;
}
} // namespace ChatCompletions
