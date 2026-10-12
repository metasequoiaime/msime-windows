#include "tests/includes/test_framework.h"

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
