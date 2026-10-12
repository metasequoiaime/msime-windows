#include "ai_assistant_request.h"
#include "chat_completions.h"

#include "voice-input/voice_providers.h"

namespace AiAssistant::detail
{
nlohmann::json BuildRequestBody(const Request &request)
{
    const auto &config = request.config;
    const bool thinking = VoiceInput::UsesDeepSeekThinking(config.provider, config.thinking);
    nlohmann::json input = {{"segmented_pinyin", request.pinyin_segments},
                            {"context", request.context},
                            {"candidate_limit", config.candidate_limit}};
    nlohmann::json body = {
        {"model", config.model},
        {"stream", false},
        {"temperature", 0.2},
        // Reasoning tokens may count against max_tokens; 512 would cut the JSON answer off.
        {"max_tokens", thinking ? 8192 : 512},
        {"response_format", {{"type", "json_object"}}},
        {"messages",
         {{{"role", "system"}, {"content", config.prompt}}, {{"role", "user"}, {"content", input.dump()}}}}};
    if (thinking)
    {
        body["thinking"] = {{"type", "enabled"}};
        body["reasoning_effort"] = config.thinking;
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
    return VoiceInput::UsesDeepSeekThinking(config.provider, config.thinking) ? 30000L
                                                                              : ChatCompletions::kDefaultTimeoutMs;
}
} // namespace AiAssistant::detail
