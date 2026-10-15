#pragma once

#include "ai/chat_request_params.h"

#include <nlohmann/json.hpp>
#include <string>

namespace ApiCredentialTest::detail
{
inline nlohmann::json BuildChatProbeRequestBody(const std::string &provider, const std::string &model)
{
    nlohmann::json body = {
        {"model", model}, {"stream", false}, {"messages", {{{"role", "user"}, {"content", "Reply OK"}}}}};
    // A one-token probe can reject reasoning models before they return a response.
    const bool openai_reasoning = ChatCompletions::IsOpenAiReasoningRequest(provider, model);
    ChatCompletions::SetTokenLimit(body, provider, openai_reasoning ? 32 : 1);
    // Probe with the same effort the real requests send, so a model that rejects it fails here.
    if (openai_reasoning)
        body["reasoning_effort"] = ChatCompletions::kOpenAiReasoningEffort;
    else if (provider == "deepseek")
        body["thinking"] = {{"type", "disabled"}};
    else if (provider == "siliconflow")
        body["enable_thinking"] = false;
    return body;
}
} // namespace ApiCredentialTest::detail
