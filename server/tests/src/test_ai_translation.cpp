#include "tests/includes/test_framework.h"

#include "ai/chat_request_params.h"
#include "cloud/ai_translation.h"

TEST_CASE(AiTranslationValidatesConfig)
{
    AiTranslation::Config valid;
    valid.provider = "deepseek";
    valid.endpoint = "https://api.deepseek.com/chat/completions";
    valid.token = "sk-test";
    valid.model = "deepseek-chat";
    REQUIRE(AiTranslation::IsUsableConfig(valid));

    AiTranslation::Config missing_token = valid;
    missing_token.token = "";
    REQUIRE(!AiTranslation::IsUsableConfig(missing_token));

    AiTranslation::Config missing_endpoint = valid;
    missing_endpoint.endpoint = "";
    REQUIRE(!AiTranslation::IsUsableConfig(missing_endpoint));

    AiTranslation::Config missing_model = valid;
    missing_model.model = "";
    REQUIRE(!AiTranslation::IsUsableConfig(missing_model));

    // 出厂模板里的占位 Token 不能被当成真凭据，否则候选词会带着假 Token 发出去。
    for (const char *placeholder : {"FAKESECRET_e1f2g3h4i5j6k7l8m9n0", "<YOUR_AI_TOKEN_DEEPSEEK>"})
    {
        AiTranslation::Config fake = valid;
        fake.token = placeholder;
        REQUIRE(!AiTranslation::IsUsableConfig(fake));
    }
}

TEST_CASE(AiTranslationBuildsConfigFromTranslationSnapshot)
{
    TranslationConfig snapshot;
    snapshot.ai = {"deepseek", "  https://api.deepseek.com/chat/completions ", " sk-test ", " deepseek-chat "};
    const auto config = AiTranslation::FromConfiguredTranslation(snapshot);
    REQUIRE_EQ(config.provider, std::string("deepseek"));
    REQUIRE_EQ(config.endpoint, std::string("https://api.deepseek.com/chat/completions"));
    REQUIRE_EQ(config.token, std::string("sk-test"));
    REQUIRE_EQ(config.model, std::string("deepseek-chat"));
    REQUIRE(AiTranslation::IsUsableConfig(config));
}

TEST_CASE(AiTranslationOmitsSamplingForOpenAiReasoningModels)
{
    AiTranslation::Config config{"OpenAI", "https://example.test", "sk-test", "GPT-5.6-Luna", "force"};
    const auto body = AiTranslation::detail::BuildRequestBody(config, {"你好", "世界"}, "zh", "en");
    REQUIRE_EQ(body["model"].get<std::string>(), config.model);
    REQUIRE_EQ(body["stream"].get<bool>(), false);
    REQUIRE_EQ(body["response_format"]["type"].get<std::string>(), "json_object");
    REQUIRE_EQ(body["messages"][1]["content"].get<std::string>(), R"(["你好","世界"])");
    REQUIRE(!body.contains("temperature"));
    REQUIRE_EQ(body["reasoning_effort"].get<std::string>(), "low");
    REQUIRE(!body.contains("max_tokens"));
    REQUIRE(!body.contains("max_completion_tokens"));
    REQUIRE(!body.contains("thinking"));
    REQUIRE(AiTranslation::detail::RequestTimeoutMs(config) > 8000L);

    config.model = "gpt-4o-mini";
    const auto older_model = AiTranslation::detail::BuildRequestBody(config, {"你好"}, "zh", "en");
    REQUIRE_EQ(older_model["temperature"].get<double>(), 0.0);
    REQUIRE(!older_model.contains("reasoning_effort"));
    REQUIRE_EQ(AiTranslation::detail::RequestTimeoutMs(config), 8000L);
}

TEST_CASE(AiTranslationKeepsOtherProviderRequestShape)
{
    // 地址是本机服务上的 DeepSeek 兼容实现，靠标签认不出厂商，thinking 按接口地址判断。
    AiTranslation::Config config{"deepseek", "https://example.test", "sk-test", "deepseek-chat"};
    const auto unknown_host = AiTranslation::detail::BuildRequestBody(config, {"你好"}, "zh", "en");
    REQUIRE_EQ(unknown_host["temperature"].get<double>(), 0.0);
    REQUIRE(!unknown_host.contains("thinking"));

    config.endpoint = "https://api.deepseek.com/chat/completions";
    const auto deepseek = AiTranslation::detail::BuildRequestBody(config, {"你好"}, "zh", "en");
    REQUIRE_EQ(deepseek["temperature"].get<double>(), 0.0);
    REQUIRE_EQ(deepseek["thinking"]["type"].get<std::string>(), "disabled");
    REQUIRE(!deepseek.contains("max_tokens"));
    REQUIRE(!deepseek.contains("max_completion_tokens"));

    config.provider = "custom";
    config.endpoint = "https://example.test";
    config.model = "GPT-5.6-Luna";
    const auto custom = AiTranslation::detail::BuildRequestBody(config, {"你好"}, "zh", "en");
    REQUIRE_EQ(custom["temperature"].get<double>(), 0.0);
    REQUIRE(!custom.contains("thinking"));
}

TEST_CASE(AiTranslationFollowsJsonModeStrategy)
{
    const auto body_for = [](const std::string &mode, const std::string &endpoint) {
        AiTranslation::Config config{"custom", endpoint, "sk-test", "qwen3:8b", mode};
        return AiTranslation::detail::BuildRequestBody(config, {"你好"}, "zh", "en");
    };

    // 强制档不看地址，一律带上。
    REQUIRE(body_for("force", "http://127.0.0.1:11434/v1/chat/completions").contains("response_format"));
    // 兼容档不看地址，一律不带。
    REQUIRE(!body_for("off", "https://api.deepseek.com/chat/completions").contains("response_format"));
    // 自动档只对原生实现带上。
    REQUIRE(body_for("auto", "https://api.deepseek.com/chat/completions").contains("response_format"));
    REQUIRE(body_for("auto", "https://api.openai.com/v1/chat/completions").contains("response_format"));
    REQUIRE(!body_for("auto", "http://127.0.0.1:1234/v1/chat/completions").contains("response_format"));
    REQUIRE(!body_for("auto", "https://relay.example.com/v1/chat/completions").contains("response_format"));

    // 空串与未知取值按自动档处理，旧配置不会因为缺键就变回强制。
    REQUIRE(!body_for("", "http://localhost:8000/v1/chat/completions").contains("response_format"));
    REQUIRE(!body_for("nonsense", "localhost:8000/v1/chat/completions").contains("response_format"));
}

TEST_CASE(AiTranslationDetectsLocalEndpoints)
{
    for (const char *endpoint :
         {"http://localhost:11434/v1/chat/completions", "http://127.0.0.1:1234/v1/chat/completions",
          "http://[::1]:8080/v1/chat/completions", "http://192.168.1.20:8000/v1/chat/completions",
          "http://10.0.0.5:8000/v1/chat/completions", "http://172.16.0.9:8000/v1/chat/completions",
          "http://172.31.255.1:8000/v1/chat/completions"})
    {
        REQUIRE(ChatCompletions::IsLocalChatEndpoint(endpoint));
    }
    for (const char *endpoint :
         {"https://api.deepseek.com/chat/completions", "https://api.openai.com/v1/chat/completions",
          "http://172.32.0.9:8000/v1/chat/completions", "http://172.15.0.9:8000/v1/chat/completions",
          "https://notlocalhost.example.com/v1",
          // 数字开头的公网域名不能被前缀匹配误判成本机。
          "https://172.16.example.com/v1/chat/completions", "https://10.evil.com/v1/chat/completions",
          "https://127.evil.com/v1/chat/completions", "https://192.168.evil.com/v1/chat/completions",
          "http://172.16abc.example.com/v1/chat/completions", "http://127.0.0.1.example.com/v1/chat/completions",
          // 越界的点分段同样不合法。
          "http://300.1.1.1/v1/chat/completions"})
    {
        REQUIRE(!ChatCompletions::IsLocalChatEndpoint(endpoint));
    }
}

TEST_CASE(AiTranslationRejectsRepliesThatDoNotLineUp)
{
    // 合法 JSON 但没有约定的键：不能把整段回复当译文。
    for (const char *reply : {R"({"测试":"test"})", R"({"output":"hi"})", R"({"translations":"oops"})"})
    {
        const auto parsed = AiTranslation::ParseTranslations(reply, 1);
        REQUIRE_EQ(parsed.size(), 1u);
        REQUIRE(parsed[0].empty());
    }

    // 数组长度与请求不一致时整批作废，避免错位写进释义库。
    const auto short_array = AiTranslation::ParseTranslations(R"({"translations":["only"]})", 2);
    REQUIRE(short_array[0].empty());
    REQUIRE(short_array[1].empty());

    // 恰好 N 行的格式化 JSON 不能被逐行当成译文。
    const auto pretty = AiTranslation::ParseTranslations("{\n\"x\": 1\n}", 3);
    REQUIRE(pretty[0].empty());
    REQUIRE(pretty[1].empty());
    REQUIRE(pretty[2].empty());

    // 残缺 JSON 同理。
    const auto broken = AiTranslation::ParseTranslations("{\"translations\": [\"a\"", 1);
    REQUIRE(broken[0].empty());

    // 单个字符串字段只在请求一条时接受。
    const std::string single = R"({"translation":"hello"})";
    const auto one = AiTranslation::ParseTranslations(single, 1);
    REQUIRE_EQ(one[0], std::string("hello"));
    const auto two = AiTranslation::ParseTranslations(single, 2);
    REQUIRE(two[0].empty());

    // 无视 response_format、逐行作答的服务商仍然可用。
    const auto lines = AiTranslation::ParseTranslations("hello\nworld", 2);
    REQUIRE_EQ(lines[0], std::string("hello"));
    REQUIRE_EQ(lines[1], std::string("world"));
}

TEST_CASE(AiTranslationParsesJsonResponses)
{
    // 标准 JSON 对象包含 translations 数组
    const std::string standard = R"({"translations":["hello","world"]})";
    const auto r1 = AiTranslation::ParseTranslations(standard, 2);
    REQUIRE_EQ(r1.size(), 2u);
    REQUIRE_EQ(r1[0], std::string("hello"));
    REQUIRE_EQ(r1[1], std::string("world"));

    // 纯 JSON 数组
    const std::string raw_array = R"(["bonjour","monde"])";
    const auto r2 = AiTranslation::ParseTranslations(raw_array, 2);
    REQUIRE_EQ(r2.size(), 2u);
    REQUIRE_EQ(r2[0], std::string("bonjour"));
    REQUIRE_EQ(r2[1], std::string("monde"));

    // Markdown 代码块包裹
    const std::string fenced = "```json\n{\"translations\":[\"hola\"]}\n```";
    const auto r3 = AiTranslation::ParseTranslations(fenced, 1);
    REQUIRE_EQ(r3.size(), 1u);
    REQUIRE_EQ(r3[0], std::string("hola"));

    // 格式损坏或空响应安全返回空数组
    const auto r_empty = AiTranslation::ParseTranslations("not a json", 2);
    REQUIRE_EQ(r_empty.size(), 2u);
    REQUIRE(r_empty[0].empty());
    REQUIRE(r_empty[1].empty());
}

TEST_CASE(AiTranslationUsesOllamaWithoutToken)
{
    AiTranslation::Config ollama;
    ollama.provider = "ollama";
    ollama.endpoint = "http://localhost:11434/v1/chat/completions";
    ollama.model = "qwen3:8b";
    REQUIRE(AiTranslation::IsUsableConfig(ollama));

    // 配置里残留的旧 Token 不随 Ollama 的请求发出去。
    TranslationConfig snapshot;
    snapshot.ai = {"ollama", "http://localhost:11434/v1/chat/completions", "sk-left-over", "qwen3:8b"};
    const auto config = AiTranslation::FromConfiguredTranslation(snapshot);
    REQUIRE(config.token.empty());
    REQUIRE(AiTranslation::IsUsableConfig(config));

    // 其他服务商仍然要 Token。
    AiTranslation::Config deepseek = ollama;
    deepseek.provider = "deepseek";
    deepseek.endpoint = "https://api.deepseek.com/chat/completions";
    REQUIRE(!AiTranslation::IsUsableConfig(deepseek));
}
