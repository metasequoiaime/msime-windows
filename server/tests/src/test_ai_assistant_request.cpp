#include "ai/ai_assistant_request.h"
#include "settings/api_credential_test_request.h"
#include "tests/includes/test_framework.h"

namespace
{
AiAssistant::Request MakeRequest(const std::string &provider, const std::string &thinking)
{
    AiAssistant::Request request;
    request.pinyin_segments = {"shang", "hai"};
    request.context = "我准备去";
    request.config.provider = provider;
    request.config.model = "example-model";
    request.config.prompt = "Return the best candidate.";
    request.config.thinking = thinking;
    return request;
}
} // namespace

TEST_CASE(ai_request_disables_deepseek_thinking_by_default)
{
    const auto body = AiAssistant::detail::BuildRequestBody(MakeRequest("deepseek", "disabled"));
    REQUIRE_EQ(body["thinking"]["type"].get<std::string>(), "disabled");
    REQUIRE(!body.contains("reasoning_effort"));
    REQUIRE_EQ(body["max_tokens"].get<int>(), 512);
    REQUIRE(!body.contains("max_completion_tokens"));
    REQUIRE_EQ(body["temperature"].get<double>(), 0.2);
    REQUIRE_EQ(body["response_format"]["type"].get<std::string>(), "json_object");
    REQUIRE_EQ(AiAssistant::detail::RequestTimeoutMs(MakeRequest("deepseek", "disabled").config), 8000L);
}

TEST_CASE(ai_request_sends_deepseek_thinking_level)
{
    for (const char *level : {"low", "high", "max"})
    {
        const auto request = MakeRequest("deepseek", level);
        const auto body = AiAssistant::detail::BuildRequestBody(request);
        REQUIRE_EQ(body["thinking"]["type"].get<std::string>(), "enabled");
        REQUIRE_EQ(body["reasoning_effort"].get<std::string>(), std::string(level));
        REQUIRE(body["max_tokens"].get<int>() > 512);
        REQUIRE(!body.contains("max_completion_tokens"));
        REQUIRE_EQ(body["temperature"].get<double>(), 0.2);
        REQUIRE(AiAssistant::detail::RequestTimeoutMs(request.config) > 8000L);
    }
}

TEST_CASE(ai_request_ignores_thinking_level_for_other_providers)
{
    const auto request = MakeRequest("custom", "high");
    const auto body = AiAssistant::detail::BuildRequestBody(request);
    REQUIRE(!body.contains("thinking"));
    REQUIRE(!body.contains("reasoning_effort"));
    REQUIRE_EQ(body["max_tokens"].get<int>(), 512);
    REQUIRE_EQ(AiAssistant::detail::RequestTimeoutMs(request.config), 8000L);
}

TEST_CASE(ai_request_uses_openai_completion_limit_for_older_models)
{
    auto request = MakeRequest("openai", "disabled");
    request.config.model = "gpt-4o-mini";
    const auto body = AiAssistant::detail::BuildRequestBody(request);
    REQUIRE_EQ(body["max_completion_tokens"].get<int>(), 512);
    REQUIRE(!body.contains("max_tokens"));
    REQUIRE_EQ(body["temperature"].get<double>(), 0.2);
    REQUIRE_EQ(body["response_format"]["type"].get<std::string>(), "json_object");
    REQUIRE(!body.contains("reasoning_effort"));
    REQUIRE(!body.contains("thinking"));
    REQUIRE_EQ(AiAssistant::detail::RequestTimeoutMs(request.config), 8000L);
}

TEST_CASE(ai_request_omits_unsupported_sampling_for_openai_reasoning_models)
{
    for (const char *model : {"gpt-5.6-luna", "GPT-5.6-Luna", "gpt-5", "o3-mini", "O3-mini"})
    {
        auto request = MakeRequest("OpenAI", "disabled");
        request.config.model = model;
        const auto body = AiAssistant::detail::BuildRequestBody(request);
        REQUIRE_EQ(body["model"].get<std::string>(), std::string(model));
        REQUIRE_EQ(body["max_completion_tokens"].get<int>(), 8192);
        REQUIRE(!body.contains("max_tokens"));
        REQUIRE(!body.contains("temperature"));
        REQUIRE_EQ(body["reasoning_effort"].get<std::string>(), "low");
        REQUIRE_EQ(body["response_format"]["type"].get<std::string>(), "json_object");
        REQUIRE(!body.contains("thinking"));
        REQUIRE(AiAssistant::detail::RequestTimeoutMs(request.config) > 8000L);
    }
}

TEST_CASE(ai_request_keeps_custom_provider_request_shape)
{
    auto request = MakeRequest("custom", "high");
    request.config.model = "gpt-5.6-luna";
    const auto body = AiAssistant::detail::BuildRequestBody(request);
    REQUIRE_EQ(body["max_tokens"].get<int>(), 512);
    REQUIRE(!body.contains("max_completion_tokens"));
    REQUIRE_EQ(body["temperature"].get<double>(), 0.2);
    REQUIRE_EQ(body["response_format"]["type"].get<std::string>(), "json_object");
    REQUIRE(!body.contains("reasoning_effort"));
    REQUIRE(!body.contains("thinking"));
}

TEST_CASE(credential_test_uses_openai_completion_limit)
{
    const auto older_model = ApiCredentialTest::detail::BuildChatProbeRequestBody("openai", "gpt-4o-mini");
    REQUIRE_EQ(older_model["max_completion_tokens"].get<int>(), 1);
    REQUIRE(!older_model.contains("max_tokens"));
    REQUIRE(!older_model.contains("reasoning_effort"));

    for (const char *model : {"gpt-5.6-luna", "GPT-5.6-Luna", "gpt-6-luna", "o3-mini"})
    {
        const auto body = ApiCredentialTest::detail::BuildChatProbeRequestBody("OpenAI", model);
        REQUIRE_EQ(body["model"].get<std::string>(), std::string(model));
        REQUIRE_EQ(body["max_completion_tokens"].get<int>(), 32);
        REQUIRE(!body.contains("max_tokens"));
        REQUIRE_EQ(body["reasoning_effort"].get<std::string>(), "low");
        REQUIRE(!body.contains("thinking"));
        REQUIRE(!body.contains("temperature"));
        REQUIRE_EQ(body["messages"][0]["content"].get<std::string>(), "Reply OK");
    }
}

TEST_CASE(credential_test_keeps_other_provider_request_shape)
{
    const auto deepseek = ApiCredentialTest::detail::BuildChatProbeRequestBody("deepseek", "deepseek-chat");
    REQUIRE_EQ(deepseek["max_tokens"].get<int>(), 1);
    REQUIRE(!deepseek.contains("max_completion_tokens"));
    REQUIRE_EQ(deepseek["thinking"]["type"].get<std::string>(), "disabled");
    REQUIRE(!deepseek.contains("enable_thinking"));

    const auto siliconflow = ApiCredentialTest::detail::BuildChatProbeRequestBody("siliconflow", "Qwen/Qwen3-8B");
    REQUIRE_EQ(siliconflow["max_tokens"].get<int>(), 1);
    REQUIRE_EQ(siliconflow["enable_thinking"].get<bool>(), false);
    REQUIRE(!siliconflow.contains("max_completion_tokens"));

    const auto custom = ApiCredentialTest::detail::BuildChatProbeRequestBody("custom", "gpt-5.6-luna");
    REQUIRE_EQ(custom["max_tokens"].get<int>(), 1);
    REQUIRE(!custom.contains("max_completion_tokens"));
    REQUIRE(!custom.contains("thinking"));
    REQUIRE(!custom.contains("enable_thinking"));
}
