#pragma once

#include "ai/chat_request_params.h"

#include <nlohmann/json.hpp>
#include <string>

namespace ApiCredentialTest::detail
{
inline nlohmann::json BuildChatProbeRequestBody(const std::string &provider, const std::string &model,
                                                const std::string &endpoint = {})
{
    nlohmann::json body = {
        {"model", model}, {"stream", false}, {"messages", {{{"role", "user"}, {"content", "Reply OK"}}}}};
    // A one-token probe can reject reasoning models before they return a response.
    const bool openai_reasoning = ChatCompletions::IsOpenAiReasoningRequest(provider, model);
    ChatCompletions::SetTokenLimit(body, ChatCompletions::UsesOpenAiOutputField(provider, endpoint),
                                   openai_reasoning ? 32 : 1);
    // Vendor fields follow the endpoint, not the label: the DeepSeek slot can hold any
    // OpenAI-compatible service, and thinking.type makes services that do not accept it
    // (MiniMax among them) reject the request instead of answering it.
    const auto vendor = VoiceInput::VendorForRequest(provider, endpoint);
    // Probe with the same effort the real requests send, so a model that rejects it fails here.
    if (openai_reasoning)
        body["reasoning_effort"] = ChatCompletions::kOpenAiReasoningEffort;
    else if (vendor == VoiceInput::ChatVendor::DeepSeek)
        body["thinking"] = {{"type", "disabled"}};
    else if (vendor == VoiceInput::ChatVendor::SiliconFlow)
        body["enable_thinking"] = false;
    return body;
}
} // namespace ApiCredentialTest::detail
