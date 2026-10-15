#include "ai_assistant_request.h"
#include "chat_completions.h"

#include "voice-input/voice_providers.h"

namespace AiAssistant::detail
{
namespace
{
bool IsOpenAiReasoningModel(std::string_view model)
{
    const auto has_family = [model](std::string_view family) {
        return model == family || (model.size() > family.size() && model.substr(0, family.size()) == family &&
                                   (model[family.size()] == '-' || model[family.size()] == '.'));
    };
    if (has_family("gpt-5") || has_family("gpt-6"))
        return true;
    return model.size() >= 2 && model.front() == 'o' && model[1] >= '1' && model[1] <= '9' &&
           (model.size() == 2 || model[2] == '-');
}
} // namespace

nlohmann::json BuildRequestBody(const Request &request)
{
    const auto &config = request.config;
    const bool openai = VoiceInput::NormalizeProviderId(config.provider) == "openai";
    const bool openai_reasoning = openai && IsOpenAiReasoningModel(config.model);
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
    body[openai ? "max_completion_tokens" : "max_tokens"] = thinking || openai_reasoning ? 8192 : 512;
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
