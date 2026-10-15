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
    const int token_limit =
        ChatCompletions::IsOpenAiProvider(provider) && ChatCompletions::IsOpenAiReasoningModel(model) ? 32 : 1;
    ChatCompletions::SetTokenLimit(body, provider, token_limit);
    if (provider == "deepseek")
        body["thinking"] = {{"type", "disabled"}};
    else if (provider == "siliconflow")
        body["enable_thinking"] = false;
    return body;
}
} // namespace ApiCredentialTest::detail
