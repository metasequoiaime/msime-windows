#include "ai_assistant_request.h"
#include "chat_completions.h"
#include "chat_request_params.h"

#include "voice-input/voice_providers.h"

namespace AiAssistant::detail
{
nlohmann::json BuildRequestBody(const Request &request)
{
    const auto &config = request.config;
    // Which vendor answers is a property of the endpoint, not of the label in the settings UI: a
    // slot can hold any OpenAI-compatible endpoint, and DeepSeek's thinking.type makes MiniMax
    // reject the request outright instead of silently returning no suggestion.
    const auto vendor = VoiceInput::VendorForRequest(config.provider, config.endpoint);
    const bool openai_reasoning = ChatCompletions::IsOpenAiReasoningRequest(config.provider, config.model);
    const bool thinking = vendor == VoiceInput::ChatVendor::DeepSeek &&
                          VoiceInput::IsValidThinkingLevel(config.thinking) && config.thinking != "disabled";
    nlohmann::json input = {{"segmented_pinyin", request.pinyin_segments},
                            {"context", request.context},
                            {"candidate_limit", config.candidate_limit}};
    nlohmann::json body = {
        {"model", config.model},
        {"stream", false},
        {"messages",
         {{{"role", "system"}, {"content", config.prompt}}, {{"role", "user"}, {"content", input.dump()}}}}};
    // 本地部署与未知服务默认不带，见 ChatCompletions::JsonMode。
    if (ChatCompletions::UsesJsonResponseFormat(config.json_mode, config.endpoint))
        body["response_format"] = {{"type", "json_object"}};
    if (!openai_reasoning)
        body["temperature"] = 0.2;
    // Reasoning tokens count against the output limit and can leave no room for JSON.
    ChatCompletions::SetTokenLimit(body, config.provider, thinking || openai_reasoning ? 8192 : 512);
    if (thinking)
    {
        body["thinking"] = {{"type", "enabled"}};
        body["reasoning_effort"] = config.thinking;
    }
    else if (openai_reasoning)
    {
        body["reasoning_effort"] = ChatCompletions::kOpenAiReasoningEffort;
    }
    else if (vendor == VoiceInput::ChatVendor::DeepSeek)
    {
        // DeepSeek thinking can add hundreds of reasoning tokens and noticeably delay
        // an IME suggestion. Every other service gets a plain OpenAI-compatible request.
        body["thinking"] = {{"type", "disabled"}};
    }
    return body;
}

long RequestTimeoutMs(const AiAssistantConfig &config)
{
    return VoiceInput::UsesDeepSeekThinking(config.provider, config.endpoint, config.thinking) ||
                   ChatCompletions::IsOpenAiReasoningRequest(config.provider, config.model)
               ? ChatCompletions::kReasoningTimeoutMs
               : ChatCompletions::kDefaultTimeoutMs;
}
} // namespace AiAssistant::detail
