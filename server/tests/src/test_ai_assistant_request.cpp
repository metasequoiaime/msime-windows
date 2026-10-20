#include "ai/ai_assistant_request.h"
#include "settings/api_credential_test_request.h"
#include "tests/includes/test_framework.h"

namespace
{
// 厂商字段现在按接口地址判断，所以构造请求时必须显式给地址；默认给一个 DeepSeek 地址，
// 让"标签=deepseek"的历史用例保持原意。
AiAssistant::Request MakeRequest(const std::string &provider, const std::string &thinking,
                                 const std::string &endpoint = "https://api.deepseek.com/chat/completions")
{
    AiAssistant::Request request;
    request.pinyin_segments = {"shang", "hai"};
    request.context = "我准备去";
    request.config.provider = provider;
    request.config.model = "example-model";
    request.config.endpoint = endpoint;
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
    // A plain OpenAI-compatible endpoint stays a plain request, thinking level or not.
    const auto request = MakeRequest("custom", "high", "https://relay.example.com/v1/chat/completions");
    const auto body = AiAssistant::detail::BuildRequestBody(request);
    REQUIRE(!body.contains("thinking"));
    REQUIRE(!body.contains("reasoning_effort"));
    REQUIRE_EQ(body["max_tokens"].get<int>(), 512);
    REQUIRE_EQ(AiAssistant::detail::RequestTimeoutMs(request.config), 8000L);
}

TEST_CASE(ai_request_follows_the_endpoint_not_the_provider_label)
{
    // 标签写着 deepseek、地址却是别的服务：带着 thinking.type 会被对方直接拒绝
    // （MiniMax 返回 2013 requires adaptive thinking），所以必须按地址判断。
    const auto foreign = MakeRequest("deepseek", "high", "https://api.minimax.cn/v1/chat/completions");
    const auto foreign_body = AiAssistant::detail::BuildRequestBody(foreign);
    REQUIRE(!foreign_body.contains("thinking"));
    REQUIRE(!foreign_body.contains("reasoning_effort"));
    REQUIRE_EQ(AiAssistant::detail::RequestTimeoutMs(foreign.config), 8000L);

    // 反过来：custom 槽指向真正的 DeepSeek 时，thinking 依然可用。
    const auto real = MakeRequest("custom", "high", "https://api.deepseek.com/chat/completions");
    const auto real_body = AiAssistant::detail::BuildRequestBody(real);
    REQUIRE_EQ(real_body["thinking"]["type"].get<std::string>(), "enabled");
    REQUIRE_EQ(real_body["reasoning_effort"].get<std::string>(), "high");
    REQUIRE(AiAssistant::detail::RequestTimeoutMs(real.config) > 8000L);
}

TEST_CASE(ai_request_uses_openai_completion_limit_for_older_models)
{
    auto request = MakeRequest("openai", "disabled", "https://api.openai.com/v1/chat/completions");
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

TEST_CASE(ai_request_keys_the_output_limit_off_the_endpoint)
{
    auto ollama = MakeRequest("openai", "disabled", "http://localhost:11434/v1/chat/completions");
    const auto ollama_body = AiAssistant::detail::BuildRequestBody(ollama);
    REQUIRE_EQ(ollama_body["max_tokens"].get<int>(), 512);
    REQUIRE(!ollama_body.contains("max_completion_tokens"));

    auto real = MakeRequest("custom", "disabled", "https://api.openai.com/v1/chat/completions");
    const auto real_body = AiAssistant::detail::BuildRequestBody(real);
    REQUIRE_EQ(real_body["max_tokens"].get<int>(), 512);
    REQUIRE(!real_body.contains("max_completion_tokens"));

    auto azure =
        MakeRequest("openai", "disabled", "https://example.openai.azure.com/openai/deployments/d/chat/completions");
    const auto azure_body = AiAssistant::detail::BuildRequestBody(azure);
    REQUIRE_EQ(azure_body["max_completion_tokens"].get<int>(), 512);
    REQUIRE(!azure_body.contains("max_tokens"));
}

TEST_CASE(ai_request_omits_unsupported_sampling_for_openai_reasoning_models)
{
    for (const char *model : {"gpt-5.6-luna", "GPT-5.6-Luna", "gpt-5", "o3-mini", "O3-mini"})
    {
        auto request = MakeRequest("OpenAI", "disabled", "https://api.openai.com/v1/chat/completions");
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
    auto request = MakeRequest("custom", "high", "https://relay.example.com/v1/chat/completions");
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

TEST_CASE(credential_test_keys_the_output_limit_off_the_endpoint)
{
    const auto ollama = ApiCredentialTest::detail::BuildChatProbeRequestBody(
        "openai", "gpt-4o-mini", "http://localhost:11434/v1/chat/completions");
    REQUIRE_EQ(ollama["max_tokens"].get<int>(), 1);
    REQUIRE(!ollama.contains("max_completion_tokens"));

    const auto real = ApiCredentialTest::detail::BuildChatProbeRequestBody(
        "custom", "gpt-4o-mini", "https://api.openai.com/v1/chat/completions");
    REQUIRE_EQ(real["max_tokens"].get<int>(), 1);
    REQUIRE(!real.contains("max_completion_tokens"));
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
