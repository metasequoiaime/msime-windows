#include "ai_assistant_request.h"
#include "chat_completions.h"
#include "chat_request_params.h"

#include "voice-input/voice_providers.h"

namespace AiAssistant::detail
{
nlohmann::json BuildRequestBody(const Request &request)
{
    const auto &config = request.config;
    const bool openai_reasoning = ChatCompletions::IsOpenAiReasoningRequest(config.provider, config.model);
    const bool thinking = VoiceInput::UsesDeepSeekThinking(config.provider, config.thinking);
    nlohmann::json input = {{"segmented_pinyin", request.pinyin_segments},
                            {"context", request.context},
                            {"candidate_limit", config.candidate_limit}};
    nlohmann::json body = {
        {"model", config.model},
        {"stream", false},
        {"response_format", {{"type", "json_object"}}},
        {"messages",
         {{{"role", "system"}, {"content", config.prompt}}, {{"role", "user"}, {"content", input.dump()}}}}};
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
    else if (config.provider == "deepseek")
    {
        // DeepSeek thinking can add hundreds of reasoning tokens and noticeably delay
        // an IME suggestion. Keep custom OpenAI-compatible providers untouched.
        body["thinking"] = {{"type", "disabled"}};
    }
    return body;
}

long RequestTimeoutMs(const AiAssistantConfig &config)
{
    return VoiceInput::UsesDeepSeekThinking(config.provider, config.thinking) ||
                   ChatCompletions::IsOpenAiReasoningRequest(config.provider, config.model)
               ? ChatCompletions::kReasoningTimeoutMs
               : ChatCompletions::kDefaultTimeoutMs;
}
} // namespace AiAssistant::detail
